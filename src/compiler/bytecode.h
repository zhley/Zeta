#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace Zeta {

struct CompileFunction;
struct CompileClass;
struct CompileValue;

// TODO: 加入一些 push 常量的指令提高效率, 比如PushInt, PushFloat, PushNull
enum class Opcode : uint8_t {
    Nop         = 0x00,
    // Mem
    LoadConst   = 0x01,
    LoadGlobal  = 0x02,
    StoreGlobal = 0x03,
    LoadVar     = 0x04,
    StoreVar    = 0x05,
    // Arithmetic
    Add         = 0x10,
    Sub         = 0x11,
    Mul         = 0x12,
    Div         = 0x13,
    Mod         = 0x14,
    Neg         = 0x15,
    BitAnd      = 0x16,
    BitOr       = 0x17,
    BitXor      = 0x18,
    BitNot      = 0x19,
    Shl         = 0x1A,
    Shr         = 0x1B,
    Not         = 0x1C,
    // Comparison
    Eq          = 0x20,
    Neq         = 0x21,
    Lt          = 0x22,
    Gt          = 0x23,
    Le          = 0x24,
    Ge          = 0x25,
    // Control Flow
    Jump        = 0x30,
    JumpIfFalse = 0x31,
    JumpIfTrue  = 0x32,
    Ret         = 0x33,
    Call        = 0x34,
    // Object
    GetField    = 0x40, 
    SetField    = 0x41,
    CallMethod  = 0x42,
    IndexGet    = 0x43,
    IndexSet    = 0x44,
    // Misc
    Pop         = 0x50,
    Dup         = 0x51,
    CallBuiltin = 0x52,
    Halt        = 0xFF
};

// NOTE: 内置函数必须有编译期确定的操作数栈深度增量
enum class Builtin : uint8_t {
    GetIter     = 0x00,
    IterNext    = 0x01,
    NewArray    = 0x02,
    NewMap      = 0x03,
    Print       = 0x04,
    Input       = 0x05,
    Error       = 0x06,
};

struct BuiltinDesc {
    Builtin id;
    std::string funcName;
    int stackDelta;
};

inline constexpr BuiltinDesc builtinTable[] = {
    {Builtin::GetIter, "iter", 0},
    {Builtin::IterNext, "next", 0},
    {Builtin::NewArray, "array", 0},
    {Builtin::NewMap, "map", 0},
    {Builtin::Print, "print", -1},
    {Builtin::Input, "input", 1},
    {Builtin::Error, "error", 1},
};
inline constexpr int builtinTableSize = sizeof(builtinTable) / sizeof(BuiltinDesc);

struct CompileFunction {
    uint32_t protoIndex;
};

struct CompileClass {
    std::string name;
    std::pair<std::string, std::string> base; // <module_name, class_name>
    std::unordered_map<std::string, CompileValue> fields;
    std::unordered_map<std::string, CompileFunction> methods;
};

struct CompileValue{
    enum class Type : uint8_t {
        Null = 0x00,
        Int,
        Float,
        Bool,
        String,
        Array,
        Map,
        Function,
        Class
    } type;
    union {
        int64_t intValue;
        double floatValue;
        bool boolValue;
        std::string* strValue;
        std::vector<CompileValue>* arrayValue;
        std::vector<std::pair<std::string, CompileValue>>* mapValue;
        CompileFunction* funcValue;
        CompileClass* classValue;
    };

    CompileValue() : type(Type::Null) {}
    CompileValue(int64_t i) : type(Type::Int), intValue(i) {}
    CompileValue(double f) : type(Type::Float), floatValue(f) {}
    CompileValue(bool b) : type(Type::Bool), boolValue(b) {}
    CompileValue(const std::string& s) : type(Type::String), strValue(new std::string(s)) {}
    CompileValue(std::vector<CompileValue>* arr) : type(Type::Array), arrayValue(arr) {}
    CompileValue(std::vector<std::pair<std::string, CompileValue>>* m) : type(Type::Map), mapValue(m) {}
    CompileValue(CompileFunction* f) : type(Type::Function), funcValue(f) {}
    CompileValue(CompileClass* c) : type(Type::Class), classValue(c) {}
    ~CompileValue() {
        switch (type) {
            case Type::String:      delete strValue; break;
            case Type::Array:       delete arrayValue; break;
            case Type::Map:         delete mapValue; break;
            case Type::Function:    delete funcValue; break;
            case Type::Class:       delete classValue; break;
            default: break;
        }
    }
    CompileValue(const CompileValue& other) {
        type = other.type;
        switch (type) {
            case Type::Int:         intValue = other.intValue; break;
            case Type::Float:       floatValue = other.floatValue; break;
            case Type::Bool:        boolValue = other.boolValue; break;
            case Type::String:      strValue = new std::string(*other.strValue); break;
            case Type::Array:       arrayValue = new std::vector<CompileValue>(*other.arrayValue); break;
            case Type::Map:         mapValue = new std::vector<std::pair<std::string, CompileValue>>(*other.mapValue); break;
            case Type::Function:    funcValue = new CompileFunction(*other.funcValue); break;
            case Type::Class:       classValue = new CompileClass(*other.classValue); break;
            default: break;
        }
    }
    CompileValue& operator=(const CompileValue& other) {
        if (this == &other) return *this;
        this->~CompileValue();
        new (this) CompileValue(other);
        return *this;
    }
    CompileValue(CompileValue&& other) noexcept : type(other.type) {
        switch (type) {
            case Type::Int:         intValue = other.intValue; break;
            case Type::Float:       floatValue = other.floatValue; break;
            case Type::Bool:        boolValue = other.boolValue; break;
            case Type::String:      strValue = other.strValue; other.strValue = nullptr; break;
            case Type::Array:       arrayValue = other.arrayValue; other.arrayValue = nullptr; break;
            case Type::Map:         mapValue = other.mapValue; other.mapValue = nullptr; break;
            case Type::Function:    funcValue = other.funcValue; other.funcValue = nullptr; break;
            case Type::Class:       classValue = other.classValue; other.classValue = nullptr; break;
            default: break;
        }
    }
    CompileValue& operator=(CompileValue&& other) noexcept {
        if (this == &other) return *this;
        this->~CompileValue();
        new (this) CompileValue(std::move(other));
        return *this;
    }
    bool operator==(const CompileValue& other) const {
        if (type != other.type) return false;
        switch (type) {
            case Type::Null:        return true;
            case Type::Int:         return intValue == other.intValue;
            case Type::Float:       return floatValue == other.floatValue;
            case Type::Bool:        return boolValue == other.boolValue;
            case Type::String:      return *strValue == *other.strValue;
            case Type::Array:       return *arrayValue == *other.arrayValue;
            case Type::Map:         return *mapValue == *other.mapValue;
            case Type::Function:    return funcValue->protoIndex == other.funcValue->protoIndex;
            case Type::Class:       return classValue->name == other.classValue->name;
            default: return false;
        }
    }
};

struct Proto {
    uint32_t index;
    std::vector<uint8_t> bytecode;
    std::vector<CompileValue> constants;
    int arity = 0;
    int localCount = 0;
    int maxStackSize = 0; // max operand stack size
};

struct Pos{
    uint32_t protoIndex;
    uint32_t bytecodeOffset;
};

struct Symbol{
    bool isMutable;
    CompileValue initValue;
    std::vector<Pos> relocations;
};

struct ExtSymbol{
    std::string name;
    std::string moduleName;
    std::vector<Pos> relocations;

    bool operator==(const ExtSymbol& other) const {
        return name == other.name && moduleName == other.moduleName;
    }
};

struct Module{
    std::vector<std::string> imports;
    std::unordered_map<std::string, Symbol> globalSyms;
    std::vector<ExtSymbol> externalSyms;
    std::vector<std::unique_ptr<Proto>> protos;
};

} // namespace Zeta
