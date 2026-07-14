#include "vm.h"

#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "vm/value.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>
#include <format>

// NOTE: VM 不会校验编译模块的合法性, 假设所有输入的模块都是合法的, 任何不合预期的模块输入都使用断言终止

namespace Zeta {

VM::VM(Config config, ErrorHandler handler) : config(config), errorHandler(handler) {
    stack.base = static_cast<Value*>(std::malloc(config.initStackSize * 1024));
    stack.capacity = config.initStackSize * 1024 / sizeof(Value);
    stack.top = stack.base;
    if(!stack.base) {
        errorHandler({Error::Type::VMError, "Failed to allocate initial stack"});
    }
    for(const auto& path : config.moduleSearchPaths) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::canonical(path, ec);
        if(ec) {
            errorHandler({Error::Type::VMError, std::format("Invalid module search path: \"{}\" <{}>", path, ec.message())});
        }
    }
    gc = std::make_unique<GC>(this);
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

callFunction接口只能调用显式加载的模块中定义的函数, 换句话说, VM 自动加载的模块对外部不可见.

对于未指定模块名的外部符号, 按照导入的顺序查找. 如果有多个模块都定义了该符号, 则使用第一个模块中的定义, 不建议开发者依赖这一特性, 这种情况应该使用别名来避免.
*/

void VM::loadModule(const std::string& filePath) {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::canonical(filePath, ec);
    if(ec) {
        errorHandler({Error::Type::VMError, std::format("Invalid module file path: \"{}\" <{}>", filePath, ec.message())});
        return;
    } else if(!std::filesystem::is_regular_file(path)) {
        errorHandler({Error::Type::VMError, std::format("\"{}\" is not a regular file", filePath)});
        return;
    }
    importModule(path);
    publicModules.push_back(path.string());
}

Value VM::callFunction(const std::string& moduleName, const std::string& funcName, int argc, Value* args) {
    auto it = std::find(publicModules.begin(), publicModules.end(), moduleName);
    if(it == publicModules.end()) {
        errorHandler({Error::Type::RuntimeError, std::format("Module \"{}\" not loaded", moduleName)});
        return Value();
    }
    const auto& moduleInfo = loadedModules[moduleName];
    auto symIt = moduleInfo.symbolMap.find(funcName);
    if(symIt == moduleInfo.symbolMap.end()) {
        errorHandler({Error::Type::RuntimeError, std::format("Function \"{}\" not found in module \"{}\"", funcName, moduleName)});
        return Value();
    }
    Value& funcVal = global[symIt->second];
    if(funcVal.type != Value::Type::Object || funcVal.ptrValue->type != Object::Type::Function) {
        errorHandler({Error::Type::RuntimeError, std::format("\"{}\" in module \"{}\" is not a function", funcName, moduleName)});
        return Value();
    }
    Function* func = static_cast<Function*>(funcVal.ptrValue);
    return callFunction(func, argc, args);
}

Value VM::callFunction(const std::string& funcName, int argc, Value* args) {
    for(const auto& modulePath : publicModules) {
        const auto& moduleInfo = loadedModules[modulePath];
        auto it = moduleInfo.symbolMap.find(funcName);
        if(it != moduleInfo.symbolMap.end()) {
            uint32_t globalIndex = it->second;
            Value& funcVal = global[globalIndex];
            if(funcVal.type != Value::Type::Object || funcVal.ptrValue->type != Object::Type::Function) {
                errorHandler({Error::Type::RuntimeError, std::format("\"{}\" in module \"{}\" is not a function", funcName, modulePath)});
                return Value();
            }
            Function* func = static_cast<Function*>(funcVal.ptrValue);
            return callFunction(func, argc, args);
        }
    }
    errorHandler({Error::Type::RuntimeError, std::format("Function \"{}\" not found in any loaded module", funcName)});
    return Value();
}

void VM::importModule(const std::filesystem::path& path) {
    std::string pathStr = path.string();
    if(loadedModules.find(pathStr) != loadedModules.end()) {
        return;
    }
    std::string compileError;
    std::unique_ptr<Module> module = compileModule(pathStr, &compileError);
    if(!module) {
        errorHandler({Error::Type::VMError, "Failed to compile module: " + pathStr + "\n" + compileError});
        return;
    }
    ModuleInfo& moduleInfo = loadedModules[pathStr];
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
                Function* func = gc->allocate<Function>();
                func->routine = routines[routineIndex].get();
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
                    Function* func = gc->allocate<Function>();
                    func->routine = routines[routineIndex].get();
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
        routine->bytecode = std::move(proto->bytecode);
        for(const auto& constVal : proto->constants) {
            routine->constants.push_back(makeValue(makeValue, constVal));
        }
        routine->arity = proto->arity;
        routine->localCount = proto->localCount;
        routine->maxStackSize = proto->maxStackSize;
        routines.push_back(std::move(routine));
    }
    for(const auto& [symName, sym] : module->globalSyms) {
        uint32_t index = global.size();
        global.push_back(makeValue(makeValue, sym.initValue));
        moduleInfo.symbolMap[symName] = index;
        for(const auto& pos : sym.relocations) {
            uint32_t routineIndex = moduleInfo.getRoutineIndex(pos.protoIndex);
            uint32_t offset = pos.bytecodeOffset;
            assert(routineIndex < routines.size() && offset + 4 <= routines[routineIndex]->bytecode.size());
            std::memcpy(routines[routineIndex]->bytecode.data() + offset, &index, 4);
        }
    }
    std::vector<std::pair<std::string, std::string>> nameToPath;
    for(const auto& import : module->imports) {
        std::filesystem::path importPath = searchModuleFile(path, import);
        if(importPath.empty()) {
            errorHandler({Error::Type::VMError, std::format("Failed to find imported module: \"{}\" imported by \"{}\"", import, pathStr)});
            return;
        }
        nameToPath.push_back({import, importPath.string()});
        importModule(importPath);
    }
    for(const auto& extSym : module->externalSyms) {
        uint32_t index;
        if(extSym.moduleName.empty()) {
            bool found = false;
            for(const auto& [import, importPath] : nameToPath) {
                const auto& symMap = loadedModules[importPath].symbolMap;
                auto symIt = symMap.find(extSym.name);
                if(symIt != symMap.end()) {
                    index = symIt->second;
                    found = true;
                    break;
                }
            }
            if(!found) {
                errorHandler({Error::Type::VMError, std::format("Failed to resolve external symbol: \"{}\" in module \"{}\"", extSym.name, pathStr)});
                return;
            }
        } else {
            auto it = std::find_if(nameToPath.begin(), nameToPath.end(), [&extSym](const std::pair<std::string, std::string>& p) {
                return p.first == extSym.moduleName;
            });
            assert(it != nameToPath.end());
            const auto& symMap = loadedModules[it->second].symbolMap;
            auto symIt = symMap.find(extSym.name);
            if(symIt == symMap.end()) {
                errorHandler({Error::Type::VMError, std::format("Failed to resolve external symbol: \"{}\" in module \"{}\", module \"{}\" does not define the symbol", extSym.name, pathStr, extSym.moduleName)});
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
            bool found = false;
            for(const auto& [import, importPath] : nameToPath) {
                const auto& symMap = loadedModules[importPath].symbolMap;
                auto symIt = symMap.find(patch.baseClassNames.second);
                if(symIt != symMap.end()) {
                    baseIdx = symIt->second;
                    found = true;
                    break;
                }
            }
            if(!found) {
                errorHandler({Error::Type::VMError, std::format("Failed to resolve base class: \"{}\" for class \"{}\" in module \"{}\"", patch.baseClassNames.second, patch.className, pathStr)});
                return;
            }
        } else {
            auto it = std::find_if(nameToPath.begin(), nameToPath.end(), [&patch](const std::pair<std::string, std::string>& p) {
                return p.first == patch.baseClassNames.first;
            });
            assert(it != nameToPath.end());
            const auto& symMap = loadedModules[it->second].symbolMap;
            auto symIt = symMap.find(patch.baseClassNames.second);
            if(symIt == symMap.end()) {
                errorHandler({Error::Type::VMError, std::format("Failed to resolve base class: \"{}\" for class \"{}\" in module \"{}\", module \"{}\" does not define the class", patch.baseClassNames.second, patch.className, pathStr, patch.baseClassNames.first)});
                return;
            }
            baseIdx = symIt->second;
        }
        Value& baseVal = global[baseIdx];
        assert(baseVal.type == Value::Type::Object && baseVal.ptrValue->type == Object::Type::Class);
        gc->writeBarrier(cls, (Object**)(&cls->base), baseVal.ptrValue);
    }
}

std::filesystem::path VM::searchModuleFile(const std::filesystem::path& basePath, const std::string& moduleName) {
    // 1. based on the directory of the importing module
    std::filesystem::path candidate = (basePath.parent_path() / moduleName).lexically_normal();
    if(std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
        return candidate;
    }
    // 2. absolute path or relative to current working directory
    candidate = std::filesystem::path(moduleName);
    if(candidate.is_absolute()) {
        if(std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    } else {
        candidate = (std::filesystem::current_path() / candidate).lexically_normal();
        if(std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    // 3. search in module search paths
    for(const auto& searchPath : config.moduleSearchPaths) {
        candidate = (std::filesystem::path(searchPath) / moduleName).lexically_normal();
        if(std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return {};
}

Value VM::callFunction(Function* func, int argc, Value* args) {
    Routine* routine = func->routine;
    if(argc != routine->arity) {
        errorHandler({Error::Type::RuntimeError, std::format("Function expects {} arguments, but {} were provided", routine->arity, argc)});
        return Value();
    }
    StackFrame frame;
    frame.routine = routine;
    frame.base = stack.top;
    frame.top = stack.top + routine->localCount;
    frame.ip = 0;
    pushFrame(frame);
    for(int i = 0; i < argc; i++) {
        frame.base[i] = args[i];
    }
    return execute();
}

// TODO: 目前编译期和运行期都没做函数参数个数检查, 这是个严重问题, 可能导致栈结构破坏. 由于编译期不知道外部函数信息, 将参数个数作为参数传递并在运行期检查会影响性能, 所以可以考虑在模块加载期完成这个检查.
Value VM::execute() {
    StackFrame* curFrame = &stackFrames.back();
    Routine* curRoutine = curFrame->routine;

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
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue + b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue + b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) + b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue + static_cast<double>(b.intValue)));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Add"});
                    return Value();
                }
                break;
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Sub"});
                    return Value();
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Mul"});
                    return Value();
                }
                break;
            }
            case Opcode::Div: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    if(b.intValue == 0) {
                        errorHandler({Error::Type::RuntimeError, "Division by zero"});
                        return Value();
                    }
                    PUSH(Value(a.intValue / b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue / b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) / b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue / static_cast<double>(b.intValue)));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Div"});
                    return Value();
                }
            }
            case Opcode::Mod: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    if(b.intValue == 0) {
                        errorHandler({Error::Type::RuntimeError, "Modulo by zero"});
                        return Value();
                    }
                    PUSH(Value(a.intValue % b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Mod"});
                    return Value();
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand type for Neg"});
                    return Value();
                }
                break;
            }
            case Opcode::BitAnd: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue & b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for BitAnd"});
                    return Value();
                }
                break;
            }
            case Opcode::BitOr: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue | b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for BitOr"});
                    return Value();
                }
                break;
            }
            case Opcode::BitXor: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue ^ b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for BitXor"});
                    return Value();
                }
                break;
            }
            case Opcode::BitNot: {
                Value a = POP();
                if(a.type == Value::Type::Int) {
                    PUSH(Value(~a.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand type for BitNot"});
                    return Value();
                }
                break;
            }
            case Opcode::Shl: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue << b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Shl"});
                    return Value();
                }
                break;
            }
            case Opcode::Shr: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue >> b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Shr"});
                    return Value();
                }
                break;
            }
            case Opcode::Not: {
                Value a = POP();
                if(a.type == Value::Type::Bool) {
                    PUSH(Value(!a.boolValue));
                } else if(a.type == Value::Type::Int) {
                    PUSH(Value(static_cast<bool>(a.intValue)));
                } else if(a.type == Value::Type::Float) {
                    PUSH(Value(static_cast<bool>(a.floatValue)));
                } else if(a.type == Value::Type::Null) {
                    PUSH(Value(true));
                } else if(a.type == Value::Type::String || a.type == Value::Type::Object) {
                    PUSH(Value(false));
                } else {
                    assert(false);
                }
                break;
            }
            case Opcode::Eq: {
                // TODO: 目前是严格类型相等, 后续可能要支持类型转换的相等, 比如 0 == 0.0, Opcode::Neq 同理
                Value b = POP();
                Value a = POP();
                if(a.type != b.type) {
                    PUSH(Value(false));
                } else {
                    switch(a.type) {
                        case Value::Type::Null: PUSH(Value(true)); break;
                        case Value::Type::Int: PUSH(Value(a.intValue == b.intValue)); break;
                        case Value::Type::Float: PUSH(Value(a.floatValue == b.floatValue)); break;
                        case Value::Type::Bool: PUSH(Value(a.boolValue == b.boolValue)); break;
                        case Value::Type::String: PUSH(Value(a.strValue == b.strValue)); break;
                        case Value::Type::Object: PUSH(Value(a.ptrValue == b.ptrValue)); break;
                        default: assert(false);
                    }
                }
                break;
            }
            case Opcode::Neq: {
                Value b = POP();
                Value a = POP();
                if(a.type != b.type) {
                    PUSH(Value(true));
                } else {
                    switch(a.type) {
                        case Value::Type::Null: PUSH(Value(false)); break;
                        case Value::Type::Int: PUSH(Value(a.intValue != b.intValue)); break;
                        case Value::Type::Float: PUSH(Value(a.floatValue != b.floatValue)); break;
                        case Value::Type::Bool: PUSH(Value(a.boolValue != b.boolValue)); break;
                        case Value::Type::String: PUSH(Value(a.strValue != b.strValue)); break;
                        case Value::Type::Object: PUSH(Value(a.ptrValue != b.ptrValue)); break;
                        default: assert(false);
                    }
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Lt"});
                    return Value();
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Gt"});
                    return Value();
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Le"});
                    return Value();
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
                    errorHandler({Error::Type::RuntimeError, "Unsupported operand types for Ge"});
                    return Value();
                }
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
                // TODO: 需要支持类型转换
                if(cond.type != Value::Type::Bool) {
                    errorHandler({Error::Type::RuntimeError, "Condition must be a boolean"});
                    return Value();
                }
                if(!cond.boolValue) {
                    curFrame->ip = offset;
                }
                break;
            }
            case Opcode::JumpIfTrue: {
                uint32_t offset;
                READ_UINT32(offset);
                Value cond = POP();
                // TODO: 需要支持类型转换
                if(cond.type != Value::Type::Bool) {
                    errorHandler({Error::Type::RuntimeError, "Condition must be a boolean"});
                    return Value();
                }
                if(cond.boolValue) {
                    curFrame->ip = offset;
                }
                break;
            }
            case Opcode::Ret:{
                Value retVal = POP();
                popFrame();
                if(stackFrames.empty()) {
                    return retVal;
                }
                curFrame = &stackFrames.back();
                curRoutine = curFrame->routine;
                PUSH(retVal);
                break;
            }
            case Opcode::Call: {
                Value callee = POP();
                if(callee.type == Value::Type::Object && callee.ptrValue->type == Object::Type::Function) {
                    Function* func = static_cast<Function*>(callee.ptrValue);
                    Routine* routine = func->routine;
                    StackFrame newFrame;
                    newFrame.routine = routine;
                    newFrame.base = stack.top;
                    newFrame.top = newFrame.base + routine->localCount;
                    newFrame.ip = 0;
                    pushFrame(newFrame);
                    for(int i = routine->arity - 1; i >= 0; i--) {
                        newFrame.base[i] = POP();
                    }
                    curFrame = &stackFrames.back();
                    curRoutine = curFrame->routine;
                } else if(callee.type == Value::Type::Object && callee.ptrValue->type == Object::Type::Class) {
                    Class* cls = static_cast<Class*>(callee.ptrValue);
                    Instance* instance = gc->allocate<Instance>(gc.get(), cls);
                    auto constructor = cls->methods->get(cls->name);
                    if(constructor.has_value()) {
                        Value constructorVal = constructor.value();
                        assert(constructorVal.type == Value::Type::Object && constructorVal.ptrValue->type == Object::Type::Function);
                        Function* func = static_cast<Function*>(constructorVal.ptrValue);
                        Routine* routine = func->routine;
                        StackFrame newFrame;
                        newFrame.routine = routine;
                        newFrame.base = stack.top;
                        newFrame.top = newFrame.base + routine->localCount;
                        newFrame.ip = 0;
                        pushFrame(newFrame);
                        newFrame.base[0] = Value(instance); // push 'this' as the first argument
                        for(int i = routine->arity - 1; i >= 1; i--) {
                            newFrame.base[i] = POP();
                        }
                        curFrame = &stackFrames.back();
                        curRoutine = curFrame->routine;
                    } else {
                        PUSH(Value(instance));
                    }
                } else {
                    errorHandler({Error::Type::RuntimeError, "Call: callee must be a function"});
                    return Value();
                }
                break;
            }
            case Opcode::GetField: {
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    errorHandler({Error::Type::RuntimeError, "GetField: object must be a class instance"});
                    return Value();
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
                    errorHandler({Error::Type::RuntimeError, std::format("GetField: field \"{}\" not found in instance of class \"{}\"", fieldName->data, instance->cls->name->data)});
                    return Value();
                }
                PUSH(fieldValOpt.value());
            }
            case Opcode::SetField: {
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    errorHandler({Error::Type::RuntimeError, "SetField: object must be a class instance"});
                    return Value();
                }
                Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* fieldName = nameVal.strValue;
                Value fieldVal = POP();
                instance->fields->set(fieldName, fieldVal);
            }
            case Opcode::CallMethod: {
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    errorHandler({Error::Type::RuntimeError, "CallMethod: object must be a class instance"});
                    return Value();
                }
                Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* methodName = nameVal.strValue;
                auto methodValOpt = instance->cls->methods->get(methodName);
                if(!methodValOpt.has_value()) {
                    errorHandler({Error::Type::RuntimeError, std::format("CallMethod: method \"{}\" not found in class \"{}\"", methodName->data, instance->cls->name->data)});
                    return Value();
                }
                Value methodVal = methodValOpt.value();
                assert(methodVal.type == Value::Type::Object && methodVal.ptrValue->type == Object::Type::Function);
                Function* func = static_cast<Function*>(methodVal.ptrValue);
                Routine* routine = func->routine;
                StackFrame newFrame;
                newFrame.routine = routine;
                newFrame.base = stack.top;
                newFrame.top = newFrame.base + routine->localCount;
                newFrame.ip = 0;
                pushFrame(newFrame);
                newFrame.base[0] = objVal; // push 'this' as the first argument
                for(int i = routine->arity - 1; i >= 1; i--) {
                    newFrame.base[i] = POP();
                }
                curFrame = &stackFrames.back();
                curRoutine = curFrame->routine;
                break;
            }
            case Opcode::IndexGet: {
                Value index = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object) {
                    errorHandler({Error::Type::RuntimeError, "IndexGet: object must be an array or map"});
                    return Value();
                }
                if(objVal.ptrValue->type == Object::Type::Array) {
                    Array* arr = static_cast<Array*>(objVal.ptrValue);
                    if(index.type != Value::Type::Int) {
                        errorHandler({Error::Type::RuntimeError, "IndexGet: array index must be an integer"});
                        return Value();
                    }
                    int64_t idx = index.intValue;
                    if(idx < 0 || idx >= arr->size) {
                        errorHandler({Error::Type::RuntimeError, "IndexGet: array index out of bounds"});
                        return Value();
                    }
                    PUSH(arr->get(idx));
                } else if(objVal.ptrValue->type == Object::Type::Map) {
                    Map* map = static_cast<Map*>(objVal.ptrValue);
                    if(index.type != Value::Type::String) {
                        errorHandler({Error::Type::RuntimeError, "IndexGet: map key must be a string"});
                        return Value();
                    }
                    auto valOpt = map->get(index.strValue);
                    if(!valOpt.has_value()) {
                        errorHandler({Error::Type::RuntimeError, std::format("IndexGet: key \"{}\" not found in map", index.strValue->data)});
                        return Value();
                    }
                    PUSH(valOpt.value());
                } else {
                    errorHandler({Error::Type::RuntimeError, "IndexGet: object must be an array or map"});
                    return Value();
                }
                break;
            }
            case Opcode::IndexSet: {
                Value index = POP();
                Value objVal = POP();
                Value value = POP();
                if(objVal.type != Value::Type::Object) {
                    errorHandler({Error::Type::RuntimeError, "IndexSet: object must be an array or map"});
                    return Value();
                }
                if(objVal.ptrValue->type == Object::Type::Array) {
                    Array* arr = static_cast<Array*>(objVal.ptrValue);
                    if(index.type != Value::Type::Int) {
                        errorHandler({Error::Type::RuntimeError, "IndexSet: array index must be an integer"});
                        return Value();
                    }
                    int64_t idx = index.intValue;
                    if(idx < 0 || idx >= arr->size) {
                        errorHandler({Error::Type::RuntimeError, "IndexSet: array index out of bounds"});
                        return Value();
                    }
                    arr->set(idx, value);
                } else if(objVal.ptrValue->type == Object::Type::Map) {
                    Map* map = static_cast<Map*>(objVal.ptrValue);
                    if(index.type != Value::Type::String) {
                        errorHandler({Error::Type::RuntimeError, "IndexSet: map key must be a string"});
                        return Value();
                    }
                    map->set(index.strValue, value);
                } else {
                    errorHandler({Error::Type::RuntimeError, "IndexSet: object must be an array or map"});
                    return Value();
                }
                break;
            }
            case Opcode::Pop: {
                POP();
                break;
            }
            case Opcode::Dup: {
                PUSH(*curFrame->top);
                break;
            }
            case Opcode::CallBuiltin: {
                uint8_t builtinId;
                READ_BYTE(builtinId);
                switch(static_cast<Builtin>(builtinId)) {
                    case Builtin::GetIter: {
                        // TODO
                        break;
                    }
                    case Builtin::IterNext: {
                        // TODO
                        break;
                    }
                    case Builtin::NewArray: {
                        Value sizeVal = POP();
                        assert(sizeVal.type == Value::Type::Int);
                        int64_t size = sizeVal.intValue;
                        assert(size >= 0);
                        Array* arr = gc->allocate<Array>(gc.get(), static_cast<uint32_t>(size));
                        for(int i = size - 1; i >= 0; i--) {
                            arr->set(i, POP());
                        }
                        PUSH(Value(arr));
                        break;
                    }
                    case Builtin::NewMap: {
                        Value sizeVal = POP();
                        assert(sizeVal.type == Value::Type::Int);
                        int64_t size = sizeVal.intValue;
                        assert(size >= 0);
                        Map* map = gc->allocate<Map>(gc.get(), static_cast<uint32_t>(size));
                        for(int i = size - 1; i >= 0; i--) {
                            Value value = POP();
                            Value keyVal = POP();
                            assert(keyVal.type == Value::Type::String);
                            map->set(keyVal.strValue, value);
                        }
                        PUSH(Value(map));
                        break;
                    }
                    default: {
                        errorHandler({Error::Type::RuntimeError, std::format("Unknown builtin function: {:#04x}", builtinId)});
                        return Value();
                    }
                }
                break;
            }
            case Opcode::Halt: {
                errorHandler({Error::Type::RuntimeError, "Program halted"});
                return Value();
            }
            default: {
                errorHandler({Error::Type::RuntimeError, std::format("Unknown opcode: {:#04x}", static_cast<int>(opcode))});
                return Value();
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

void VM::pushFrame(const StackFrame& frame) {
    stackFrames.push_back(frame);
    stack.top += frame.routine->localCount + frame.routine->maxStackSize;
    if(stack.top > stack.base + stack.capacity) {
        // TODO: 栈扩容
        errorHandler({Error::Type::RuntimeError, "Stack overflow"});
    }
}

void VM::popFrame() {
    stack.top = stackFrames.back().base;
    stackFrames.pop_back();
}

}
