#include "vm.h"

#include "compiler/bytecode.h"
#include "compiler/compiler.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <system_error>

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
            errorHandler({Error::Type::VMError, "Invalid module search path: " + path + " <" + ec.message() + ">"});
        }
    }
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
        errorHandler({Error::Type::VMError, "Invalid module file path: " + filePath + " <" + ec.message() + ">"});
        return;
    } else if(!std::filesystem::is_regular_file(path)) {
        errorHandler({Error::Type::VMError, filePath + " is not a regular file"});
        return;
    }
    importModule(path);
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
    for(auto& proto : module->protos) {
        auto routine = std::make_unique<Routine>();
        routine->bytecode = std::move(proto->bytecode);
        for(const auto& constVal : proto->constants) {
            routine->constants.push_back(makeValue(constVal, moduleInfo.protoBaseIndex));
        }
        routine->arity = proto->arity;
        routine->localCount = proto->localCount;
        routine->maxStackSize = proto->maxStackSize;
        routines.push_back(std::move(routine));
    }
    for(const auto& [symName, sym] : module->globalSyms) {
        uint32_t index = global.size();
        global.push_back(makeValue(sym.initValue, moduleInfo.protoBaseIndex));
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
            errorHandler({Error::Type::VMError, "Failed to find imported module: " + import + " imported by " + pathStr});
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
                errorHandler({Error::Type::VMError, "Failed to resolve external symbol: " + extSym.name + " in module " + pathStr});
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
                errorHandler({Error::Type::VMError, "Failed to resolve external symbol: " + extSym.name + " in module " + pathStr + " , module " + extSym.moduleName + " does not define the symbol"});
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

Value VM::makeValue(const CompileValue& compileValue, uint32_t moduleProtoBaseIndex) {
    switch (compileValue.type) {
        case CompileValue::Type::Null: return Value();
        case CompileValue::Type::Int: return Value(compileValue.intValue);
        case CompileValue::Type::Float: return Value(compileValue.floatValue);
        case CompileValue::Type::Bool: return Value(compileValue.boolValue);
        case CompileValue::Type::String: return Value(internString(*(compileValue.strValue)));
        
        // TODO: ...
        default: assert(false);
    }
    return Value();
}

}
