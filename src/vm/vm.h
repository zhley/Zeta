#pragma once

#include "value.h"
#include "gc.h"

#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <filesystem>
#include <format>

#define ZETA_DEFAULT_STACK_SIZE 1024
#define ZETA_DEFAULT_INIT_HEAP_SIZE 1024
#define ZETA_DEFAULT_MAX_HEAP_SIZE -1

namespace Zeta {

class VM {
public:
    friend class GC;

    struct Config{
        // KiB
        int stackSize;
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
        Routine* routine;
        Value* base; // [0]...[localCount-1]: local variables; [localCount]...[localCount+maxStackSize-1]: operand stack
        Value* top;
        uint32_t ip;
    };
    struct Error{
        enum Type{ RuntimeError, VMError } type;
        uint32_t line;
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

    VM(Config config = {ZETA_DEFAULT_STACK_SIZE, ZETA_DEFAULT_INIT_HEAP_SIZE, ZETA_DEFAULT_MAX_HEAP_SIZE}, ErrorHandler handler = defaultErrorHandler);
    ~VM();

    void setErrorHandler(const ErrorHandler& handler) { errorHandler = handler; }

    void loadModule(const std::string& filePath);
    Value callFunction(const std::string& moduleName, const std::string& funcName, int argc, Value* args);
    Value callFunction(const std::string& funcName, int argc, Value* args); 

private:
    Config config;
    ErrorHandler errorHandler;
    // memory
    Stack stack;
    std::vector<Value> global;
    std::unique_ptr<GC> gc; // manage heap memory
    std::unordered_map<std::string, std::unique_ptr<String>> stringTable; // for string interning.

    std::vector<StackFrame> stackFrames;
    std::vector<std::string> publicModules; // the list of modules that are explicitly loaded by the user, in order of loading
    std::unordered_map<std::string, ModuleInfo> loadedModules; // module path -> module info
    std::vector<std::unique_ptr<Routine>> routines; // [module1.protos[0], module1.protos[1], ..., module2.protos[0], ...]

    // literal strings
    struct Strings {
        String* _iter;
        String* _next;
        String* _equals;
        String* _init;
        String* size;
        String* add;
        String* len;
        String* Null;
        String* Int;
        String* Float;
        String* Bool;
        String* String_;
        String* Object;
        String* Error;
        String* Block;
        String* Array;
        String* Map;
        String* Function;
        String* Class;
        String* Instance;
        String* Iterator;
        String* StrObj;
        String* true_;
        String* false_;
        String* null;
    };
    const Strings STRINGS = {
        internString("_iter"),
        internString("_next"),
        internString("_equals"),
        internString("_init"),
        internString("size"),
        internString("add"),
        internString("len"),
        internString("Null"),
        internString("Int"),
        internString("Float"),
        internString("Bool"),
        internString("String"),
        internString("Object"),
        internString("Error"),
        internString("Block"),
        internString("Array"),
        internString("Map"),
        internString("Function"),
        internString("Class"),
        internString("Instance"),
        internString("Iterator"),
        internString("StrObj"),
        internString("true"),
        internString("false"),
        internString("null")
    };

    void importModule(const std::filesystem::path& path);
    std::filesystem::path searchModuleFile(const std::filesystem::path& basePath, const std::string& moduleName);
    
    Value callFunction(Function* func, int argc, Value* args);
    Value execute();

    String* internString(const std::string& str);
    void pushFrame(const StackFrame& frame);
    void popFrame();

    static void defaultErrorHandler(const Error& error) {
        std::cout << std::format("[{}][line {}]: {}\n", error.type == Error::RuntimeError ? "Runtime Error" : "VM Error", error.line, error.message);
    }

    static uint32_t getLine(const Routine* routine, uint32_t ip) {
        if(routine->lineInfo.empty()) return 0;
        int left = 0, right = routine->lineInfo.size() - 1;
        while(left <= right) {
            int mid = left + (right - left) / 2;
            if(ip >= routine->lineInfo[mid].first && ip < routine->lineInfo[mid + 1].first) {
                return routine->lineInfo[mid].second;
            } else if(ip < routine->lineInfo[mid].first) {
                right = mid - 1;
            } else {
                left = mid + 1;
            }
        }
        return left;
    }
};

}
