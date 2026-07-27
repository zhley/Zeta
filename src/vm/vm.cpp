#include "vm.h"

#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "vm/value.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <format>

// NOTE: VM 不会校验编译模块的合法性, 假设所有输入的模块都是合法的, 任何不合预期的模块输入都使用断言终止

namespace Zeta {

VM::VM(Config config, ErrorHandler handler) : config(config), errorHandler(handler) {
    stack.base = static_cast<Value*>(std::malloc(config.stackSize * 1024));
    stack.capacity = config.stackSize * 1024 / sizeof(Value);
    stack.top = stack.base;
    if(!stack.base) {
        errorHandler({Error::Type::VMError, 0, "Failed to allocate initial stack"});
    }
    for(const auto& path : config.moduleSearchPaths) {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::canonical(path, ec);
        if(ec) {
            errorHandler({Error::Type::VMError, 0, std::format("Invalid module search path: \"{}\" <{}>", path, ec.message())});
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
        errorHandler({Error::Type::VMError, 0, std::format("Invalid module file path: \"{}\" <{}>", filePath, ec.message())});
        return;
    } else if(!std::filesystem::is_regular_file(path)) {
        errorHandler({Error::Type::VMError, 0, std::format("\"{}\" is not a regular file", filePath)});
        return;
    }
    importModule(path);
    publicModules.push_back(path.string());
}

Value VM::callFunction(const std::string& moduleName, const std::string& funcName, int argc, Value* args) {
    auto it = std::find(publicModules.begin(), publicModules.end(), moduleName);
    if(it == publicModules.end()) {
        errorHandler({Error::Type::RuntimeError, 0, std::format("Module \"{}\" not loaded", moduleName)});
        return Value::Error;
    }
    const auto& moduleInfo = loadedModules[moduleName];
    auto symIt = moduleInfo.symbolMap.find(funcName);
    if(symIt == moduleInfo.symbolMap.end()) {
        errorHandler({Error::Type::RuntimeError, 0, std::format("Function \"{}\" not found in module \"{}\"", funcName, moduleName)});
        return Value::Error;
    }
    Value& funcVal = global[symIt->second];
    if(funcVal.type != Value::Type::Function) {
        errorHandler({Error::Type::RuntimeError, 0, std::format("\"{}\" in module \"{}\" is not a function", funcName, moduleName)});
        return Value::Error;
    }
    return callFunction(funcVal.funcValue, argc, args);
}

Value VM::callFunction(const std::string& funcName, int argc, Value* args) {
    for(const auto& modulePath : publicModules) {
        const auto& moduleInfo = loadedModules[modulePath];
        auto it = moduleInfo.symbolMap.find(funcName);
        if(it != moduleInfo.symbolMap.end()) {
            uint32_t globalIndex = it->second;
            Value& funcVal = global[globalIndex];
            if(funcVal.type != Value::Type::Function) {
                errorHandler({Error::Type::RuntimeError, 0, std::format("\"{}\" in module \"{}\" is not a function", funcName, modulePath)});
                return Value::Error;
            }
            return callFunction(funcVal.funcValue, argc, args);
        }
    }
    errorHandler({Error::Type::RuntimeError, 0, std::format("Function \"{}\" not found in any loaded module", funcName)});
    return Value::Error;
}

void VM::importModule(const std::filesystem::path& path) {
    std::string pathStr = path.string();
    if(loadedModules.find(pathStr) != loadedModules.end()) {
        return;
    }
    std::string compileError;
    std::unique_ptr<Module> module = compileModule(pathStr, &compileError);
    if(!module) {
        errorHandler({Error::Type::VMError, 0, "Failed to compile module: " + pathStr + "\n" + compileError});
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
        routine->bytecode = std::move(proto->bytecode);
        routine->lineInfo = std::move(proto->lineInfo);
        routine->arity = proto->arity;
        routine->localCount = proto->localCount;
        routine->maxStackSize = proto->maxStackSize;
        routines.push_back(std::move(routine));
    }
    for(int i = 0; i < module->protos.size(); i++) {
        auto& proto = module->protos[i];
        auto& routine = routines[moduleInfo.protoBaseIndex + i];
        for(const auto& constVal : proto->constants) {
            routine->constants.push_back(makeValue(makeValue, constVal));
        }
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
            errorHandler({Error::Type::VMError, 0, std::format("Failed to find imported module: \"{}\" imported by \"{}\"", import, pathStr)});
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
                errorHandler({Error::Type::VMError, 0, std::format("Failed to resolve external symbol: \"{}\" in module \"{}\"", extSym.name, pathStr)});
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
                errorHandler({Error::Type::VMError, 0, std::format("Failed to resolve external symbol: \"{}\" in module \"{}\", module \"{}\" does not define the symbol", extSym.name, pathStr, extSym.moduleName)});
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
                    errorHandler({Error::Type::VMError, 0, std::format("Failed to resolve base class: \"{}\" for class \"{}\" in module \"{}\"", patch.baseClassNames.second, patch.className, pathStr)});
                    return;
                }
            }
        } else {
            auto it = std::find_if(nameToPath.begin(), nameToPath.end(), [&patch](const std::pair<std::string, std::string>& p) {
                return p.first == patch.baseClassNames.first;
            });
            assert(it != nameToPath.end());
            const auto& symMap = loadedModules[it->second].symbolMap;
            auto symIt = symMap.find(patch.baseClassNames.second);
            if(symIt == symMap.end()) {
                errorHandler({Error::Type::VMError, 0, std::format("Failed to resolve base class: \"{}\" for class \"{}\" in module \"{}\", module \"{}\" does not define the class", patch.baseClassNames.second, patch.className, pathStr, patch.baseClassNames.first)});
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

Value VM::callFunction(Routine* func, int argc, Value* args) {
    if(argc != func->arity) {
        errorHandler({Error::Type::RuntimeError, 0, std::format("Function expects {} arguments, but {} were provided", func->arity, argc)});
        return Value::Error;
    }
    StackFrame frame;
    frame.routine = func;
    frame.base = stack.top;
    frame.top = stack.top + func->localCount;
    frame.ip = 0;
    pushFrame(frame);
    for(int i = 0; i < argc; i++) {
        frame.base[i] = args[i];
    }
    return execute();
}

// TODO: 需要重构
Value VM::execute() {
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
                    StrObj* str = gc->allocate<StrObj>(gc.get(), a.asString(), b.asString());
                    PUSH(Value(str));
                    break;
                }
                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Add"});
                return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Sub"});
                    return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Mul"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::Div: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    if(b.intValue == 0) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Division by zero"});
                        return Value::Error;
                    }
                    PUSH(Value(a.intValue / b.intValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Float) {
                    PUSH(Value(a.floatValue / b.floatValue));
                } else if(a.type == Value::Type::Int && b.type == Value::Type::Float) {
                    PUSH(Value(static_cast<double>(a.intValue) / b.floatValue));
                } else if(a.type == Value::Type::Float && b.type == Value::Type::Int) {
                    PUSH(Value(a.floatValue / static_cast<double>(b.intValue)));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Div"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::Mod: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    if(b.intValue == 0) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Modulo by zero"});
                        return Value::Error;
                    }
                    PUSH(Value(a.intValue % b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Mod"});
                    return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand type for Neg"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::BitAnd: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue & b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for BitAnd"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::BitOr: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue | b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for BitOr"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::BitXor: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue ^ b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for BitXor"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::BitNot: {
                Value a = POP();
                if(a.type == Value::Type::Int) {
                    PUSH(Value(~a.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand type for BitNot"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::Shl: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue << b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Shl"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::Shr: {
                Value b = POP();
                Value a = POP();
                if(a.type == Value::Type::Int && b.type == Value::Type::Int) {
                    PUSH(Value(a.intValue >> b.intValue));
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Shr"});
                    return Value::Error;
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
                                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Eq: _equals method of class \"{}\" should have 2 arguments", aInst->cls->name->data)});
                                    return Value::Error;
                                }
                                Value args[2] = {a, b};
                                PUSH(callFunction(func, 2, args));
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
                                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Neq: _equals method of class \"{}\" should have 2 arguments", aInst->cls->name->data)});
                                    return Value::Error;
                                }
                                Value args[2] = {a, b};
                                Value ret = callFunction(func, 2, args);
                                assert(ret.type == Value::Type::Bool);
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Lt"});
                    return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Gt"});
                    return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Le"});
                    return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Unsupported operand types for Ge"});
                    return Value::Error;
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
                if(stackFrames.size() == initDepth - 1) {
                    return retVal;
                }
                curFrame = &stackFrames.back();
                curRoutine = curFrame->routine;
                PUSH(retVal);
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
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Function expects {} arguments, but {} were provided", routine->arity, argCount)});
                        return Value::Error;
                    }
                    for(int i = routine->arity - 1; i >= 0; i--) {
                        newFrame.base[i] = POP();
                    }
                    pushFrame(newFrame);
                    curFrame = &stackFrames.back();
                    curRoutine = curFrame->routine;
                } else if(callee.type == Value::Type::Object && callee.ptrValue->type == Object::Type::Class) {
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
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Constructor expects {} arguments, but {} were provided", routine->arity - 1, argCount)});
                            return Value::Error;
                        }
                        newFrame.base[0] = Value(instance); // push 'this' as the first argument
                        for(int i = routine->arity - 1; i >= 1; i--) {
                            newFrame.base[i] = POP();
                        }
                        pushFrame(newFrame);
                        curFrame = &stackFrames.back();
                        curRoutine = curFrame->routine;
                    } else {
                        PUSH(Value(instance));
                    }
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Call: callee must be a function"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::GetField: {
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "GetField: object must be a class instance"});
                    return Value::Error;
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
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("GetField: field \"{}\" not found in instance of class \"{}\"", fieldName->data, instance->cls->name->data)});
                    return Value::Error;
                }
                PUSH(fieldValOpt.value());
                break;
            }
            case Opcode::SetField: {
                Value fieldVal = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object || objVal.ptrValue->type != Object::Type::Instance) {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "SetField: object must be a class instance"});
                    return Value::Error;
                }
                Instance* instance = static_cast<Instance*>(objVal.ptrValue);
                uint32_t nameIndex;
                READ_UINT32(nameIndex);
                assert(nameIndex < curRoutine->constants.size());
                Value nameVal = curRoutine->constants[nameIndex];
                assert(nameVal.type == Value::Type::String);
                String* fieldName = nameVal.strValue;
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
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method \"len\" expects 0 arguments, but {} were provided", argCount)});
                            return Value::Error;
                        }
                        PUSH(Value(static_cast<int64_t>(objVal.strValue->length)));
                        break;
                    } else {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("CallMethod: string does not have method \"{}\"", methodName->data)});
                        return Value::Error;
                    }
                } else if(objVal.type == Value::Type::Object) {
                    Object* obj = objVal.ptrValue;
                    switch(obj->type) {
                        case Object::Type::Array: {
                            if(methodName == STRINGS.size) {
                                if(argCount != 0) {
                                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method \"size\" expects 0 arguments, but {} were provided", argCount)});
                                    return Value::Error;
                                }
                                Array* arr = static_cast<Array*>(obj);
                                PUSH(Value(static_cast<int64_t>(arr->size)));
                                break;
                            } else if (methodName == STRINGS.add) {
                                if(argCount != 1) {
                                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method \"add\" expects 1 argument, but {} were provided", argCount)});
                                    return Value::Error;
                                }
                                Array* arr = static_cast<Array*>(obj);
                                Value valueToAdd = POP();
                                arr->add(valueToAdd);
                                PUSH(Value::Null);
                                break;
                            } else {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("CallMethod: array does not have method \"{}\"", methodName->data)});
                                return Value::Error;
                            }
                        }
                        case Object::Type::Map: {
                            if(methodName == STRINGS.size) {
                                if(argCount != 0) {
                                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method \"size\" expects 0 arguments, but {} were provided", argCount)});
                                    return Value::Error;
                                }
                                Map* map = static_cast<Map*>(obj);
                                PUSH(Value(static_cast<int64_t>(map->size)));
                                break;
                            } else {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("CallMethod: map does not have method \"{}\"", methodName->data)});
                                return Value::Error;
                            }
                        }
                        case Object::Type::StrObj: {
                            if(methodName == STRINGS.len) {
                                if(argCount != 0) {
                                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method \"len\" expects 0 arguments, but {} were provided", argCount)});
                                    return Value::Error;
                                }
                                StrObj* strObj = static_cast<StrObj*>(obj);
                                PUSH(Value(static_cast<int64_t>(strObj->length)));
                                break;
                            } else {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("CallMethod: string object does not have method \"{}\"", methodName->data)});
                                return Value::Error;
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
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("CallMethod: method \"{}\" not found in class \"{}\" or its superclasses", methodName->data, instance->cls->name->data)});
                                return Value::Error;
                            }
                            Value methodVal = methodValOpt.value();
                            assert(methodVal.type == Value::Type::Function);
                            Routine* routine = methodVal.funcValue;
                            StackFrame newFrame;
                            newFrame.routine = routine;
                            newFrame.base = stack.top;
                            newFrame.top = newFrame.base + routine->localCount;
                            newFrame.ip = 0;
                            if(argCount != routine->arity - 1) {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method expects {} arguments, but {} were provided", routine->arity - 1, argCount)});
                                return Value::Error;
                            }
                            newFrame.base[0] = objVal; // push 'this' as the first argument
                            for(int i = routine->arity - 1; i >= 1; i--) {
                                newFrame.base[i] = POP();
                            }
                            pushFrame(newFrame);
                            curFrame = &stackFrames.back();
                            curRoutine = curFrame->routine;
                            break;
                        }
                        case Object::Type::Class: {
                            Class* cls = static_cast<Class*>(objVal.ptrValue);
                            auto methodValOpt = cls->methods->get(methodName);
                            if(!methodValOpt.has_value()) {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("CallMethod: method \"{}\" not found in class \"{}\"", methodName->data, static_cast<Class*>(objVal.ptrValue)->name->data)});
                                return Value::Error;
                            }
                            Value methodVal = methodValOpt.value();
                            assert(methodVal.type == Value::Type::Function);
                            Routine* routine = methodVal.funcValue;
                            StackFrame newFrame;
                            newFrame.routine = routine;
                            newFrame.base = stack.top;
                            newFrame.top = newFrame.base + routine->localCount;
                            newFrame.ip = 0;
                            if(argCount != routine->arity) {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Method expects {} arguments (the first is 'this'), but {} were provided", routine->arity, argCount)});
                                return Value::Error;
                            }
                            for(int i = routine->arity - 1; i >= 0; i--) {
                                newFrame.base[i] = POP();
                            }
                            pushFrame(newFrame);
                            curFrame = &stackFrames.back();
                            curRoutine = curFrame->routine;
                            break;
                        }
                        default: {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "CallMethod: object must be an array, map, string or class instance"});
                            return Value::Error;
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
                Class* superClass = instance->cls->base;
                std::optional<Value> methodValOpt;
                while(superClass) {
                    methodValOpt = superClass->methods->get(methodName);
                    if(methodValOpt.has_value()) {
                        break;
                    }
                    superClass = superClass->base;
                }
                if(!methodValOpt.has_value()) {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("SuperCall: method \"{}\" not found in superclass of class \"{}\"", methodName->data, instance->cls->name->data)});
                    return Value::Error;
                }
                Value methodVal = methodValOpt.value();
                assert(methodVal.type == Value::Type::Function);
                Routine* routine = methodVal.funcValue;
                StackFrame newFrame;
                newFrame.routine = routine;
                newFrame.base = stack.top;
                newFrame.top = newFrame.base + routine->localCount;
                newFrame.ip = 0;
                if(argCount != routine->arity - 1) {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Super method expects {} arguments, but {} were provided", routine->arity - 1, argCount)});
                    return Value::Error;
                }
                newFrame.base[0] = objVal; // push 'this' as the first argument
                for(int i = routine->arity - 1; i >= 1; i--) {
                    newFrame.base[i] = POP();
                }
                pushFrame(newFrame);
                curFrame = &stackFrames.back();
                curRoutine = curFrame->routine;
                break;
            }
            case Opcode::IndexGet: {
                Value index = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object) {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexGet: object must be an array or map"});
                    return Value::Error;
                }
                if(objVal.ptrValue->type == Object::Type::Array) {
                    Array* arr = static_cast<Array*>(objVal.ptrValue);
                    if(index.type != Value::Type::Int) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexGet: array index must be an integer"});
                        return Value::Error;
                    }
                    int64_t idx = index.intValue;
                    if(idx < 0 || idx >= arr->size) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexGet: array index out of bounds"});
                        return Value::Error;
                    }
                    PUSH(arr->get(idx));
                } else if(objVal.ptrValue->type == Object::Type::Map) {
                    Map* map = static_cast<Map*>(objVal.ptrValue);
                    // NOTE: map 只能使用常驻字符串来访问, 对象型字符串必须调用内置函数 intern 驻留后访问.
                    if(index.type != Value::Type::String) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexGet: map key must be a string"});
                        return Value::Error;
                    }
                    auto valOpt = map->get(index.strValue);
                    if(!valOpt.has_value()) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("IndexGet: key \"{}\" not found in map", index.strValue->data)});
                        return Value::Error;
                    }
                    PUSH(valOpt.value());
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexGet: object must be an array or map"});
                    return Value::Error;
                }
                break;
            }
            case Opcode::IndexSet: {
                Value value = POP();
                Value index = POP();
                Value objVal = POP();
                if(objVal.type != Value::Type::Object) {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexSet: object must be an array or map"});
                    return Value::Error;
                }
                if(objVal.ptrValue->type == Object::Type::Array) {
                    Array* arr = static_cast<Array*>(objVal.ptrValue);
                    if(index.type != Value::Type::Int) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexSet: array index must be an integer"});
                        return Value::Error;
                    }
                    int64_t idx = index.intValue;
                    if(idx < 0 || idx >= arr->size) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexSet: array index out of bounds"});
                        return Value::Error;
                    }
                    arr->set(idx, value);
                } else if(objVal.ptrValue->type == Object::Type::Map) {
                    Map* map = static_cast<Map*>(objVal.ptrValue);
                    if(index.type != Value::Type::String) {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexSet: map key must be a string"});
                        return Value::Error;
                    }
                    map->set(index.strValue, value);
                } else {
                    errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IndexSet: object must be an array or map"});
                    return Value::Error;
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
                uint8_t builtinId;
                READ_BYTE(builtinId);
                switch(static_cast<Builtin>(builtinId)) {
                    case Builtin::GetIter: {
                        Value objVal = POP();
                        if(objVal.type != Value::Type::Object) {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "GetIter: object must be an array, map or class instance"});
                            return Value::Error;
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
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("GetIter: class \"{}\" does not have an _iter method with 0 arguments", instance->cls->name->data)});
                                return Value::Error;
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
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("GetIter: class \"{}\" does not have an _iter method with 0 arguments", instance->cls->name->data)});
                                return Value::Error;
                            }
                            newFrame.base[0] = objVal; // push 'this' as the first argument
                            pushFrame(newFrame);
                            curFrame = &stackFrames.back();
                            curRoutine = curFrame->routine;
                        } else {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "GetIter: object must be an array, map or class instance"});
                            return Value::Error;
                        }
                        break;
                    }
                    case Builtin::IterNext: {
                        Value iterVal = POP();
                        if(iterVal.type != Value::Type::Object) {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IterNext: object must be an iterator or class instance"});
                            return Value::Error;
                        }
                        if(iterVal.ptrValue->type == Object::Type::Iterator) {
                            Iterator* iter = static_cast<Iterator*>(iterVal.ptrValue);
                            Value nextVal = iter->next();
                            PUSH(nextVal);
                        } else if (iterVal.ptrValue->type == Object::Type::Instance) {
                            Instance* instance = static_cast<Instance*>(iterVal.ptrValue);
                            auto nextMethodOpt = instance->cls->methods->get(STRINGS._next);
                            if(!nextMethodOpt.has_value()) {
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("IterNext: class \"{}\" does not have a _next method with 0 arguments", instance->cls->name->data)});
                                return Value::Error;
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
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("IterNext: class \"{}\" does not have a _next method with 0 arguments", instance->cls->name->data)});
                                return Value::Error;
                            }
                            newFrame.base[0] = iterVal; // push 'this' as the first argument
                            pushFrame(newFrame);
                            curFrame = &stackFrames.back();
                            curRoutine = curFrame->routine;
                        } else {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "IterNext: object must be an iterator or class instance"});
                            return Value::Error;
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
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Intern: argument must be a StrObj"});
                            return Value::Error;
                        }
                        String* internedStr = internString(static_cast<char*>(static_cast<StrObj*>(strVal.ptrValue)->data->getData()));
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
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Int: invalid string format"});
                                return Value::Error;
                            }
                        } else {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Int: unexpected type"});
                            return Value::Error;
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
                                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Float: invalid string format"});
                                return Value::Error;
                            }
                        } else {
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Float: unexpected type"});
                            return Value::Error;
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
                            errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Str: unexpected type"});
                            return Value::Error;
                        }
                        break;
                    }
                    default: {
                        errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Unknown builtin function: {:#04x}", builtinId)});
                        return Value::Error;
                    }
                }
                break;
            }
            case Opcode::Halt: {
                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), "Program halted"});
                return Value::Error;
            }
            default: {
                errorHandler({Error::Type::RuntimeError, getLine(curRoutine, curFrame->ip - 1), std::format("Unknown opcode: {:#04x}", static_cast<int>(opcode))});
                return Value::Error;
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
        errorHandler({Error::Type::RuntimeError, 0, "Stack overflow"});
    }
}

void VM::popFrame() {
    stack.top = stackFrames.back().base;
    stackFrames.pop_back();
}

}
