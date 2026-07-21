#include "compiler.h"

#include "bytecode.h"
#include "translate.h"
#include "syntax.tab.hpp"
#include "error.h"

#include <format>
#include <iostream>

void yyrestart(FILE*);

namespace Zeta {

std::unique_ptr<Module> compileModule(const std::string& path, std::string* outError){
    FILE* file = fopen(path.c_str(), "r");
    if(!file){
        REPORT_SYSTEM_ERROR("Failed to open file '{}'", path);
        goto ERROR;
    } else {
        yyrestart(file);
        std::unique_ptr<AST::Program> root;
        Parser parser(root);
        parser.parse();
        if(CompileErrorCollector::get().hasErrors()) goto ERROR;
        Translator translator;
        std::unique_ptr<Module> module = translator.translate(root.get());
        if(CompileErrorCollector::get().hasErrors()) goto ERROR;
        fclose(file);
        return module;
    }

    ERROR:
    if(file){
        fclose(file);
    }
    if(outError){
        *outError = CompileErrorCollector::get().getAllAsString();
    }else{
        CompileErrorCollector::get().printAll();
    }
    CompileErrorCollector::get().clear();
    return nullptr;
}

static void printCompileValue(const CompileValue& val, int indent = 2) {
    std::string pad(indent, ' ');
    switch (val.type) {
        case CompileValue::Type::Null:
            std::cout << "null";
            break;
        case CompileValue::Type::Int:
            std::cout << val.intValue;
            break;
        case CompileValue::Type::Float:
            std::cout << val.floatValue;
            break;
        case CompileValue::Type::Bool:
            std::cout << (val.boolValue ? "true" : "false");
            break;
        case CompileValue::Type::String:
            std::cout << std::format("\"{}\"", *val.strValue);
            break;
        case CompileValue::Type::Array:
            std::cout << "[";
            if (val.arrayValue && !val.arrayValue->empty()) {
                std::cout << "\n";
                for (size_t i = 0; i < val.arrayValue->size(); ++i) {
                    std::cout << pad << "  ";
                    printCompileValue((*val.arrayValue)[i], indent + 2);
                    if (i + 1 < val.arrayValue->size()) std::cout << ",";
                    std::cout << "\n";
                }
                std::cout << pad;
            }
            std::cout << "]";
            break;
        case CompileValue::Type::Map:
            std::cout << "{";
            if (val.mapValue && !val.mapValue->empty()) {
                std::cout << "\n";
                for (size_t i = 0; i < val.mapValue->size(); ++i) {
                    std::cout << pad << "  " << (*val.mapValue)[i].first << ": ";
                    printCompileValue((*val.mapValue)[i].second, indent + 2);
                    if (i + 1 < val.mapValue->size()) std::cout << ",";
                    std::cout << "\n";
                }
                std::cout << pad;
            }
            std::cout << "}";
            break;
        case CompileValue::Type::Function:
            std::cout << std::format("<function proto={}>", val.funcValue->protoIndex);
            break;
        case CompileValue::Type::Class:
            std::cout << std::format("<class {}>", val.classValue->name);
            break;
    }
}

static const char* opcodeName(uint8_t op) {
    switch (static_cast<Opcode>(op)) {
        case Opcode::Nop:         return "Nop";
        case Opcode::LoadConst:   return "LoadConst";
        case Opcode::LoadGlobal:  return "LoadGlobal";
        case Opcode::StoreGlobal: return "StoreGlobal";
        case Opcode::LoadVar:     return "LoadVar";
        case Opcode::StoreVar:    return "StoreVar";
        case Opcode::Add:         return "Add";
        case Opcode::Sub:         return "Sub";
        case Opcode::Mul:         return "Mul";
        case Opcode::Div:         return "Div";
        case Opcode::Mod:         return "Mod";
        case Opcode::Neg:         return "Neg";
        case Opcode::BitAnd:      return "BitAnd";
        case Opcode::BitOr:       return "BitOr";
        case Opcode::BitXor:      return "BitXor";
        case Opcode::BitNot:      return "BitNot";
        case Opcode::Shl:         return "Shl";
        case Opcode::Shr:         return "Shr";
        case Opcode::Not:         return "Not";
        case Opcode::Eq:          return "Eq";
        case Opcode::Neq:         return "Neq";
        case Opcode::Lt:          return "Lt";
        case Opcode::Gt:          return "Gt";
        case Opcode::Le:          return "Le";
        case Opcode::Ge:          return "Ge";
        case Opcode::Is:          return "Is";
        case Opcode::Jump:        return "Jump";
        case Opcode::JumpIfFalse: return "JumpIfFalse";
        case Opcode::JumpIfTrue:  return "JumpIfTrue";
        case Opcode::Ret:         return "Ret";
        case Opcode::Call:        return "Call";
        case Opcode::GetField:    return "GetField";
        case Opcode::SetField:    return "SetField";
        case Opcode::CallMethod:  return "CallMethod";
        case Opcode::IndexGet:    return "IndexGet";
        case Opcode::IndexSet:    return "IndexSet";
        case Opcode::Pop:         return "Pop";
        case Opcode::Dup:         return "Dup";
        case Opcode::CallBuiltin: return "CallBuiltin";
        case Opcode::Halt:        return "Halt";
        default:                  return "???";
    }
}

static const char* builtinName(uint8_t id) {
    switch (static_cast<Builtin>(id)) {
        case Builtin::GetIter:  return "GetIter";
        case Builtin::IterNext: return "IterNext";
        case Builtin::NewArray: return "NewArray";
        case Builtin::NewMap:   return "NewMap";
        case Builtin::Print:    return "Print";
        case Builtin::Input:    return "Input";
        case Builtin::Error:    return "Error";
        case Builtin::Check:    return "Check";
        case Builtin::Intern:   return "Intern";
        default:                return "???";
    }
}

void printModule(const Module *module, const std::string &path){
    std::cout << std::format("=== Module: {} ===\n\n", path);

    // ── Imports ──
    std::cout << std::format("Imports ({}):\n", module->imports.size());
    for (size_t i = 0; i < module->imports.size(); ++i) {
        std::cout << std::format("  [{}] \"{}\"\n", i, module->imports[i]);
    }
    std::cout << "\n";

    // ── Global Symbols ──
    std::cout << std::format("Global Symbols ({}):\n", module->globalSyms.size());
    for (const auto& [name, sym] : module->globalSyms) {
        std::cout << std::format("  {}: {{\n", name);
        std::cout << std::format("    mutable: {}\n", sym.isMutable);
        std::cout << "    initValue: ";
        printCompileValue(sym.initValue, 4);
        std::cout << "\n";
        if (!sym.relocations.empty()) {
            std::cout << std::format("    relocations ({}):\n", sym.relocations.size());
            for (const auto& pos : sym.relocations) {
                std::cout << std::format("      proto={}, offset={}\n", pos.protoIndex, pos.bytecodeOffset);
            }
        }
        std::cout << "  }\n";
    }
    std::cout << "\n";

    // ── External Symbols ──
    std::cout << std::format("External Symbols ({}):\n", module->externalSyms.size());
    for (const auto& esym : module->externalSyms) {
        std::cout << std::format("  {}.{}:\n", esym.moduleName.empty() ? "(any)" : esym.moduleName, esym.name);
        for (const auto& pos : esym.relocations) {
            std::cout << std::format("    proto={}, offset={}\n", pos.protoIndex, pos.bytecodeOffset);
        }
    }
    std::cout << "\n";

    // ── Protos ──
    std::cout << std::format("Protos ({}):\n", module->protos.size());
    for (const auto& proto : module->protos) {
        std::cout << std::format("  Proto[{}]:\n", proto->index);
        std::cout << std::format("    arity: {}\n", proto->arity);
        std::cout << std::format("    localCount: {}\n", proto->localCount);
        std::cout << std::format("    maxStackSize: {}\n", proto->maxStackSize);

        // Constants
        std::cout << std::format("    constants ({}):\n", proto->constants.size());
        for (size_t i = 0; i < proto->constants.size(); ++i) {
            std::cout << std::format("      [{}] ", i);
            printCompileValue(proto->constants[i], 8);
            std::cout << "\n";
        }

        // Bytecode
        std::cout << std::format("    bytecode ({} bytes):\n", proto->bytecode.size());
        uint32_t ip = 0;
        const auto& bc = proto->bytecode;
        while (ip < bc.size()) {
            uint8_t op = bc[ip];
            std::cout << std::format("      {:04x}  {}", ip, opcodeName(op));

            switch (static_cast<Opcode>(op)) {
                case Opcode::LoadConst:
                case Opcode::LoadGlobal:
                case Opcode::StoreGlobal:
                case Opcode::LoadVar:
                case Opcode::StoreVar:
                case Opcode::GetField:
                case Opcode::SetField: {
                    uint32_t val;
                    std::memcpy(&val, &bc[ip + 1], 4);
                    std::cout << std::format(" {}", val);
                    ip += 4;
                    break;
                }
                case Opcode::Jump:
                case Opcode::JumpIfFalse:
                case Opcode::JumpIfTrue: {
                    uint32_t target;
                    std::memcpy(&target, &bc[ip + 1], 4);
                    std::cout << std::format(" -> {:04x}", target);
                    ip += 4;
                    break;
                }
                case Opcode::Call: {
                    uint8_t cnt = bc[ip + 1];
                    std::cout << std::format(" argc={}", cnt);
                    ip += 1;
                    break;
                }
                case Opcode::CallMethod: {
                    uint8_t cnt = bc[ip + 1];
                    uint32_t nameIdx;
                    std::memcpy(&nameIdx, &bc[ip + 2], 4);
                    std::cout << std::format(" argc={} nameIdx={}", cnt, nameIdx);
                    ip += 5;
                    break;
                }
                case Opcode::CallBuiltin: {
                    uint8_t id = bc[ip + 1];
                    std::cout << std::format(" {}", builtinName(id));
                    ip += 1;
                    break;
                }
                default:
                    break;
            }

            std::cout << "\n";
            ip += 1;
        }
        std::cout << "\n";
    }
}

}
