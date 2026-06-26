#pragma once

#include "value.h"
#include "compiler/bytecode.h"
#include "gc.h"

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <filesystem>

#define ZETA_DEFAULT_INIT_STACK_SIZE 1024
#define ZETA_DEFAULT_INIT_HEAP_SIZE 1024
#define ZETA_DEFAULT_MAX_STACK_SIZE -1
#define ZETA_DEFAULT_MAX_HEAP_SIZE -1

namespace Zeta {

class VM {
public:
    struct Config{
        // KiB
        int initStackSize;
        int maxStackSize; // -1 for unlimited
        int initHeapSize;
        int maxHeapSize;
        std::vector<std::string> moduleSearchPaths; 
    };
    struct Stack{
        Value* base;
        Value* top;
        int capacity;
    };
    struct StackFrame{
        Proto* proto;
        Value* base; // [0]...[localCount-1]: local variables; [localCount]...[localCount+maxStackSize-1]: operand stack
        uint32_t ip;
    };
    struct Error{
        enum Type{ RuntimeError, VMError } type;
        std::string message;
    };
    // for each module. Compile-time data -> Runtime data
    struct ModuleInfo{
        std::unordered_map<std::string, uint32_t> symbolMap;    // global symbol name -> global index
        uint32_t protoBaseIndex; // the base index of the module's protos in the VM's routine list

        uint32_t getRoutineIndex(uint32_t protoIndex) const {
            return protoBaseIndex + protoIndex;
        }
    };
    using ErrorHandler = std::function<void(const Error&)>;

    VM(Config config = {ZETA_DEFAULT_INIT_STACK_SIZE, ZETA_DEFAULT_MAX_STACK_SIZE, ZETA_DEFAULT_INIT_HEAP_SIZE, ZETA_DEFAULT_MAX_HEAP_SIZE}, ErrorHandler handler = defaultErrorHandler);
    ~VM();

    void setErrorHandler(const ErrorHandler& handler) { errorHandler = handler; }

    void loadModule(const std::string& filePath);

private:
    Config config;
    ErrorHandler errorHandler;
    // memory
    Stack stack;
    std::vector<Value> global;
    std::unique_ptr<GC> gc; // manage heap memory
    std::unordered_map<std::string, std::unique_ptr<String>> stringTable; // for string interning.

    std::vector<StackFrame> stackFrames;
    std::unordered_map<std::string, ModuleInfo> loadedModules; // module path -> module info
    std::vector<std::unique_ptr<Routine>> routines; // [module1.protos[0], module1.protos[1], ..., module2.protos[0], ...]

    void importModule(const std::filesystem::path& path);
    std::filesystem::path searchModuleFile(const std::filesystem::path& basePath, const std::string& moduleName);
    
    String* internString(const std::string& str);
    
    Value makeValue(const CompileValue& compileValue, uint32_t moduleProtoBaseIndex);

    static void defaultErrorHandler(const Error& error) {
        printf("%s: %s\n", error.type == Error::RuntimeError ? "Runtime Error" : "VM Error", error.message.c_str());
    }
};

}
