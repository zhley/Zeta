#include "compiler.h"

#include "bytecode.h"
#include "translate.h"
#include "syntax.tab.hpp"
#include "error.h"

#include <format>
#include <fstream>
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
        {
            printModule(module.get(), path + ".dump");
        }
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

static void printCompileValue(std::ostream& os, const CompileValue& val, int indent = 2) {
    std::string pad(indent, ' ');
    switch (val.type) {
        case CompileValue::Type::Null:
            os << "null";
            break;
        case CompileValue::Type::Int:
            os << val.intValue;
            break;
        case CompileValue::Type::Float:
            os << val.floatValue;
            break;
        case CompileValue::Type::Bool:
            os << (val.boolValue ? "true" : "false");
            break;
        case CompileValue::Type::String:
            os << std::format("\"{}\"", *val.strValue);
            break;
        case CompileValue::Type::Array:
            os << "[";
            if (val.arrayValue && !val.arrayValue->empty()) {
                os << "\n";
                for (size_t i = 0; i < val.arrayValue->size(); ++i) {
                    os << pad << "  ";
                    printCompileValue(os, (*val.arrayValue)[i], indent + 2);
                    if (i + 1 < val.arrayValue->size()) os << ",";
                    os << "\n";
                }
                os << pad;
            }
            os << "]";
            break;
        case CompileValue::Type::Map:
            os << "{";
            if (val.mapValue && !val.mapValue->empty()) {
                os << "\n";
                for (size_t i = 0; i < val.mapValue->size(); ++i) {
                    os << pad << "  " << (*val.mapValue)[i].first << ": ";
                    printCompileValue(os, (*val.mapValue)[i].second, indent + 2);
                    if (i + 1 < val.mapValue->size()) os << ",";
                    os << "\n";
                }
                os << pad;
            }
            os << "}";
            break;
        case CompileValue::Type::Function:
            os << std::format("<function proto={}>", val.funcValue->protoIndex);
            break;
        case CompileValue::Type::Class:
            os << std::format("<class {}>", val.classValue->name);
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
    std::ofstream ofs(path);
    if (!ofs) {
        std::cerr << std::format("Failed to open file for writing: {}\n", path);
        return;
    }

    ofs << std::format("=== Module: {} ===\n\n", path);

    // ── Imports ──
    ofs << std::format("Imports ({}):\n", module->imports.size());
    for (size_t i = 0; i < module->imports.size(); ++i) {
        ofs << std::format("  [{}] \"{}\"\n", i, module->imports[i]);
    }
    ofs << "\n";

    // ── Global Symbols ──
    ofs << std::format("Global Symbols ({}):\n", module->globalSyms.size());
    for (const auto& [name, sym] : module->globalSyms) {
        ofs << std::format("  {}: {{\n", name);
        ofs << std::format("    mutable: {}\n", sym.isMutable);
        ofs << "    initValue: ";
        printCompileValue(ofs, sym.initValue, 4);
        ofs << "\n";
        if (!sym.relocations.empty()) {
            ofs << std::format("    relocations ({}):\n", sym.relocations.size());
            for (const auto& pos : sym.relocations) {
                ofs << std::format("      proto={}, offset={}\n", pos.protoIndex, pos.bytecodeOffset);
            }
        }
        ofs << "  }\n";
    }
    ofs << "\n";

    // ── External Symbols ──
    ofs << std::format("External Symbols ({}):\n", module->externalSyms.size());
    for (const auto& esym : module->externalSyms) {
        ofs << std::format("  {}.{}:\n", esym.moduleName.empty() ? "(any)" : esym.moduleName, esym.name);
        for (const auto& pos : esym.relocations) {
            ofs << std::format("    proto={}, offset={}\n", pos.protoIndex, pos.bytecodeOffset);
        }
    }
    ofs << "\n";

    // ── Protos ──
    ofs << std::format("Protos ({}):\n", module->protos.size());
    for (const auto& proto : module->protos) {
        ofs << std::format("  Proto[{}]:\n", proto->index);
        ofs << std::format("    arity: {}\n", proto->arity);
        ofs << std::format("    localCount: {}\n", proto->localCount);
        ofs << std::format("    maxStackSize: {}\n", proto->maxStackSize);

        // Constants
        ofs << std::format("    constants ({}):\n", proto->constants.size());
        for (size_t i = 0; i < proto->constants.size(); ++i) {
            ofs << std::format("      [{}] ", i);
            printCompileValue(ofs, proto->constants[i], 8);
            ofs << "\n";
        }

        // Bytecode
        ofs << std::format("    bytecode ({} bytes):\n", proto->bytecode.size());
        uint32_t ip = 0;
        const auto& bc = proto->bytecode;
        while (ip < bc.size()) {
            uint8_t op = bc[ip];
            ofs << std::format("      {:04x}  {}", ip, opcodeName(op));

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
                    ofs << std::format(" {}", val);
                    ip += 4;
                    break;
                }
                case Opcode::Jump:
                case Opcode::JumpIfFalse:
                case Opcode::JumpIfTrue: {
                    uint32_t target;
                    std::memcpy(&target, &bc[ip + 1], 4);
                    ofs << std::format(" -> {:04x}", target);
                    ip += 4;
                    break;
                }
                case Opcode::Call: {
                    uint8_t cnt = bc[ip + 1];
                    ofs << std::format(" argc={}", cnt);
                    ip += 1;
                    break;
                }
                case Opcode::CallMethod: {
                    uint8_t cnt = bc[ip + 1];
                    uint32_t nameIdx;
                    std::memcpy(&nameIdx, &bc[ip + 2], 4);
                    ofs << std::format(" argc={} nameIdx={}", cnt, nameIdx);
                    ip += 5;
                    break;
                }
                case Opcode::CallBuiltin: {
                    uint8_t id = bc[ip + 1];
                    ofs << std::format(" {}", builtinName(id));
                    ip += 1;
                    break;
                }
                default:
                    break;
            }

            ofs << "\n";
            ip += 1;
        }
        ofs << "\n";
    }
}

}
