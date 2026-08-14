#include "vm.h"

#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "vm/gc.h"
#include "vm/value.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <format>
#include <utility>

// NOTE: VM 不会校验编译模块的合法性, 假设所有输入的模块都是合法的, 任何不合预期的模块输入都使用断言终止

namespace Zeta {

VM::VM(Config config, ErrorHandler handler) : config(config), errorHandler(handler) {
    stack.base = static_cast<Value*>(std::malloc(config.stackSize * 1024));
    stack.capacity = config.stackSize * 1024 / sizeof(Value);
    stack.top = stack.base;
    if(!stack.base) {
        reportError("Failed to allocate initial stack", Error::Type::VMError);
        throw VMException(VMException::Type::OutOfMemory);
    }
    for(const auto& path : config.moduleSearchPaths) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::canonical(path, ec);
        if(ec) {
            reportError(std::format("Invalid module search path: \"{}\" <{}>", path, ec.message()), Error::Type::VMError);
        }
    }
    gc = std::make_unique<GC>(this);
    StackFrame mainFrame;
    mainFrame.routine = nullptr;
    mainFrame.base = stack.base;
    mainFrame.top = stack.top;
    mainFrame.ip = -1;
    stackFrames.push_back(mainFrame);
}

VM::~VM() {
    std::free(stack.base);
}

/*
同一个 VM 实例每个模块只会被加载一次, 所有模块共享运行时状态.

VM 可以显式加载多个模块, 在显式加载模块时会自动加载imports列表中未被加载的模块,
同时会处理模块内的外部符号重定位, 这个重定位只会依赖显式写在imports列表中的模块, 
也就是外部符号只会在imports列表中的模块中定义的全局符号中找.
比如模块A导入模块B, 模块B导入模块C, 模块A中只能使用模块B中的全局符号.

动态模块加载先不做, 如果最后有外部符号未处理直接报错.

对于未指定模块名的外部符号, 按照导入的顺序查找. 如果有多个模块都定义了该符号, 则使用第一个模块中的定义, 不建议开发者依赖这一特性, 这种情况应该使用别名来避免.
*/

// NOTE: import 应该写模块名而非路径 比如 import "/lib/zeta/math" 而不是 import "/lib/zeta/math.zt". 导入时会优先找 "lib/zeta/math.ztc", 再找 "lib/zeta/math.zt", 如果写的是"math.zt", 会找"math.zt.zt"

void VM::loadModule(const Module* module) {
    importModule(module);
}

int VM::registerFunction(const std::string& name, NativeFunction func) {
    String* nameStr = internString(name);
    uint32_t index = global.size();
    global.push_back(Value(func));
    registeredSyms[name] = index;
    return index;
}

int VM::registerClass(const std::string& name, const std::vector<std::pair<std::string, Value>>& fields, const std::vector<std::pair<std::string, NativeFunction>>& methods) {
    String* nameStr = internString(name);
    gc->lock();
    Map* fieldMap = gc->allocate<Map>(gc.get(), fields.size());
    for(const auto& [fieldName, fieldVal] : fields) {
        fieldMap->set(internString(fieldName), fieldVal);
    }
    Map* methodMap = gc->allocate<Map>(gc.get(), methods.size());
    for(const auto& [methodName, methodFunc] : methods) {
        methodMap->set(internString(methodName), Value(methodFunc));
    }
    Class* cls = gc->allocate<Class>(gc.get(), nameStr, nullptr, fieldMap, methodMap);
    uint32_t index = global.size();
    global.push_back(Value(cls));
    gc->unlock();
    registeredSyms[name] = index;
    return index;
}

// NOTE: 在当前帧上压入一个元素, 这可能会超出当前帧原定容量, 此时会自动扩展栈顶.
// 在没有外部干预的情况下, 函数栈帧都是一次性分配好的, 外部调用了 push() 才可能出现帧容量大于localCount + maxStackSize 的情况.
Value* VM::push(Value val) {
    auto& frame = stackFrames.back();
    Value* ret = frame.top++;
    if (frame.top > stack.top) {
        if (stack.top > stack.base + stack.capacity) {
            reportError("Stack overflow", Error::Type::VMError);
            throw VMException(VMException::Type::StackOverflow);
            return nullptr;
        }
        assert(frame.top == stack.top + 1);
        stack.top = frame.top;
    }
    *ret = val;
    return ret;
}

// NOTE: pop 过头会破坏栈结构, 调用方有责任保证安全性.
Value VM::pop() {
    return *(--stackFrames.back().top);
} 

// NOTE: offset <= -1. 修改不属于自己的元素是未定义行为, 也就是 offset 不能过小
Value* VM::peek(int offset) {
    assert(offset <= -1);
    return stackFrames.back().top + offset;
}

int VM::findGlobal(const std::string& moduleName, const std::string& globalName) {
    if(moduleName.empty()) {
        auto it = registeredSyms.find(globalName);
        if(it != registeredSyms.end()) {
            return it->second;
        }
    } else {
        auto it = loadedModules.find(moduleName);
        if(it != loadedModules.end()) {
            const ModuleInfo& moduleInfo = it->second;
            auto symIt = moduleInfo.symbolMap.find(globalName);
            if(symIt != moduleInfo.symbolMap.end()) {
                return symIt->second;
            }
        }
    }
    reportError(std::format("Global symbol \"{}\" not found in module \"{}\"", globalName, moduleName.empty() ? "<registered>" : moduleName), Error::Type::RuntimeError);
    return -1;
}

Value VM::getGlobal(int index) {
    if(index < 0 || index >= global.size()) {
        reportError(std::format("Global index {} out of bounds", index), Error::Type::RuntimeError);
        return Value::Error;
    }
    return global[index];
}

void VM::setGlobal(int index, Value val) {
    if(index < 0 || index >= global.size()) {
        reportError(std::format("Global index {} out of bounds", index), Error::Type::RuntimeError);
        return;
    }
    global[index] = val;
}

void VM::call(int argc) {
    Value funcVal = pop();
    if(funcVal.type == Value::Type::NativeFunc) {
        funcVal.nativeFuncValue(this, argc);
    } else if(funcVal.type == Value::Type::Function) {
        call(funcVal.funcValue, argc);
    } else {
        reportError("call() expects a function", Error::Type::RuntimeError);
        push(Value::Error);
    }
}

void VM::callMethod(const std::string& methodName, int argc) {
    String* methodNameStr = internString(methodName);
    callMethod(methodNameStr, argc);
}

void VM::callMethod(String* methodName, int argc) {
    Value objVal = pop();
    if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
        reportError("callMethod() expects an instance", Error::Type::RuntimeError);
        push(Value::Error);
        return;
    }
    Instance* inst = static_cast<Instance*>(objVal.ptrValue);
    Class* cls = inst->cls;
    while(cls) {
        auto methodValOpt = cls->methods->get(methodName);
        if(methodValOpt.has_value()) {
            Value methodVal = methodValOpt.value();
            if(methodVal.type == Value::Type::NativeFunc) {
                push(objVal);
                methodVal.nativeFuncValue(this, argc + 1); 
            } else if(methodVal.type == Value::Type::Function) {
                push(objVal);
                std::memmove(stackFrames.back().top - argc, stackFrames.back().top - argc - 1, argc * sizeof(Value));
                *(stackFrames.back().top - argc - 1) = objVal;
                call(methodVal.funcValue, argc + 1); 
            } else {
                reportError(std::format("CallMethod: method \"{}\" is not a function", methodName->data), Error::Type::RuntimeError);
                push(Value::Error);
                return;
            }
            return;
        }
        cls = cls->base;
    }
    reportError(std::format("CallMethod: method \"{}\" not found in class \"{}\" or its ancestors", methodName->data, inst->cls->name->data), Error::Type::RuntimeError);
    push(Value::Error);
}

void VM::newInstance(int argc) {
    Value classVal = pop();
    if(classVal.type != Value::Type::Object || classVal.ptrValue->type != Object::Type::Class) {
        reportError("newInstance() expects a Class object", Error::Type::RuntimeError);
        push(Value::Error);
        return;
    }
    gc->lock();
    Class* cls = static_cast<Class*>(classVal.ptrValue);
    Instance* instance = gc->allocate<Instance>(gc.get(), cls);
    // call constructor if exists
    auto ctorValOpt = cls->methods->get(STRINGS._init);
    if(ctorValOpt.has_value()) {
        push(Value(instance));
        gc->unlock();
        callMethod(STRINGS._init, argc);
    } else {
        push(Value(instance));
        gc->unlock();
    }
}

void VM::newArray() {
    gc->lock();
    Array* arr = gc->allocate<Array>(gc.get());
    push(Value(arr));
    gc->unlock();
}

void VM::newArray(int size) {
    gc->lock();
    Array* arr = gc->allocate<Array>(gc.get(), size);
    push(Value(arr));
    gc->unlock();
}

void VM::newMap() {
    gc->lock();
    Map* map = gc->allocate<Map>(gc.get());
    push(Value(map));
    gc->unlock();
}

void VM::newStrObj(std::string_view str) {
    gc->lock();
    StrObj* strObj = gc->allocate<StrObj>(gc.get(), str.data(), str.length());
    push(Value(strObj));
    gc->unlock();
}

void VM::wrapPointer(void* ptr, Value class_) {
    if(class_.type != Value::Type::Object || class_.ptrValue->type != Object::Type::Class) {
        reportError("wrapPointer() expects a Class object", Error::Type::RuntimeError);
        push(Value::Error);
        return;
    }
    gc->lock();
    Class* cls = static_cast<Class*>(class_.ptrValue);
    Instance* instance = gc->allocate<Instance>(gc.get(), cls);
    instance->fields->set(STRINGS._cpp_ptr, Value((int64_t)ptr));
    push(Value(instance));
    gc->unlock();
}

void* VM::unwrapPointer() {
    Value obj = pop();
    if(obj.type != Value::Type::Object || obj.ptrValue->type != Object::Type::Instance) {
        reportError("unwrapPointer() expects an Instance object", Error::Type::RuntimeError);
        return nullptr;
    }
    Instance* instance = static_cast<Instance*>(obj.ptrValue);
    auto ptrValOpt = instance->fields->get(STRINGS._cpp_ptr);
    if(!ptrValOpt.has_value() || ptrValOpt.value().type != Value::Type::Int) {
        reportError("unwrapPointer() expects an Instance with a _cpp_ptr field of type Int", Error::Type::RuntimeError);
        return nullptr;
    }
    return (void*)(ptrValOpt.value().intValue);
}

Value* VM::pushTempRoot() {
    tempRoots.push_back(std::make_unique<Value>(pop()));
    return tempRoots.back().get();
}

void VM::popTempRoot(Value* root) {
    auto it = std::find_if(tempRoots.begin(), tempRoots.end(), [root](const std::unique_ptr<Value>& ptr) {
        return ptr.get() == root;
    });
    if(it != tempRoots.end()) {
        std::swap(*it, tempRoots.back());
        tempRoots.pop_back();
    } else {
        reportError("popTempRoot() called on a non-temp-root", Error::Type::RuntimeError);
    }
}

void VM::importModule(const Module* module) {
    const std::string& moduleName = module->name; 
    if(loadedModules.find(moduleName) != loadedModules.end()) {
        return;
    }
    ModuleInfo& moduleInfo = loadedModules[moduleName];
    moduleInfo.protoBaseIndex = routines.size();

    struct BaseClassPatch{
        std::string className;
        std::pair<std::string, std::string> baseClassNames;
    };
    std::vector<BaseClassPatch> baseClassPatches;

    auto makeValue = [this, &moduleInfo, &baseClassPatches](auto&& self, const CompileValue& compileValue) {
        switch (compileValue.type) {
            case CompileValue::Type::Null: return Value();
            case CompileValue::Type::Int: return Value(compileValue.intValue);
            case CompileValue::Type::Float: return Value(compileValue.floatValue);
            case CompileValue::Type::Bool: return Value(compileValue.boolValue);
            case CompileValue::Type::String: return Value(internString(*(compileValue.strValue)));
            case CompileValue::Type::Array: {
                Array* arr = gc->allocate<Array>(gc.get(), compileValue.arrayValue->size());
                for(int i = 0; i < compileValue.arrayValue->size(); i++) {
                    arr->set(i, self(self, (*compileValue.arrayValue)[i]));
                }
                return Value(arr);
            }
            case CompileValue::Type::Map: {
                Map* map = gc->allocate<Map>(gc.get(), compileValue.mapValue->size());
                for(const auto& [key, val] : *compileValue.mapValue) {
                    map->set(internString(key), self(self, val));
                }
                return Value(map);
            }
            case CompileValue::Type::Function: {
                uint32_t routineIndex = moduleInfo.protoBaseIndex + compileValue.funcValue->protoIndex;
                assert(routineIndex < routines.size());
                Routine* func = routines[routineIndex].get();
                return Value(func);
            }
            case CompileValue::Type::Class: {
                String* name = internString(compileValue.classValue->name);
                if(!compileValue.classValue->base.second.empty()){
                    baseClassPatches.push_back({compileValue.classValue->name, compileValue.classValue->base});
                }
                Map* fields = gc->allocate<Map>(gc.get(), compileValue.classValue->fields.size());
                Map* methods = gc->allocate<Map>(gc.get(), compileValue.classValue->methods.size());
                for(const auto& [fieldName, fieldVal] : compileValue.classValue->fields) {
                    fields->set(internString(fieldName), self(self, fieldVal));
                }
                for(const auto& [methodName, methodVal] : compileValue.classValue->methods) {
                    uint32_t routineIndex = moduleInfo.protoBaseIndex + methodVal.protoIndex;
                    assert(routineIndex < routines.size());
                    Routine* func = routines[routineIndex].get();
                    methods->set(internString(methodName), Value(func));
                }
                Class* cls = gc->allocate<Class>(gc.get(), name, nullptr, fields, methods);
                return Value(cls);
            }
            default: assert(false);
        }
        return Value();
    };

    for(auto& proto : module->protos) {
        auto routine = std::make_unique<Routine>();
        routine->bytecode = proto->bytecode;
        routine->lineInfo = proto->lineInfo;
        routine->moduleName = moduleName;
        routine->arity = proto->arity;
        routine->localCount = proto->localCount;
        routine->maxStackSize = proto->maxStackSize;
        routines.push_back(std::move(routine));
    }
    for(int i = 0; i < module->protos.size(); i++) {
        auto& proto = module->protos[i];
        auto& routine = routines[moduleInfo.protoBaseIndex + i];
        for(const auto& constVal : proto->constants) {
            GCLockGuard lock(gc.get());
            routine->constants.push_back(makeValue(makeValue, constVal));
        }
    }

    for(const auto& [symName, sym] : module->globalSyms) {
        uint32_t index = global.size();
        GCLockGuard lock(gc.get());
        global.push_back(makeValue(makeValue, sym.initValue));
        moduleInfo.symbolMap[symName] = index;
        for(const auto& pos : sym.relocations) {
            uint32_t routineIndex = moduleInfo.getRoutineIndex(pos.protoIndex);
            uint32_t offset = pos.bytecodeOffset;
            assert(routineIndex < routines.size() && offset + 4 <= routines[routineIndex]->bytecode.size());
            std::memcpy(routines[routineIndex]->bytecode.data() + offset, &index, 4);
        }
    }
    std::vector<std::pair<std::string, std::string>> nameToFullname;
    for(const auto& import : module->imports) {
        auto importPath = searchModuleFile(moduleName, import);
        if(importPath.second.empty()) {
            reportError(std::format("Failed to find imported module: \"{}\" imported by \"{}\"", import, moduleName), Error::Type::VMError);
            return;
        }
        nameToFullname.push_back({import, std::filesystem::path(importPath.second).replace_extension("").string()});
        importModule(importPath.second, importPath.first);
    }
    for(const auto& extSym : module->externalSyms) {
        uint32_t index;
        if(extSym.moduleName.empty()) {
            bool found = false;
            for(const auto& [import, importPath] : nameToFullname) {
                const auto& symMap = loadedModules[importPath].symbolMap;
                auto symIt = symMap.find(extSym.name);
                if(symIt != symMap.end()) {
                    index = symIt->second;
                    found = true;
                    break;
                }
            }
            auto symIt = registeredSyms.find(extSym.name);
            if(symIt != registeredSyms.end()) {
                index = symIt->second;
                found = true;
            }
            if(!found) {
                reportError(std::format("Failed to resolve external symbol: \"{}\" in module \"{}\"", extSym.name, moduleName), Error::Type::VMError);
                return;
            }
        } else {
            auto it = std::find_if(nameToFullname.begin(), nameToFullname.end(), [&extSym](const std::pair<std::string, std::string>& p) {
                return p.first == extSym.moduleName;
            });
            assert(it != nameToFullname.end());
            const auto& symMap = loadedModules[it->second].symbolMap;
            auto symIt = symMap.find(extSym.name);
            if(symIt == symMap.end()) {
                reportError(std::format("Failed to resolve external symbol: \"{}\" in module \"{}\", module \"{}\" does not define the symbol", extSym.name, moduleName, extSym.moduleName), Error::Type::VMError);
                return;
            }
            index = symIt->second;
        }
        for(const auto& pos : extSym.relocations) {
            uint32_t routineIndex = moduleInfo.getRoutineIndex(pos.protoIndex);
            uint32_t offset = pos.bytecodeOffset;
            assert(routineIndex < routines.size() && offset + 4 <= routines[routineIndex]->bytecode.size());
            std::memcpy(routines[routineIndex]->bytecode.data() + offset, &index, 4);
        }
    }  
    // base class patch
    for(const auto& patch : baseClassPatches) {
        uint32_t idx = moduleInfo.symbolMap[patch.className];
        Value& classVal = global[idx];
        assert(classVal.type == Value::Type::Object && classVal.ptrValue->type == Object::Type::Class);
        Class* cls = static_cast<Class*>(classVal.ptrValue);
        uint32_t baseIdx;
        if(patch.baseClassNames.first.empty()) {
            auto it = moduleInfo.symbolMap.find(patch.baseClassNames.second);
            if(it != moduleInfo.symbolMap.end()) {
                baseIdx = it->second;
            } else {
                bool found = false;
                for(const auto& [import, importPath] : nameToFullname) {
                    const auto& symMap = loadedModules[importPath].symbolMap;
                    auto symIt = symMap.find(patch.baseClassNames.second);
                    if(symIt != symMap.end()) {
                        baseIdx = symIt->second;
                        found = true;
                        break;
                    }
                }
                auto symIt = registeredSyms.find(patch.baseClassNames.second);
                if(symIt != registeredSyms.end()) {
                    baseIdx = symIt->second;
                    found = true;
                }
                if(!found) {
                    reportError(std::format("Failed to resolve base class: \"{}\" for class \"{}\" in module \"{}\"", patch.baseClassNames.second, patch.className, moduleName), Error::Type::VMError);
                    return;
                }
            }
        } else {
            auto it = std::find_if(nameToFullname.begin(), nameToFullname.end(), [&patch](const std::pair<std::string, std::string>& p) {
                return p.first == patch.baseClassNames.first;
            });
            assert(it != nameToFullname.end());
            const auto& symMap = loadedModules[it->second].symbolMap;
            auto symIt = symMap.find(patch.baseClassNames.second);
            if(symIt == symMap.end()) {
                reportError(std::format("Failed to resolve base class: \"{}\" for class \"{}\" in module \"{}\", module \"{}\" does not define the class", patch.baseClassNames.second, patch.className, moduleName, patch.baseClassNames.first), Error::Type::VMError);
                return;
            }
            baseIdx = symIt->second;
        }
        Value& baseVal = global[baseIdx];
        assert(baseVal.type == Value::Type::Object && baseVal.ptrValue->type == Object::Type::Class);
        gc->writeBarrier(cls, (Object**)(&cls->base), baseVal.ptrValue);
    }
    // bind each method routine to the class it is defined in, so that SuperCall
    // can resolve from the lexically containing class instead of the instance's class
    for(const auto& [symName, sym] : module->globalSyms) {
        auto it = moduleInfo.symbolMap.find(symName);
        if(it == moduleInfo.symbolMap.end()) continue;
        Value& classVal = global[it->second];
        if(classVal.type != Value::Type::Object || classVal.ptrValue->type != Object::Type::Class) continue;
        Class* cls = static_cast<Class*>(classVal.ptrValue);
        cls->methods->forEach([cls](const String*, Value value) {
            if(value.type == Value::Type::Function) {
                value.funcValue->ownerClass = cls;
            }
        });
    }
}

void VM::importModule(const std::filesystem::path& path, bool isSrcFile) {
    std::unique_ptr<Module> module;
    std::string error;
    if(isSrcFile){
        module = compileModule(path.string(), &error);
        if(!module) {
            reportError(std::format("Failed to compile module from source file {}: {}", path.string(), error), Error::Type::VMError);
            return;
        }
    } else {
        module = deserializeModule(path.string(), &error);
        if(!module) {
            reportError(std::format("Failed to deserialize module from {}: {}", path.string(), error), Error::Type::VMError);
            return;
        }
    }
    importModule(module.get());
}

std::pair<bool, std::filesystem::path> VM::searchModuleFile(const std::filesystem::path& basePath, const std::string& moduleName) {
    auto tryFindFile = [](const std::filesystem::path& path) -> std::pair<bool, std::filesystem::path> {
        auto bcPath = std::filesystem::path(path).concat(ZETA_BC_EXT);
        if(std::filesystem::exists(bcPath) && std::filesystem::is_regular_file(bcPath)) {
            return {false, bcPath};
        }
        auto srcPath = std::filesystem::path(path).concat(ZETA_SRC_EXT);
        if(std::filesystem::exists(srcPath) && std::filesystem::is_regular_file(srcPath)) {
            return {true, srcPath};
        }
        return {false, std::filesystem::path()};
    };

    // 1. based on the directory of the importing module
    std::filesystem::path candidate = (basePath.parent_path() / moduleName).lexically_normal();
    auto result = tryFindFile(candidate);
    if(!result.second.empty()) {
        return result;
    }
    // 2. absolute path or relative to current working directory
    candidate = std::filesystem::path(moduleName);
    if(candidate.is_absolute()) {
        result = tryFindFile(candidate);
        if(!result.second.empty()) {
            return result;
        }
    } else {
        candidate = (std::filesystem::current_path() / candidate).lexically_normal();
        result = tryFindFile(candidate);
        if(!result.second.empty()) {
            return result;
        }
    }
    // 3. search in module search paths
    for(const auto& searchPath : config.moduleSearchPaths) {
        candidate = (std::filesystem::path(searchPath) / moduleName).lexically_normal();
        result = tryFindFile(candidate);
        if(!result.second.empty()) {
            return result;
        }
    }
    return {};
}

void VM::call(Routine* func, int argc) {
    if(argc != func->arity) {
        reportError(std::format("Function expects {} arguments, but {} were provided", func->arity, argc), Error::Type::RuntimeError);
        push(Value::Error);
        return;
    }
    StackFrame frame;
    frame.routine = func;
    frame.base = stack.top;
    frame.top = stack.top + func->localCount;
    frame.ip = 0;
    StackFrame* curFrame = pushFrame(frame);
    StackFrame* prevFrame = curFrame - 1;
    std::memcpy(curFrame->base, prevFrame->top - argc, argc * sizeof(Value));
    prevFrame->top -= argc;
    execute();
}

// TODO: 需要重构
void VM::execute() {
    StackFrame* curFrame = &stackFrames.back();
    Routine* curRoutine = curFrame->routine;
    int initDepth = stackFrames.size();

    #define READ_BYTE(val) (val = curRoutine->bytecode[curFrame->ip++])
    #define READ_UINT32(val) { std::memcpy(&val, &(curRoutine->bytecode[curFrame->ip]), 4); curFrame->ip += 4; } 
    #define PUSH(val) (*curFrame->top++ = val)
    #define POP() (*--curFrame->top)

    while(true){
        uint8_t opcode;
        READ_BYTE(opcode);
        switch(static_cast<Opcode>(opcode)) {
            case Opcode::Nop: break;
            case Opcode::LoadConst: {
                uint32_t constIndex;
                READ_UINT32(constIndex);
                assert(constIndex < curRoutine->constants.size());
                PUSH(curRoutine->constants[constIndex]);
                break;
            }
            case Opcode::LoadGlobal: {
                uint32_t globalIndex;
                READ_UINT32(globalIndex);
                assert(globalIndex < global.size());
                PUSH(global[globalIndex]);
                break;
            }
            case Opcode::StoreGlobal: {
                uint32_t globalIndex;
                READ_UINT32(globalIndex);
                assert(globalIndex < global.size());
                global[globalIndex] = POP();
                break;
            }
            case Opcode::LoadVar: {
                uint32_t localIndex;
                READ_UINT32(localIndex);
                assert(localIndex < curRoutine->localCount);
                PUSH(curFrame->base[localIndex]);
                break;
            }
            case Opcode::StoreVar: {
                uint32_t localIndex;
                READ_UINT32(localIndex);
                assert(localIndex < curRoutine->localCount);
                curFrame->base[localIndex] = POP();
                break;
            }
            case Opcode::Add: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int) {
                    if(b.type == Value::Type::Int) {
                        PUSH(Value(a.intValue + b.intValue));
                        break;
                    } else if(b.type == Value::Type::Float) {
                        PUSH(Value(static_cast<double>(a.intValue) + b.floatValue));
                        break;
                    }
                } else if(a.type == Value::Type::Float) {
                    if(b.type == Value::Type::Int) {
                        PUSH(Value(a.floatValue + static_cast<double>(b.intValue)));
                        break;
                    } else if(b.type == Value::Type::Float) {
                        PUSH(Value(a.floatValue + b.floatValue));
                        break;
                    }
                } else if(a.isString() && b.isString()) {
                    GCLockGuard lock(gc.get());
                    StrObj* str = gc->allocate<StrObj>(gc.get(), a.asString(), b.asString());
                    PUSH(Value(str));
                    break;
                }
                reportError("Unsupported operand types for Add", Error::Type::RuntimeError);
                PUSH(Value::Error);
                return;
            }
            case Opcode::Sub: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue - b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue - b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) - b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue - static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Sub", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Mul: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue * b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue * b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) * b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue * static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Mul", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Div: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    if(b.intValue == 0) {
                        reportError("Division by zero", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    PUSH(Value(a.intValue / b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue / b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) / b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue / static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Div", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Mod: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    if(b.intValue == 0) {
                        reportError("Modulo by zero", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    PUSH(Value(a.intValue % b.intValue));
                } else {
                    reportError("Unsupported operand types for Mod", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Neg: {
                Value a = POP();
                if(a.type == Value::Type::Int) {
                    PUSH(Value(-a.intValue));
                } else if(a.type == Value::Type::Float) {
                    PUSH(Value(-a.floatValue));
                } else {
                    reportError("Unsupported operand type for Neg", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::BitAnd: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue & b.intValue));
                } else {
                    reportError("Unsupported operand types for BitAnd", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::BitOr: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue | b.intValue));
                } else {
                    reportError("Unsupported operand types for BitOr", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::BitXor: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue ^ b.intValue));
                } else {
                    reportError("Unsupported operand types for BitXor", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::BitNot: {
                Value a = POP();
                if(a.type == Value::Type::Int) {
                    PUSH(Value(~a.intValue));
                } else {
                    reportError("Unsupported operand type for BitNot", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Shl: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue << b.intValue));
                } else {
                    reportError("Unsupported operand types for Shl", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Shr: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue >> b.intValue));
                } else {
                    reportError("Unsupported operand types for Shr", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Not: {
                Value a = POP();
                PUSH(Value(!static_cast<bool>(a)));
                break;
            }
            case Opcode::Eq: {
                Value b = POP();
                Value a = POP();
                if(a == b) {
                    PUSH(Value(true));
                    break;
                }
                switch(a.type) {
                    case Value::Type::Null: {
                        PUSH(Value(b.type == Value::Type::Null));
                        break;
                    }
                    case Value::Type::Int: {
                        if(b.type == Value::Type::Int) {
                            PUSH(Value(a.intValue == b.intValue));
                        } else if(b.type == Value::Type::Float) {
                            PUSH(Value(static_cast<double>(a.intValue) == b.floatValue));
                        } else {
                            PUSH(Value(false));
                        }
                        break;
                    }
                    case Value::Type::Float: {
                        if(b.type == Value::Type::Float) {
                            PUSH(Value(a.floatValue == b.floatValue));
                        } else if(b.type == Value::Type::Int) {
                            PUSH(Value(a.floatValue == static_cast<double>(b.intValue)));
                        } else {
                            PUSH(Value(false));
                        }
                        break;
                    }
                    case Value::Type::Bool: {
                        PUSH(Value(b.type == Value::Type::Bool && a.boolValue == b.boolValue));
                        break;
                    }
                    case Value::Type::String: {
                        if(b.type == Value::Type::String) {
                            PUSH(Value(a.ptrValue == b.ptrValue));
                        } else if(b.type == Value::Type::Object && b.ptrValue->type == Object::Type::StrObj) {
                            PUSH(Value(a.strValue->length == static_cast<StrObj*>(b.ptrValue)->length && std::memcmp(a.strValue->data, static_cast<StrObj*>(b.ptrValue)->data->getData(), a.strValue->length) == 0));
                        } else {
                            PUSH(Value(false));
                        }
                        break;
                    }
                    case Value::Type::Object: {
                        Object* aObj = a.ptrValue;
                        if(aObj->type == Object::Type::Instance) {
                            Instance* aInst = static_cast<Instance*>(aObj);
                            auto equalsMethodOpt = aInst->cls->methods->get(STRINGS._equals);
                            if(!equalsMethodOpt.has_value()) {
                                PUSH(Value(false));
                            } else {
                                Value equalsMethodVal = equalsMethodOpt.value();
                                assert(equalsMethodVal.type == Value::Type::Function);
                                Routine* func = equalsMethodVal.funcValue;
                                if(func->arity != 2) {
                                    reportError(std::format("Eq: _equals method of class \"{}\" should have 2 arguments", aInst->cls->name->data), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                PUSH(a);
                                PUSH(b);
                                call(func, 2);
                                Value ret = POP();
                                if(ret.type != Value::Type::Bool) {
                                    reportError(std::format("Eq: _equals method of class \"{}\" should return a boolean value", aInst->cls->name->data), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                PUSH(ret);
                            }
                        } else if (aObj->type == Object::Type::StrObj) {
                            if(b.type == Value::Type::String) {
                                PUSH(Value(static_cast<StrObj*>(aObj)->length == b.strValue->length && std::memcmp(static_cast<StrObj*>(aObj)->data->getData(), b.strValue->data, b.strValue->length) == 0));
                            } else if(b.type == Value::Type::Object && b.ptrValue->type == Object::Type::StrObj) {
                                PUSH(Value(static_cast<StrObj*>(aObj)->length == static_cast<StrObj*>(b.ptrValue)->length && std::memcmp(static_cast<StrObj*>(aObj)->data->getData(), static_cast<StrObj*>(b.ptrValue)->data->getData(), static_cast<StrObj*>(aObj)->length) == 0));
                            } else {
                                PUSH(Value(false));
                            }
                        } else {
                            PUSH(Value(b.type == Value::Type::Object && a.ptrValue == b.ptrValue));
                        }
                        break;
                    }
                    case Value::Type::Error: {
                        PUSH(Value(b.type == Value::Type::Error));
                        break;
                    }
                    default: assert(false);
                }
                break;
            }
            case Opcode::Neq: {
                Value b = POP();
                Value a = POP();
                if(a == b) {
                    PUSH(Value(false));
                    break;
                }
                switch(a.type) {
                    case Value::Type::Null: {
                        PUSH(Value(b.type != Value::Type::Null));
                        break;
                    }
                    case Value::Type::Int: {
                        if(b.type == Value::Type::Int) {
                            PUSH(Value(a.intValue != b.intValue));
                        } else if(b.type == Value::Type::Float) {
                            PUSH(Value(static_cast<double>(a.intValue) != b.floatValue));
                        } else {
                            PUSH(Value(true));
                        }
                        break;
                    }
                    case Value::Type::Float: {
                        if(b.type == Value::Type::Float) {
                            PUSH(Value(a.floatValue != b.floatValue));
                        } else if(b.type == Value::Type::Int) {
                            PUSH(Value(a.floatValue != static_cast<double>(b.intValue)));
                        } else {
                            PUSH(Value(true));
                        }
                        break;
                    }
                    case Value::Type::Bool: {
                        PUSH(Value(!(b.type == Value::Type::Bool && a.boolValue == b.boolValue)));
                        break;
                    }
                    case Value::Type::String: {
                        if(b.type == Value::Type::String) {
                            PUSH(Value(!(a.ptrValue == b.ptrValue)));
                        } else if(b.type == Value::Type::Object && b.ptrValue->type == Object::Type::StrObj) {
                            PUSH(Value(!(a.strValue->length == static_cast<StrObj*>(b.ptrValue)->length && std::memcmp(a.strValue->data, static_cast<StrObj*>(b.ptrValue)->data->getData(), a.strValue->length) == 0)));
                        } else {
                            PUSH(Value(true));
                        }
                        break;
                    }
                    case Value::Type::Object: {
                        Object* aObj = a.ptrValue;
                        if(aObj->type == Object::Type::Instance) {
                            Instance* aInst = static_cast<Instance*>(aObj);
                            auto equalsMethodOpt = aInst->cls->methods->get(STRINGS._equals);
                            if(!equalsMethodOpt.has_value()) {
                                PUSH(Value(true));
                            } else {
                                Value equalsMethodVal = equalsMethodOpt.value();
                                assert(equalsMethodVal.type == Value::Type::Function);
                                Routine* func = equalsMethodVal.funcValue;
                                if(func->arity != 2) {
                                    reportError(std::format("Neq: _equals method of class \"{}\" should have 2 arguments", aInst->cls->name->data), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                PUSH(a);
                                PUSH(b);
                                call(func, 2);
                                Value ret = POP();
                                if(ret.type != Value::Type::Bool) {
                                    reportError(std::format("Neq: _equals method of class \"{}\" should return a boolean value", aInst->cls->name->data), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                PUSH(Value(!ret.boolValue));
                            }
                        } else if (aObj->type == Object::Type::StrObj) {
                            if(b.type == Value::Type::String) {
                                PUSH(Value(!(static_cast<StrObj*>(aObj)->length == b.strValue->length && std::memcmp(static_cast<StrObj*>(aObj)->data->getData(), b.strValue->data, b.strValue->length) == 0)));
                            } else if(b.type == Value::Type::Object && b.ptrValue->type == Object::Type::StrObj) {
                                PUSH(Value(!(static_cast<StrObj*>(aObj)->length == static_cast<StrObj*>(b.ptrValue)->length && std::memcmp(static_cast<StrObj*>(aObj)->data->getData(), static_cast<StrObj*>(b.ptrValue)->data->getData(), static_cast<StrObj*>(aObj)->length) == 0)));
                            } else {
                                PUSH(Value(true));
                            }
                        } else {
                            PUSH(Value(!(b.type == Value::Type::Object && a.ptrValue == b.ptrValue)));
                        }
                        break;
                    }
                    case Value::Type::Error: {
                        PUSH(Value(b.type != Value::Type::Error));
                        break;
                    }
                    default: assert(false);
                }
                break;
            }
            case Opcode::Lt: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue < b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue < b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) < b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue < static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Lt", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Gt: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue > b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue > b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) > b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue > static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Gt", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Le: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue <= b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue <= b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) <= b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue <= static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Le", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Ge: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue >= b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue >= b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) >= b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue >= static_cast<double>(b.intValue)));
                } else {
                    reportError("Unsupported operand types for Ge", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Is: {
                Value b = POP();
                Value a = POP();
                PUSH(Value(a == b));
                break;
            }
            case Opcode::Jump: {
                uint32_t offset;
                READ_UINT32(offset);
                curFrame->ip = offset;
                break;
            }
            case Opcode::JumpIfFalse: {
                uint32_t offset;
                READ_UINT32(offset);
                Value cond = POP();
                if(!static_cast<bool>(cond)) {
                    curFrame->ip = offset;
                }
                break;
            }
            case Opcode::JumpIfTrue: {
                uint32_t offset;
                READ_UINT32(offset);
                Value cond = POP();
                if(static_cast<bool>(cond)) {
                    curFrame->ip = offset;
                }
                break;
            }
            case Opcode::Ret:{
                Value retVal = POP();
                assert(curFrame->top == curFrame->base + curRoutine->localCount);
                popFrame();
                curFrame = &stackFrames.back();
                curRoutine = curFrame->routine;
                PUSH(retVal);
                if(stackFrames.size() == initDepth - 1) {
                    return;
                }
                break;
            }
            case Opcode::Call: {
                uint8_t argCount;
                READ_BYTE(argCount);
                Value callee = POP();
                if(callee.type == Value::Type::Function) {
                    Routine* routine = callee.funcValue;
                    StackFrame newFrame;
                    newFrame.routine = routine;
                    newFrame.base = stack.top;
                    newFrame.top = newFrame.base + routine->localCount;
                    newFrame.ip = 0;
                    if(argCount != routine->arity) {
                        reportError(std::format("Function expects {} arguments, but {} were provided", routine->arity, argCount), Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    StackFrame* frame = pushFrame(newFrame);
                    curFrame = frame - 1;
                    for(int i = routine->arity - 1; i >= 0; i--) {
                        newFrame.base[i] = POP();
                    }
                    curFrame = frame;
                    curRoutine = curFrame->routine;
                } else if(callee.type == Value::Type::NativeFunc) {
                    NativeFunction nativeFunc = callee.nativeFuncValue;
                    nativeFunc(this, argCount);
                } else if(callee.type == Value::Type::Object && callee.ptrValue->type == Object::Type::Class) {
                    GCLockGuard lock(gc.get());
                    Class* cls = static_cast<Class*>(callee.ptrValue);
                    Instance* instance = gc->allocate<Instance>(gc.get(), cls);
                    auto constructor = cls->methods->get(STRINGS._init);
                    if(constructor.has_value()) {
                        Value constructorVal = constructor.value();
                        assert(constructorVal.type == Value::Type::Function);
                        Routine* routine = constructorVal.funcValue;
                        StackFrame newFrame;
                        newFrame.routine = routine;
                        newFrame.base = stack.top;
                        newFrame.top = newFrame.base + routine->localCount;
                        newFrame.ip = 0;
                        if(argCount != routine->arity - 1) {
                            reportError(std::format("Constructor expects {} arguments, but {} were provided", routine->arity - 1, argCount), Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        StackFrame* frame = pushFrame(newFrame);
                        curFrame = frame - 1;
                        newFrame.base[0] = Value(instance); // push 'this' as the first argument
                        for(int i = routine->arity - 1; i >= 1; i--) {
                            newFrame.base[i] = POP();
                        }
                        curFrame = frame;
                        curRoutine = curFrame->routine;
                    } else {
                        PUSH(Value(instance));
                    }
                } else {
                    reportError("Call: callee must be a function", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::GetField: {
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    reportError("GetField: object must be a class instance", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* fieldName = nameVal.strValue;
                auto fieldValOpt = instance->fields->get(fieldName);
                if(!fieldValOpt.has_value()) {
                    reportError(std::format("GetField: field \"{}\" not found in instance of class \"{}\"", fieldName->data, instance->cls->name->data), Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                PUSH(fieldValOpt.value());
                break;
            }
            case Opcode::SetField: {
                Value fieldVal = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    reportError("SetField: object must be a class instance", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* fieldName = nameVal.strValue;
                GCLockGuard lock(gc.get());
                instance->fields->set(fieldName, fieldVal);
                break;
            }
            case Opcode::CallMethod: {
                uint8_t argCount;
                READ_BYTE(argCount);
                Value objVal = POP();
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* methodName = nameVal.strValue;
                if(objVal.type == Value::Type::String) {
                    if(methodName == STRINGS.len){
                        if(argCount != 0) {
                            reportError(std::format("Method \"len\" expects 0 arguments, but {} were provided", argCount), Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        PUSH(Value(static_cast<int64_t>(objVal.strValue->length)));
                        break;
                    } else {
                        reportError(std::format("CallMethod: string does not have method \"{}\"", methodName->data), Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                } else if(objVal.type == Value::Type::Object) {
                    Object* obj = objVal.ptrValue;
                    switch(obj->type) {
                        case Object::Type::Array: {
                            if(methodName == STRINGS.size) {
                                if(argCount != 0) {
                                    reportError(std::format("Method \"size\" expects 0 arguments, but {} were provided", argCount), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                Array* arr = static_cast<Array*>(obj);
                                PUSH(Value(static_cast<int64_t>(arr->size)));
                                break;
                            } else if (methodName == STRINGS.add) {
                                GCLockGuard lock(gc.get());
                                if(argCount != 1) {
                                    reportError(std::format("Method \"add\" expects 1 argument, but {} were provided", argCount), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                Array* arr = static_cast<Array*>(obj);
                                Value valueToAdd = POP();
                                arr->add(valueToAdd);
                                PUSH(Value::Null);
                                break;
                            } else {
                                reportError(std::format("CallMethod: array does not have method \"{}\"", methodName->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                        }
                        case Object::Type::Map: {
                            if(methodName == STRINGS.size) {
                                if(argCount != 0) {
                                    reportError(std::format("Method \"size\" expects 0 arguments, but {} were provided", argCount), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                Map* map = static_cast<Map*>(obj);
                                PUSH(Value(static_cast<int64_t>(map->size)));
                                break;
                            } else {
                                reportError(std::format("CallMethod: map does not have method \"{}\"", methodName->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                        }
                        case Object::Type::StrObj: {
                            if(methodName == STRINGS.len) {
                                if(argCount != 0) {
                                    reportError(std::format("Method \"len\" expects 0 arguments, but {} were provided", argCount), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                StrObj* strObj = static_cast<StrObj*>(obj);
                                PUSH(Value(static_cast<int64_t>(strObj->length)));
                                break;
                            } else {
                                reportError(std::format("CallMethod: string object does not have method \"{}\"", methodName->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                        }
                        case Object::Type::Instance: {
                            Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                            Class* cls = instance->cls;
                            std::optional<Value> methodValOpt;
                            while(cls) {
                                methodValOpt = cls->methods->get(methodName);
                                if(methodValOpt.has_value()) {
                                    break;
                                }
                                cls = cls->base;
                            }
                            if(!methodValOpt.has_value()) {
                                reportError(std::format("CallMethod: method \"{}\" not found in class \"{}\" or its superclasses", methodName->data, instance->cls->name->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            Value methodVal = methodValOpt.value();
                            if (methodVal.type == Value::Type::Function) {
                                Routine* routine = methodVal.funcValue;
                                StackFrame newFrame;
                                newFrame.routine = routine;
                                newFrame.base = stack.top;
                                newFrame.top = newFrame.base + routine->localCount;
                                newFrame.ip = 0;
                                if(argCount != routine->arity - 1) {
                                    reportError(std::format("Method expects {} arguments, but {} were provided", routine->arity - 1, argCount), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                StackFrame* frame = pushFrame(newFrame);
                                curFrame = frame - 1;
                                newFrame.base[0] = objVal; // push 'this' as the first argument
                                for(int i = routine->arity - 1; i >= 1; i--) {
                                    newFrame.base[i] = POP();
                                }
                                curFrame = frame;
                                curRoutine = curFrame->routine;
                            } else if (methodVal.type == Value::Type::NativeFunc) {
                                NativeFunction nativeFunc = methodVal.nativeFuncValue;
                                PUSH(objVal);
                                nativeFunc(this, argCount + 1); 
                            } else {
                                assert(false);
                                reportError(std::format("CallMethod: method \"{}\" is not a function", methodName->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            break;
                        }
                        case Object::Type::Class: {
                            Class* cls = static_cast<Class*>(objVal.ptrValue);
                            auto methodValOpt = cls->methods->get(methodName);
                            if(!methodValOpt.has_value()) {
                                reportError(std::format("CallMethod: method \"{}\" not found in class \"{}\"", methodName->data, static_cast<Class*>(objVal.ptrValue)->name->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            Value methodVal = methodValOpt.value();
                            if (methodVal.type == Value::Type::Function) {
                                Routine* routine = methodVal.funcValue;
                                StackFrame newFrame;
                                newFrame.routine = routine;
                                newFrame.base = stack.top;
                                newFrame.top = newFrame.base + routine->localCount;
                                newFrame.ip = 0;
                                if(argCount != routine->arity) {
                                    reportError(std::format("Method expects {} arguments (the first is 'this'), but {} were provided", routine->arity, argCount), Error::Type::RuntimeError);
                                    PUSH(Value::Error);
                                    return;
                                }
                                StackFrame* frame = pushFrame(newFrame);
                                curFrame = frame - 1;
                                for(int i = routine->arity - 1; i >= 0; i--) {
                                    newFrame.base[i] = POP();
                                }
                                curFrame = frame;
                                curRoutine = curFrame->routine;
                            } else if (methodVal.type == Value::Type::NativeFunc) {
                                NativeFunction nativeFunc = methodVal.nativeFuncValue;
                                Value instanceVal = *(curFrame->top - argCount);
                                std::memmove(curFrame->top - argCount, curFrame->top - argCount + 1, (argCount - 1) * sizeof(Value));
                                *(curFrame->top - 1) = instanceVal;
                                nativeFunc(this, argCount); 
                            } else {
                                assert(false);
                                reportError(std::format("CallMethod: method \"{}\" is not a function", methodName->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            break;
                        }
                        default: {
                            reportError("CallMethod: object must be an array, map, string or class instance", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                    }
                }
                break;
            }
            case Opcode::SuperCall: {
                uint8_t argCount;
                READ_BYTE(argCount);
                Value objVal = POP();
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* methodName = nameVal.strValue;
                assert(objVal.type == Value::Type::Object && objVal.ptrValue->type == Object::Type::Instance);
                Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                Class* superClass;
                if(curRoutine->ownerClass) {
                    superClass = curRoutine->ownerClass->base;
                } else {
                    assert(false);
                }
                std::optional<Value> methodValOpt;
                while(superClass) {
                    methodValOpt = superClass->methods->get(methodName);
                    if(methodValOpt.has_value()) {
                        break;
                    }
                    superClass = superClass->base;
                }
                if(!methodValOpt.has_value()) {
                    reportError(std::format("SuperCall: method \"{}\" not found in superclass of class \"{}\"", methodName->data, instance->cls->name->data), Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                Value methodVal = methodValOpt.value();
                if (methodVal.type == Value::Type::Function) {
                    Routine* routine = methodVal.funcValue;
                    StackFrame newFrame;
                    newFrame.routine = routine;
                    newFrame.base = stack.top;
                    newFrame.top = newFrame.base + routine->localCount;
                    newFrame.ip = 0;
                    if(argCount != routine->arity - 1) {
                        reportError(std::format("Super method expects {} arguments, but {} were provided", routine->arity - 1, argCount), Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    StackFrame* frame = pushFrame(newFrame);
                    curFrame = frame - 1;
                    newFrame.base[0] = objVal; // push 'this' as the first argument
                    for(int i = routine->arity - 1; i >= 1; i--) {
                        newFrame.base[i] = POP();
                    }
                    curFrame = frame;
                    curRoutine = curFrame->routine;
                } else if (methodVal.type == Value::Type::NativeFunc) {
                    NativeFunction nativeFunc = methodVal.nativeFuncValue;
                    PUSH(objVal);
                    nativeFunc(this, argCount + 1); 
                } else {
                    assert(false);
                    reportError(std::format("SuperCall: method \"{}\" is not a function", methodName->data), Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::IndexGet: {
                Value index = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object) {
                    reportError("IndexGet: object must be an array or map", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                if(objVal.ptrValue->type == Object::Type::Array) {
                    Array* arr = static_cast<Array*>(objVal.ptrValue);
                    if(index.type != Value::Type::Int) {
                        reportError("IndexGet: array index must be an integer", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    int64_t idx = index.intValue;
                    if(idx < 0 || idx >= arr->size) {
                        reportError("IndexGet: array index out of bounds", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    PUSH(arr->get(idx));
                } else if(objVal.ptrValue->type == Object::Type::Map) {
                    Map* map = static_cast<Map*>(objVal.ptrValue);
                    // NOTE: map 只能使用常驻字符串来访问, 对象型字符串必须调用内置函数 intern 驻留后访问.
                    if(index.type != Value::Type::String) {
                        reportError("IndexGet: map key must be a string", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    auto valOpt = map->get(index.strValue);
                    if(!valOpt.has_value()) {
                        reportError(std::format("IndexGet: key \"{}\" not found in map", index.strValue->data), Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    PUSH(valOpt.value());
                } else {
                    reportError("IndexGet: object must be an array or map", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::IndexSet: {
                GCLockGuard lock(gc.get());
                Value value = POP();
                Value index = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object) {
                    reportError("IndexSet: object must be an array or map", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                if(objVal.ptrValue->type == Object::Type::Array) {
                    Array* arr = static_cast<Array*>(objVal.ptrValue);
                    if(index.type != Value::Type::Int) {
                        reportError("IndexSet: array index must be an integer", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    int64_t idx = index.intValue;
                    if(idx < 0 || idx >= arr->size) {
                        reportError("IndexSet: array index out of bounds", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    arr->set(idx, value);
                } else if(objVal.ptrValue->type == Object::Type::Map) {
                    Map* map = static_cast<Map*>(objVal.ptrValue);
                    if(index.type != Value::Type::String) {
                        reportError("IndexSet: map key must be a string", Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                    map->set(index.strValue, value);
                } else {
                    reportError("IndexSet: object must be an array or map", Error::Type::RuntimeError);
                    PUSH(Value::Error);
                    return;
                }
                break;
            }
            case Opcode::Pop: {
                POP();
                break;
            }
            case Opcode::Dup: {
                PUSH(*(curFrame->top - 1));
                break;
            }
            // TODO: 内置函数内部错误有些是可恢复的, 应该返回错误码而不是报运行时错误.
            case Opcode::CallBuiltin: {
                GCLockGuard lock(gc.get());
                uint8_t builtinId;
                READ_BYTE(builtinId);
                switch(static_cast<Builtin>(builtinId)) {
                    case Builtin::GetIter: {
                        Value objVal = POP();
                        if(objVal.type != Value::Type::Object) {
                            reportError("GetIter: object must be an array, map or class instance", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        if(objVal.ptrValue->type == Object::Type::Array) {
                            Array* arr = static_cast<Array*>(objVal.ptrValue);
                            Iterator* iter = gc->allocate<Iterator>(gc.get(), arr);
                            PUSH(Value(iter));
                        } else if(objVal.ptrValue->type == Object::Type::Map) {
                            Map* map = static_cast<Map*>(objVal.ptrValue);
                            Iterator* iter = gc->allocate<Iterator>(gc.get(), map);
                            PUSH(Value(iter));
                        } else if (objVal.ptrValue->type == Object::Type::Instance) {
                            Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                            auto iterMethodOpt = instance->cls->methods->get(STRINGS._iter);
                            if(!iterMethodOpt.has_value()) {
                                reportError(std::format("GetIter: class \"{}\" does not have an _iter method with 0 arguments", instance->cls->name->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            Value iterMethodVal = iterMethodOpt.value();
                            assert(iterMethodVal.type == Value::Type::Function);
                            Routine* routine = iterMethodVal.funcValue;
                            StackFrame newFrame;
                            newFrame.routine = routine;
                            newFrame.base = stack.top;
                            newFrame.top = newFrame.base + routine->localCount;
                            newFrame.ip = 0;
                            if(routine->arity != 1) {
                                reportError(std::format("GetIter: class \"{}\" does not have an _iter method with 0 arguments", instance->cls->name->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            StackFrame* frame = pushFrame(newFrame);
                            newFrame.base[0] = objVal; // push 'this' as the first argument
                            curFrame = frame;
                            curRoutine = curFrame->routine;
                        } else {
                            reportError("GetIter: object must be an array, map or class instance", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        break;
                    }
                    case Builtin::IterNext: {
                        Value iterVal = POP();
                        if(iterVal.type != Value::Type::Object) {
                            reportError("IterNext: object must be an iterator or class instance", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        if(iterVal.ptrValue->type == Object::Type::Iterator) {
                            Iterator* iter = static_cast<Iterator*>(iterVal.ptrValue);
                            Value nextVal = iter->next();
                            PUSH(nextVal);
                        } else if (iterVal.ptrValue->type == Object::Type::Instance) {
                            Instance* instance = static_cast<Instance*>(iterVal.ptrValue);
                            auto nextMethodOpt = instance->cls->methods->get(STRINGS._next);
                            if(!nextMethodOpt.has_value()) {
                                reportError(std::format("IterNext: class \"{}\" does not have a _next method with 0 arguments", instance->cls->name->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            Value nextMethodVal = nextMethodOpt.value();
                            assert(nextMethodVal.type == Value::Type::Function);
                            Routine* routine = nextMethodVal.funcValue;
                            StackFrame newFrame;
                            newFrame.routine = routine;
                            newFrame.base = stack.top;
                            newFrame.top = newFrame.base + routine->localCount;
                            newFrame.ip = 0;
                            if(routine->arity != 1) {
                                reportError(std::format("IterNext: class \"{}\" does not have a _next method with 0 arguments", instance->cls->name->data), Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                            StackFrame* frame = pushFrame(newFrame);
                            newFrame.base[0] = iterVal; // push 'this' as the first argument
                            curFrame = frame;
                            curRoutine = curFrame->routine;
                        } else {
                            reportError("IterNext: object must be an iterator or class instance", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        break;
                    }
                    case Builtin::NewArray: {
                        Value sizeVal = POP();
                        assert(sizeVal.type == Value::Type::Int);
                        int64_t size = sizeVal.intValue;
                        assert(size >= 0);
                        Array* arr = gc->allocate<Array>(gc.get(), static_cast<uint32_t>(size));
                        PUSH(Value(arr));
                        break;
                    }
                    case Builtin::NewMap: {
                        Value sizeVal = POP();
                        assert(sizeVal.type == Value::Type::Int);
                        int64_t size = sizeVal.intValue;
                        assert(size >= 0);
                        Map* map = gc->allocate<Map>(gc.get(), static_cast<uint32_t>(size));
                        PUSH(Value(map));
                        break;
                    }
                    case Builtin::Print: {
                        Value val = POP();
                        PUSH(Value::Null); // print does not return a value, so we push null to maintain stack balance
                        switch(val.type) {
                            case Value::Type::Null: std::cout << "null"; break;
                            case Value::Type::Bool: std::cout << (val.boolValue ? "true" : "false"); break;
                            case Value::Type::Int: std::cout << val.intValue; break;
                            case Value::Type::Float: std::cout << val.floatValue; break;
                            case Value::Type::String: std::cout << val.strValue->data; break;
                            case Value::Type::Function: std::cout << "<function>"; break;
                            case Value::Type::Object: {
                                switch(val.ptrValue->type) {
                                    case Object::Type::Array: std::cout << "<array>"; break;
                                    case Object::Type::Map: std::cout << "<map>"; break;
                                    case Object::Type::Class: std::cout << "<class>"; break;
                                    case Object::Type::Instance: std::cout << "<instance>"; break;
                                    case Object::Type::Iterator: std::cout << "<iterator>"; break;
                                    case Object::Type::StrObj: {
                                        StrObj* strObj = static_cast<StrObj*>(val.ptrValue);
                                        std::cout << (const char*)strObj->data->getData();
                                        break;
                                    }
                                    default: assert(false); break;
                                }
                                break;
                            }
                            case Value::Type::Error: std::cout << "<error>"; break;
                            default: assert(false); break;
                        }
                        break;
                    }
                    case Builtin::Println: {
                        Value val = POP();
                        PUSH(Value::Null); // println does not return a value, so we push null to maintain stack balance
                        switch(val.type) {
                            case Value::Type::Null: std::cout << "null"; break;
                            case Value::Type::Bool: std::cout << (val.boolValue ? "true" : "false"); break;
                            case Value::Type::Int: std::cout << val.intValue; break;
                            case Value::Type::Float: std::cout << val.floatValue; break;
                            case Value::Type::String: std::cout << val.strValue->data; break;
                            case Value::Type::Function: std::cout << "<function>"; break;
                            case Value::Type::Object: {
                                switch(val.ptrValue->type) {
                                    case Object::Type::Array: std::cout << "<array>"; break;
                                    case Object::Type::Map: std::cout << "<map>"; break;
                                    case Object::Type::Class: std::cout << "<class>"; break;
                                    case Object::Type::Instance: std::cout << "<instance>"; break;
                                    case Object::Type::Iterator: std::cout << "<iterator>"; break;
                                    case Object::Type::StrObj: {
                                        StrObj* strObj = static_cast<StrObj*>(val.ptrValue);
                                        std::cout << (const char*)strObj->data->getData();
                                        break;
                                    }
                                    default: assert(false); break;
                                }
                                break;
                            }
                            case Value::Type::Error: std::cout << "<error>"; break;
                            default: assert(false); break;
                        }
                        std::cout << "\n";
                        break;
                    }
                    case Builtin::Input: {
                        std::string input;
                        std::cin >> input;
                        StrObj* str = gc->allocate<StrObj>(gc.get(), input.c_str(), input.size());
                        PUSH(Value(str));
                        break;
                    }
                    case Builtin::Error: {
                        Value code = POP(); 
                        assert(code.type == Value::Type::Int);
                        PUSH(Value(Value::Type::Error, code.intValue));
                        break;
                    }
                    case Builtin::Check: {
                        Value val = POP();
                        PUSH(Value(val.type != Value::Type::Error));
                        break;
                    }
                    case Builtin::Intern: {
                        Value strVal = POP();
                        if(strVal.type == Value::Type::String) {
                            PUSH(strVal);
                            break;
                        }
                        if(!(strVal.type == Value::Type::Object && strVal.ptrValue->type == Object::Type::StrObj)) {
                            reportError("Intern: argument must be a StrObj", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        String* internedStr = internString(static_cast<StrObj*>(strVal.ptrValue));
                        PUSH(Value(internedStr));
                        break;
                    }
                    case Builtin::Type: {
                        Value val = POP();
                        switch(val.type) {
                            case Value::Type::Null: PUSH(Value(STRINGS.Null)); break;
                            case Value::Type::Bool: PUSH(Value(STRINGS.Bool)); break;
                            case Value::Type::Int: PUSH(Value(STRINGS.Int)); break;
                            case Value::Type::Float: PUSH(Value(STRINGS.Float)); break;
                            case Value::Type::String: PUSH(Value(STRINGS.String_)); break;
                            case Value::Type::Function: PUSH(Value(STRINGS.Function)); break;
                            case Value::Type::Object: {
                                switch(val.ptrValue->type) {
                                    case Object::Type::Array: PUSH(Value(STRINGS.Array)); break;
                                    case Object::Type::Map: PUSH(Value(STRINGS.Map)); break;
                                    case Object::Type::Class: PUSH(Value(STRINGS.Class)); break;
                                    case Object::Type::Instance: PUSH(Value(STRINGS.Instance)); break;
                                    case Object::Type::Iterator: PUSH(Value(STRINGS.Iterator)); break;
                                    case Object::Type::StrObj: PUSH(Value(STRINGS.StrObj)); break; 
                                    default: assert(false); break;
                                }
                                break;
                            }
                            case Value::Type::Error: PUSH(Value(STRINGS.Error)); break;
                            default: assert(false); break;
                        }
                        break;
                    }
                    case Builtin::Int: {
                        Value val = POP();
                        if(val.type == Value::Type::Int) {
                            PUSH(val);
                        } else if(val.type == Value::Type::Float) {
                            PUSH(Value(static_cast<int64_t>(val.floatValue)));
                        } else if(val.type == Value::Type::Bool) {
                            PUSH(Value(static_cast<int64_t>(val.boolValue ? 1 : 0)));
                        } else if(val.type == Value::Type::String || (val.type == Value::Type::Object && val.ptrValue->type == Object::Type::StrObj)) {
                            StrView strView = val.asString();
                            try {
                                PUSH(Value(static_cast<int64_t>(std::stoll(std::string(strView.data, strView.length)))));
                            } catch(const std::exception&) {
                                reportError("Int: invalid string format", Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                        } else {
                            reportError("Int: unexpected type", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        break;
                    }
                    case Builtin::Float: {
                        Value val = POP();
                        if(val.type == Value::Type::Float) {
                            PUSH(val);
                        } else if(val.type == Value::Type::Int) {
                            PUSH(Value(static_cast<double>(val.intValue)));
                        } else if(val.type == Value::Type::Bool) {
                            PUSH(Value(static_cast<double>(val.boolValue ? 1 : 0)));
                        } else if(val.type == Value::Type::String || (val.type == Value::Type::Object && val.ptrValue->type == Object::Type::StrObj)) {
                            StrView strView = val.asString();
                            try {
                                PUSH(Value(std::stod(std::string(strView.data, strView.length))));
                            } catch(const std::exception&) {
                                reportError("Float: invalid string format", Error::Type::RuntimeError);
                                PUSH(Value::Error);
                                return;
                            }
                        } else {
                            reportError("Float: unexpected type", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        break;
                    }
                    case Builtin::Str: {
                        Value val = POP();
                        if(val.type == Value::Type::Int) {
                            std::string str = std::to_string(val.intValue);
                            StrObj* strObj = gc->allocate<StrObj>(gc.get(), str.c_str(), str.size());
                            PUSH(Value(strObj));
                        } else if(val.type == Value::Type::Float) {
                            std::string str = std::to_string(val.floatValue);
                            StrObj* strObj = gc->allocate<StrObj>(gc.get(), str.c_str(), str.size());
                            PUSH(Value(strObj));
                        } else if(val.type == Value::Type::Bool) {
                            PUSH(Value(val.boolValue ? STRINGS.true_ : STRINGS.false_));
                        } else if(val.type == Value::Type::Null) {
                            PUSH(Value(STRINGS.null));
                        } else {
                            reportError("Str: unexpected type", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        break;
                    }
                    case Builtin::Assert: {
                        Value condition = POP();
                        if(condition.type != Value::Type::Bool) {
                            reportError("Assert: condition must be a boolean", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        if(!condition.boolValue) {
                            reportError("Assertion failed", Error::Type::RuntimeError);
                            PUSH(Value::Error);
                            return;
                        }
                        PUSH(Value::Null); 
                        break;
                    }
                    default: {
                        reportError(std::format("Unknown builtin function: {:#04x}", builtinId), Error::Type::RuntimeError);
                        PUSH(Value::Error);
                        return;
                    }
                }
                break;
            }
            case Opcode::Halt: {
                reportError("Program halted", Error::Type::RuntimeError);
                PUSH(Value::Error);
                return;
            }
            default: {
                reportError(std::format("Unknown opcode: {:#04x}", static_cast<int>(opcode)), Error::Type::RuntimeError);
                PUSH(Value::Error);
                return;
            }
        }
    }
}

String* VM::internString(const std::string& str) {
    auto it = stringTable.find(str);
    if(it != stringTable.end()) {
        return it->second.get();
    }
    auto interned = std::make_unique<String>(str.c_str(), str.size());
    String* internedPtr = interned.get();
    stringTable[str] = std::move(interned);
    return internedPtr;
}

String* VM::internString(StrObj* strObj) {
    std::string str(static_cast<char*>(strObj->data->getData()), strObj->length);
    return internString(str);
}

VM::StackFrame* VM::pushFrame(const StackFrame& frame) {
    stackFrames.push_back(frame);
    stack.top += frame.routine->localCount + frame.routine->maxStackSize;
    if(stack.top > stack.base + stack.capacity) {
        reportError("Stack overflow", Error::Type::RuntimeError);
        throw VMException(VMException::Type::StackOverflow);
    }
    // Initialize local variables to null, avoid garbage values
    std::memset(frame.base, 0, sizeof(Value) * frame.routine->localCount);
    return &stackFrames.back();
}

void VM::popFrame() {
    stack.top = stackFrames.back().base;
    stackFrames.pop_back();
}

}
