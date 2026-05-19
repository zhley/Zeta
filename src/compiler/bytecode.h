#pragma once

#include "vm/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace Zeta {

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
    // Comparison
    Eq          = 0x20,
    Neq         = 0x21,
    Lt          = 0x22,
    Gt          = 0x23,
    Le          = 0x24,
    Ge          = 0x25,
    // Control Flow
    Jump        = 0x30,
    JumpIfTrue  = 0x31,
    Ret         = 0x32,
    Call        = 0x33,
    // Object
    NewInstance = 0x40,
    GetField    = 0x41, 
    SetField    = 0x42,
    CallMethod  = 0x43,
    IndexGet    = 0x44,
    IndexSet    = 0x45,
    // Misc
    Pop         = 0x50,
    Dup         = 0x51,
    CallBuiltin = 0x52,
    Halt        = 0xFF
};

struct Proto {
    std::vector<uint8_t> bytecode;
    std::vector<Value> constants;
    int arity = 0;
    int localCount = 0;
    int maxStackSize = 0;
};

struct Symbol{
    bool external; 
    bool isMutable; // for non-external
    Value initValue; // for non-external
    std::string moduleName; // for external

    struct Pos{
        uint32_t protoIndex;
        uint32_t bytecodeOffset;
    };
    std::vector<Pos> relocations;
};

struct Module{
    std::vector<std::string> imports;
    std::unordered_map<std::string, Symbol> globalSyms;
    std::vector<std::unique_ptr<Proto>> protos;
};

} // namespace Zeta
