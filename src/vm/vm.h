#pragma once

#include "value.h"
#include "gc.h"

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <filesystem>
#include <format>

/// 默认操作数栈大小（单位：KiB）
#define ZETA_DEFAULT_STACK_SIZE 1024
/// 默认初始堆大小（单位：KiB）
#define ZETA_DEFAULT_INIT_HEAP_SIZE 1024
/// 默认最大堆大小（单位：KiB），-1 表示无限制
#define ZETA_DEFAULT_MAX_HEAP_SIZE -1

namespace Zeta {

struct Module;

/**
 * @brief 栈式字节码虚拟机。
 *
 * 负责加载与链接模块、执行字节码、管理全局符号表和 GC 托管对象，
 * 并提供与 C++ 宿主代码互操作的接口（注册原生函数/类、通过 VM 栈交换数据）。
 */
class VM {
public:
    friend class GC;

    /**
     * @brief VM 运行时配置。
     */
    struct Config{
        int stackSize;                              ///< 操作数栈大小（单位：KiB）
        int initHeapSize;                           ///< 初始堆大小（单位：KiB）
        int maxHeapSize;                            ///< 最大堆大小（单位：KiB），-1 表示无限制
        std::vector<std::string> moduleSearchPaths; ///< 模块搜索路径列表
    };

    /**
     * @brief 操作数栈的底层表示。
     */
    struct Stack{
        Value* base;    ///< 栈底指针
        Value* top;     ///< 栈顶指针（指向下一个可用槽位）
        int capacity;   ///< 栈容量（以 Value 个数计）
    };

    /**
     * @brief 函数调用栈帧。
     *
     * 局部变量存放在 [0..localCount-1] 槽位，操作数栈存放在
     * [localCount..localCount+maxStackSize-1] 槽位。
     */
    struct StackFrame{
        Routine* routine; ///< 当前帧对应的函数
        Value* base;      ///< 帧基址：局部变量区与操作数栈区的起始
        Value* top;       ///< 操作数栈顶指针
        uint32_t ip;      ///< 字节码指令指针
    };

    /**
     * @brief 运行时/VM 错误信息。
     */
    struct Error{ 
        /// 错误类别：RuntimeError 为运行时错误，VMError 为 VM 内部错误。
        enum Type{ RuntimeError, VMError } type; ///< 错误类型
        std::string moduleName; ///< 出错模块名
        uint32_t line;          ///< 出错行号
        std::string message;    ///< 错误描述信息
    };

    /**
     * @brief 模块的运行时元信息（编译期数据到运行时数据的映射）。
     */
    struct ModuleInfo{
        std::unordered_map<std::string, uint32_t> symbolMap; ///< 全局符号名 → 全局索引
        uint32_t protoBaseIndex; ///< 该模块各 proto 在 VM 的 routines 列表中的基索引

        /**
         * @brief 将模块内的 proto 索引转换为 VM 级的 routine 索引。
         * @param protoIndex 模块内的 proto 索引
         * @return VM 级 routine 索引（protoBaseIndex + protoIndex）
         */
        uint32_t getRoutineIndex(uint32_t protoIndex) const {
            return protoBaseIndex + protoIndex;
        }
    };

    /// 错误处理器回调类型。
    using ErrorHandler = std::function<void(const Error&)>;

    /**
     * @brief 构造 VM 实例。
     * @param config 运行时配置
     * @param handler 错误处理器回调，默认使用 defaultErrorHandler
     */
    VM(Config config = {ZETA_DEFAULT_STACK_SIZE, ZETA_DEFAULT_INIT_HEAP_SIZE, ZETA_DEFAULT_MAX_HEAP_SIZE, {}}, ErrorHandler handler = defaultErrorHandler);

    /// 析构函数，释放操作数栈等资源。
    ~VM();

    /**
     * @brief 设置错误处理器。
     * @param handler 新的错误处理器回调
     */
    void setErrorHandler(const ErrorHandler& handler) { errorHandler = handler; }

    // TODO: 加入卸载模块的支持

    /**
     * @brief 加载一个已编译的模块，并递归加载其 imports 依赖。
     * @param module 编译期模块对象
     */
    void loadModule(const Module* module);

    // NOTE: 以下是对 C++ 和 Zeta 交互接口的一些说明. 具体使用示例可参看 tests/cpp/
    // - C++ 和 Zeta 通过 VM 栈进行数据交换.
    // - 编写 C++ 端代码时, 对于对象类型的值, 不要缓存在 Value 类型的变量中, 除非确信其生命周期内不会触发 GC, 否则内部指针可能会失效. 替代的缓存方案:
    //      对于 Zeta 全局变量, 可以安全地缓存其索引(index), 然后通过 getGlobal 和 setGlobal 来访问.
    //      对于 Zeta 栈上的变量, 可以缓存指向它的 Value 指针(Value*), 需要注意变量的生命周期, 避免变成悬空指针.
    //      对于 Zeta 临时根, 指向它的 Value* 也可以在其生命周期内安全地缓存.
    // - 原生函数接受两个参数, VM 指针和参数个数, 实际参数从当前帧上获取, 返回值通过 push() 压入当前帧.
    //      原生函数没有自己独立的栈帧, 而是寄生在调用者栈帧上, 必须将全部 argc 个参数弹出, 并压入一个返回值.
    //      函数体内需要进行参数个数校验, 值得注意的是, 虽然目前 Zeta 语法设计上不支持函数重载, 但是原生函数可以通过参数个数分发不同实现, 以达到重载效果.
    // - 如果方法是原生函数, argc = 方法形参个数 + 1, 换句话说, 不管是普通函数还是方法, 原生函数接受的 argc 都是实际传参个数.
    //      原生函数方法调用时, 实例对象是作为最后一个参数传入的, 也就是说栈顶是实例对象, 这与普通方法传参约定不同, 需要格外注意.
    //      比如: inst.say(x, y, z) 调用时, 如果 say 是原生函数, 那么 argc = 4, 从栈顶到底依次是 inst, z, y, x;
    // - C++ 调用 Zeta 方法, 参数准备时, 先依次压入参数, 最后压入实例对象, 然后调用 callMethod.

    /**
     * @brief 注册一个原生函数为全局符号。
     * @param name 全局符号名
     * @param func 原生函数指针
     * @return 该符号的全局索引
     */
    int registerFunction(const std::string& name, NativeFunction func);

    /**
     * @brief 注册一个原生类。
     * @param name 类名
     * @param fields 字段名与默认值列表
     * @param methods 方法名与原生函数列表
     * @return 该类的全局索引
     */
    int registerClass(const std::string& name, const std::vector<std::pair<std::string, Value>>& fields, const std::vector<std::pair<std::string, NativeFunction>>& methods);

    /**
     * @brief 将值压入当前帧的操作数栈，必要时扩展栈顶。
     * @param val 要压入的值
     * @return 指向新压入元素所在槽位的指针
     */
    Value* push(Value val);

    /**
     * @brief 弹出当前帧操作数栈顶的值。
     * @return 弹出的值
     * @note 调用方需保证栈非空，弹出过头会破坏栈结构。
     */
    Value pop();

    /**
     * @brief 从当前帧操作数栈顶弹出指定个数的值。
     * @param count 弹出的元素个数
     */
    void pop(int count);

    /**
     * @brief 查看当前帧操作数栈中相对栈顶偏移 offset 处的值（不弹出）。
     * @param offset 偏移量，必须 <= -1（-1 为栈顶元素，-2 为次顶元素，依此类推）
     * @return 指向该槽位的指针
     * @note 访问不属于自己的槽位是未定义行为，offset 不可过小。
     */
    Value* peek(int offset);

    /**
     * @brief 按模块名和符号名查找全局符号索引。
     * @param moduleName 模块名；为空时在已注册的原生符号中查找
     * @param globalName 全局符号名
     * @return 全局索引；未找到时报告错误并返回 -1
     */
    int findGlobal(const std::string& moduleName, const std::string& globalName);

    /**
     * @brief 按索引读取全局变量值。
     * @param index 全局索引
     * @return 全局变量值；索引越界时报告错误并返回 Value::Error
     */
    Value getGlobal(int index);

    /**
     * @brief 按索引写入全局变量值。
     * @param index 全局索引
     * @param val 要写入的值
     */
    void setGlobal(int index, Value val);

    /**
     * @brief 调用当前帧栈顶的函数值。
     *
     * 调用约定：栈上先依次压入实参，最后压入函数值；本函数弹出函数值并调用。
     * 返回值压入栈顶。
     * 支持原生函数与 Zeta 函数两种类型。
     *
     * @param argc 实参个数
     */
    void call(int argc);

    /**
     * @brief 调用当前帧栈顶实例对象的方法。
     *
     * 调用约定：先依次压入实参，最后压入实例对象（栈顶为实例）。
     * 返回值压入栈顶。
     *
     * @param methodName 方法名
     * @param argc 实参个数
     */
    void callMethod(const std::string& methodName, int argc);

    /**
     * @brief 调用当前帧栈顶实例对象的方法（驻留字符串版本）。
     * @param methodName 已驻留的方法名字符串
     * @param argc 实参个数
     */
    void callMethod(String* methodName, int argc);

    /**
     * @brief 以当前帧栈顶的 Class 为模板创建实例，若定义了 _init 则调用构造函数，创建好的实例压入栈顶。
     * @param argc 传给构造函数的实参个数
     */
    void newInstance(int argc);

    /**
     * @brief 创建一个空数组并压入当前帧栈顶。
     */
    void newArray();

    /**
     * @brief 创建一个指定大小的数组并压入当前帧栈顶。
     * @param size 数组大小
     */
    void newArray(int size);

    /**
     * @brief 创建一个空 Map 并压入当前帧栈顶。
     */
    void newMap();

    /**
     * @brief 创建一个字符串对象（StrObj）并压入当前帧栈顶。
     * @param str 字符串内容
     */
    void newStrObj(std::string_view str);

    /**
     * @brief 将 C++ 原始指针包装成一个实例对象（带 _cpp_ptr 字段），并压入栈顶。
     * @param ptr C++ 原始指针
     * @param class_ 用作包装的类，原则上该类必须具有 _cpp_ptr 字段
     */
    void wrapPointer(void* ptr, Value class_);

    /**
     * @brief 从栈顶实例对象中解包出 C++ 原始指针。
     * @return 解包出的指针；类型不符或缺少 _cpp_ptr 字段时报告错误并返回 nullptr
     */
    void* unwrapPointer();

    /**
     * @brief 弹出栈顶值并将其作为临时 GC 根保存，防止其被垃圾回收。
     * @return 指向该临时根所存值的指针
     */
    Value* pushTempRoot();

    /**
     * @brief 移除一个临时 GC 根。
     * @param val 由 pushTempRoot 返回的指针
     */
    void popTempRoot(Value* val);

    /**
     * @brief 将字符串驻留（interning），返回驻留的不可变 String。
     * @param str 字符串内容
     * @return 驻留字符串指针
     */
    String* internString(const std::string& str);

    /**
     * @brief 将 StrObj 的内容驻留为不可变 String。
     * @param strObj 字符串对象
     * @return 驻留字符串指针
     */
    String* internString(StrObj* strObj);

    /**
     * @brief 将错误报告给当前错误处理器。
     * @param message 错误描述
     * @param type 错误类型，默认 RuntimeError
     * @note 外部调用只有能力报告运行时错误，第二个参数一般不需要传入。
     */
    void reportError(const std::string& message, Error::Type type = Error::Type::RuntimeError) {
        Routine* currentRoutine = stackFrames.empty() ? nullptr : stackFrames.back().routine;
        if (currentRoutine) {
            uint32_t currentIP = stackFrames.back().ip - 1;
            uint32_t line = getLine(currentRoutine, currentIP);
            errorHandler({type, currentRoutine->moduleName, line, message});
        } else {
            errorHandler({type, "<unknown>", 0, message});
        }
    }

private:
    Config config;
    ErrorHandler errorHandler;
    // memory
    Stack stack;
    std::vector<Value> global;
    std::unique_ptr<GC> gc; // manage heap memory
    std::unordered_map<std::string, std::unique_ptr<String>> stringTable; // for string interning.

    std::vector<StackFrame> stackFrames;
    std::unordered_map<std::string, ModuleInfo> loadedModules; // module name -> module info
    std::unordered_map<std::string, uint32_t> registeredSyms;
    std::vector<std::unique_ptr<Routine>> routines; // [module1.protos[0], module1.protos[1], ..., module2.protos[0], ...]
    std::vector<std::unique_ptr<Value>> tempRoots;

    void importModule(const Module* module);
    void importModule(const std::filesystem::path& path, bool isSrcFile);
    std::pair<bool, std::filesystem::path> searchModuleFile(const std::filesystem::path& basePath, const std::string& moduleName);
    
    void call(Routine* func, int argc);
    void execute();

    StackFrame* pushFrame(const StackFrame& frame);
    void popFrame();

    static void defaultErrorHandler(const Error& error) {
        if (error.type == Error::RuntimeError) {
            std::cout << std::format("[Runtime Error][line {} in {}]: {}\n", error.line, error.moduleName, error.message);
        } else {
            std::cout << std::format("[VM Error]: {}\n", error.message);
        }
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

public:
    /**
     * @brief VM 内部驻留的协议方法名与内置类型名字符串集合。
     */
    struct Strings {
        String* _iter;    ///< 迭代器协议方法名 "_iter"
        String* _next;    ///< 迭代器协议方法名 "_next"
        String* _equals;  ///< 相等协议方法名 "_equals"
        String* _init;    ///< 构造方法名 "_init"
        String* size;     ///< 内置属性名 "size"
        String* add;      ///< 内置方法名 "add"
        String* len;      ///< 内置属性名 "len"
        String* Null;     ///< 内置类型名 "Null"
        String* Int;      ///< 内置类型名 "Int"
        String* Float;    ///< 内置类型名 "Float"
        String* Bool;     ///< 内置类型名 "Bool"
        String* String_;  ///< 内置类型名 "String"
        String* Object;   ///< 内置类型名 "Object"
        String* Error;    ///< 内置类型名 "Error"
        String* Block;    ///< 内置类型名 "Block"
        String* Array;    ///< 内置类型名 "Array"
        String* Map;      ///< 内置类型名 "Map"
        String* Function; ///< 内置类型名 "Function"
        String* Class;    ///< 内置类型名 "Class"
        String* Instance; ///< 内置类型名 "Instance"
        String* Iterator; ///< 内置类型名 "Iterator"
        String* StrObj;   ///< 内置类型名 "StrObj"
        String* true_;    ///< 内置字面量 "true"
        String* false_;   ///< 内置字面量 "false"
        String* null;     ///< 内置字面量 "null"
        String* _cpp_ptr; ///< C++ 互操作指针字段名 "_cpp_ptr"
    };
    /// 已驻留的协议方法名/类型名字符串实例。
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
        internString("null"),
        internString("_cpp_ptr")
    };
};

/**
 * @brief VM 自身无法处理的严重错误，抛出该异常供 VM 使用者捕获处理。
 */
class VMException : public std::exception {
public:
    /**
     * @brief 严重错误类型。
     */
    enum class Type { 
        StackOverflow,      ///< 栈溢出
        HeapLimitExceeded,  ///< 堆大小超过最大堆限制
        OutOfMemory         ///< 内存耗尽
    };

    /**
     * @brief 构造 VM 异常。
     * @param type 错误类型
     */
    explicit VMException(Type type) : type(type) {}

    /**
     * @brief 返回错误描述文本。
     * @return 以空字符结尾的错误描述字符串
     */
    const char* what() const noexcept override {
        switch(type) {
            case Type::StackOverflow:       return "VMException: Stack overflow";
            case Type::HeapLimitExceeded:   return "VMException: Heap limit exceeded";
            case Type::OutOfMemory:         return "VMException: Out of memory";
            default:                        return "VMException: Unknown error";
        }
    }

    /**
     * @brief 返回错误类型。
     * @return 错误类型
     */
    Type getType() const noexcept { return this->type; }

private:
    Type type;
};

}
