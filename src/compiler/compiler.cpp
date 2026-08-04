#include "compiler.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <filesystem>

#include "bytecode.h"
#include "translate.h"
#include "syntax.tab.hpp"
#include "error.h"
#include "config.h"

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
        module->name = path.substr(0, path.find_last_of('.'));
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

struct FileHeader{
    char magic[4];
    uint8_t versionMajor;
    uint8_t versionMinor;
    uint16_t flags; // reserved
};

// TODO: 统一大小端, 包括 bytecode 内部的 uint32_t

void serializeModule(const Module* module, const std::string& path, std::string* outError){
    FILE* file = fopen(path.c_str(), "wb");
    if(!file){
        if(outError) *outError = std::format("Failed to open file '{}' for writing", path);
        return;
    }

    // header
    FileHeader header = {
        {'z', 'e', 't', 'a'},
        ZETA_VERSION_MAJOR,
        ZETA_VERSION_MINOR,
        0
    };
    fwrite(&header.magic, sizeof(header.magic), 1, file);
    fwrite(&header.versionMajor, sizeof(header.versionMajor), 1, file);
    fwrite(&header.versionMinor, sizeof(header.versionMinor), 1, file);
    fwrite(&header.flags, sizeof(header.flags), 1, file);

    std::string name = std::filesystem::path(module->name).stem().string();
    uint32_t nameLen = static_cast<uint32_t>(name.size());
    fwrite(&nameLen, sizeof(nameLen), 1, file);
    fwrite(name.data(), 1, name.size(), file);

    std::vector<std::string> stringTable;
    std::unordered_map<std::string, uint32_t> stringIndexMap;
    auto getStringIndex = [&stringTable, &stringIndexMap](const std::string& str) -> uint32_t {
        auto it = stringIndexMap.find(str);
        if(it != stringIndexMap.end()){
            return it->second;
        }else{
            uint32_t index = static_cast<uint32_t>(stringTable.size());
            stringTable.push_back(str);
            stringIndexMap[str] = index;
            return index;
        }
    };

    std::vector<uint8_t> buffer;
    auto writeUint32 = [&buffer](uint32_t v){
        buffer.resize(buffer.size() + 4);
        std::memcpy(buffer.data() + buffer.size() - 4, &v, 4);
    };
    auto writeUint64 = [&buffer](uint64_t v){
        buffer.resize(buffer.size() + 8);
        std::memcpy(buffer.data() + buffer.size() - 8, &v, 8);
    };
    auto writeCompileValue = [&](auto&& self, const CompileValue& val) -> void {
        buffer.push_back(static_cast<uint8_t>(val.type));
        switch(val.type){
            case CompileValue::Type::Null:
                break;
            case CompileValue::Type::Int:
                writeUint64(std::bit_cast<uint64_t>(val.intValue));
                break;
            case CompileValue::Type::Float:
                writeUint64(std::bit_cast<uint64_t>(val.floatValue));
                break;
            case CompileValue::Type::Bool:
                buffer.push_back(val.boolValue ? 1 : 0);
                break;
            case CompileValue::Type::String: {
                uint32_t strIdx = getStringIndex(*val.strValue);
                writeUint32(strIdx);
                break;
            }
            case CompileValue::Type::Array: {
                writeUint32(val.arrayValue->size());
                for(const auto& elem : *val.arrayValue){
                    self(self, elem);
                }
                break;
            }
            case CompileValue::Type::Map: {
                writeUint32(val.mapValue->size());
                for(const auto& [key, value] : *val.mapValue){
                    uint32_t keyIdx = getStringIndex(key);
                    writeUint32(keyIdx);
                    self(self, value);
                }
                break;
            }
            case CompileValue::Type::Function: {
                writeUint32(val.funcValue->protoIndex);
                break;
            }
            case CompileValue::Type::Class: {
                writeUint32(getStringIndex(val.classValue->name));
                writeUint32(getStringIndex(val.classValue->base.first));
                writeUint32(getStringIndex(val.classValue->base.second));
                writeUint32(val.classValue->fields.size());
                for(const auto& [fieldName, fieldVal] : val.classValue->fields){
                    writeUint32(getStringIndex(fieldName));
                    self(self, fieldVal);
                }
                writeUint32(val.classValue->methods.size());
                for(const auto& [methodName, methodVal] : val.classValue->methods){
                    writeUint32(getStringIndex(methodName));
                    writeUint32(methodVal.protoIndex);
                }
                break;
            }
            default:
                assert(false);
                break;
        }
    };

    // imports
    writeUint32(static_cast<uint32_t>(module->imports.size()));
    for(const auto& import : module->imports){
        uint32_t strIdx = getStringIndex(import);
        writeUint32(strIdx);
    }

    // global symbols
    writeUint32(static_cast<uint32_t>(module->globalSyms.size()));
    for(const auto& [symName, sym] : module->globalSyms){
        uint32_t strIdx = getStringIndex(symName);
        writeUint32(strIdx);
        buffer.push_back(static_cast<uint8_t>(sym.valid));
        buffer.push_back(static_cast<uint8_t>(sym.isMutable));
        writeCompileValue(writeCompileValue, sym.initValue);
        writeUint32(static_cast<uint32_t>(sym.relocations.size()));
        for(const auto& rel : sym.relocations){
            writeUint32(rel.protoIndex);
            writeUint32(rel.bytecodeOffset);
        }
    }

    // external symbols
    writeUint32(static_cast<uint32_t>(module->externalSyms.size()));
    for(const auto& esym : module->externalSyms){
        writeUint32(getStringIndex(esym.name));
        writeUint32(getStringIndex(esym.moduleName));
        writeUint32(static_cast<uint32_t>(esym.relocations.size()));
        for(const auto& rel : esym.relocations){
            writeUint32(rel.protoIndex);
            writeUint32(rel.bytecodeOffset);
        }
    }

    // protos
    writeUint32(static_cast<uint32_t>(module->protos.size()));
    for(const auto& proto : module->protos){
        writeUint32(proto->index);
        writeUint32(proto->arity);
        writeUint32(proto->localCount);
        writeUint32(proto->maxStackSize);
        writeUint32(static_cast<uint32_t>(proto->bytecode.size()));
        buffer.resize(buffer.size() + proto->bytecode.size());
        std::memcpy(buffer.data() + buffer.size() - proto->bytecode.size(), proto->bytecode.data(), proto->bytecode.size());
        writeUint32(static_cast<uint32_t>(proto->constants.size()));
        for(const auto& constant : proto->constants){
            writeCompileValue(writeCompileValue, constant);
        }
        writeUint32(static_cast<uint32_t>(proto->lineInfo.size()));
        for(const auto& [offset, line] : proto->lineInfo){
            writeUint32(offset);
            writeUint32(line);
        }
    }

    // string table
    uint32_t stringTableSize = static_cast<uint32_t>(stringTable.size());
    fwrite(&stringTableSize, sizeof(stringTableSize), 1, file);
    for(const auto& str : stringTable){
        uint32_t strLen = static_cast<uint32_t>(str.size());
        fwrite(&strLen, sizeof(strLen), 1, file);
        fwrite(str.data(), 1, strLen, file);
    }
    fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
}


// TODO: 可能需要做更多越界检查, 目前选择信任待解析文件, 假设其在生成后未被篡改.
std::unique_ptr<Module> deserializeModule(const std::string& path, std::string* outError){
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        if (outError) *outError = std::format("Failed to open file '{}' for reading", path);
        return nullptr;
    }
    std::unique_ptr<Module> module = std::make_unique<Module>();

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    rewind(file);
    uint8_t* buffer = static_cast<uint8_t*>(std::malloc(fileSize));
    if (!buffer) {
        if (outError) *outError = std::format("Failed to allocate memory for reading file. This file[size: {}B] is too large.", fileSize);
        fclose(file);
        return nullptr;
    }
    size_t readSize = fread(buffer, 1, fileSize, file);
    if (readSize != static_cast<size_t>(fileSize)) {
        if (outError) *outError = std::format("Failed to read file '{}'. Read {} bytes, expected {} bytes.", path, readSize, fileSize);
        std::free(buffer);
        fclose(file);
        return nullptr;
    }

    uint8_t* ptr = buffer;

    // header
    FileHeader header;
    std::memcpy(&header.magic, ptr, sizeof(header.magic));
    ptr += sizeof(header.magic);
    std::memcpy(&header.versionMajor, ptr, sizeof(header.versionMajor));
    ptr += sizeof(header.versionMajor);
    std::memcpy(&header.versionMinor, ptr, sizeof(header.versionMinor));
    ptr += sizeof(header.versionMinor);
    std::memcpy(&header.flags, ptr, sizeof(header.flags));
    ptr += sizeof(header.flags);
    if(std::memcmp(header.magic, "zeta", 4) != 0){
        if (outError) *outError = std::format("File '{}' is not a valid Zeta module. Invalid magic number.", path);
        std::free(buffer);
        fclose(file);
        return nullptr;
    }

    uint32_t nameLen;
    std::memcpy(&nameLen, ptr, sizeof(nameLen));
    ptr += sizeof(nameLen);
    module->name = std::string(reinterpret_cast<const char*>(ptr), nameLen);
    ptr += nameLen;
    std::string fileName = std::filesystem::path(path).stem().string();
    if(module->name != fileName){
        if (outError) *outError = std::format("Module name '{}' does not match file name '{}'.", module->name, fileName);
        std::free(buffer);
        fclose(file);
        return nullptr;
    }

    // string table
    std::vector<std::string> stringTable;
    uint32_t stringTableSize;
    std::memcpy(&stringTableSize, ptr, sizeof(stringTableSize));
    ptr += sizeof(stringTableSize);
    stringTable.reserve(stringTableSize);
    for (uint32_t i = 0; i < stringTableSize; ++i) {
        uint32_t strLen;
        std::memcpy(&strLen, ptr, sizeof(strLen));
        ptr += sizeof(strLen);
        stringTable.emplace_back(reinterpret_cast<const char*>(ptr), strLen);
        ptr += strLen;
    }

    auto readCompileValue = [&ptr, &stringTable](auto&& self) -> CompileValue {
        CompileValue val;
        CompileValue::Type type;
        std::memcpy(&type, ptr, sizeof(type));
        ptr += sizeof(type);
        switch(type){
            case CompileValue::Type::Null:
                break;
            case CompileValue::Type::Int:{
                int64_t intValue;
                std::memcpy(&intValue, ptr, sizeof(intValue));
                ptr += sizeof(intValue);
                val = CompileValue(intValue);
                break;
            }
            case CompileValue::Type::Float:{
                double floatValue;
                std::memcpy(&floatValue, ptr, sizeof(floatValue));
                ptr += sizeof(floatValue);
                val = CompileValue(floatValue);
                break;
            }
            case CompileValue::Type::Bool:{
                bool boolValue = (*ptr++ != 0);
                val = CompileValue(boolValue);
                break;
            }
            case CompileValue::Type::String:{
                uint32_t strIdx;
                std::memcpy(&strIdx, ptr, sizeof(strIdx));
                ptr += sizeof(strIdx);
                val = CompileValue(stringTable[strIdx]); 
                break;
            }
            case CompileValue::Type::Array:{
                uint32_t arraySize;
                std::memcpy(&arraySize, ptr, sizeof(arraySize));
                ptr += sizeof(arraySize);
                auto arr = new std::vector<CompileValue>();
                arr->reserve(arraySize);
                for(uint32_t i = 0; i < arraySize; ++i){
                    arr->push_back(self(self));
                }
                val = CompileValue(arr);
                break;
            }
            case CompileValue::Type::Map:{
                uint32_t mapSize;
                std::memcpy(&mapSize, ptr, sizeof(mapSize));
                ptr += sizeof(mapSize);
                auto m = new std::vector<std::pair<std::string, CompileValue>>();
                m->reserve(mapSize);
                for(uint32_t i = 0; i < mapSize; ++i){
                    uint32_t keyIdx;
                    std::memcpy(&keyIdx, ptr, sizeof(keyIdx));
                    ptr += sizeof(keyIdx);
                    std::string key = stringTable[keyIdx];
                    CompileValue value = self(self);
                    m->emplace_back(std::move(key), std::move(value));
                }
                val = CompileValue(m);
                break;
            }
            case CompileValue::Type::Function:{
                uint32_t protoIndex;
                std::memcpy(&protoIndex, ptr, sizeof(protoIndex));
                ptr += sizeof(protoIndex);
                auto func = new CompileFunction(protoIndex);
                val = CompileValue(func);
                break;
            }
            case CompileValue::Type::Class:{
                CompileClass* cls = new CompileClass();
                uint32_t nameIdx, baseFirstIdx, baseSecondIdx, fieldCount, methodCount;
                std::memcpy(&nameIdx, ptr, sizeof(nameIdx));
                ptr += sizeof(nameIdx);
                cls->name = stringTable[nameIdx];
                std::memcpy(&baseFirstIdx, ptr, sizeof(baseFirstIdx));
                ptr += sizeof(baseFirstIdx);
                std::memcpy(&baseSecondIdx, ptr, sizeof(baseSecondIdx));
                ptr += sizeof(baseSecondIdx);
                cls->base = {stringTable[baseFirstIdx], stringTable[baseSecondIdx]};
                std::memcpy(&fieldCount, ptr, sizeof(fieldCount));
                ptr += sizeof(fieldCount);
                cls->fields.reserve(fieldCount);
                for(uint32_t i = 0; i < fieldCount; ++i){
                    uint32_t fieldNameIdx;
                    std::memcpy(&fieldNameIdx, ptr, sizeof(fieldNameIdx));
                    ptr += sizeof(fieldNameIdx);
                    std::string fieldName = stringTable[fieldNameIdx];
                    CompileValue fieldVal = self(self);
                    cls->fields.emplace(std::move(fieldName), std::move(fieldVal));
                }
                std::memcpy(&methodCount, ptr, sizeof(methodCount));
                ptr += sizeof(methodCount);
                cls->methods.reserve(methodCount);
                for(uint32_t i = 0; i < methodCount; ++i){
                    uint32_t methodNameIdx;
                    std::memcpy(&methodNameIdx, ptr, sizeof(methodNameIdx));
                    ptr += sizeof(methodNameIdx);
                    std::string methodName = stringTable[methodNameIdx];
                    uint32_t protoIndex;
                    std::memcpy(&protoIndex, ptr, sizeof(protoIndex));
                    ptr += sizeof(protoIndex);
                    cls->methods.emplace(std::move(methodName), CompileFunction(protoIndex));
                }
                val = CompileValue(cls);
                break;
            }
			default:
				assert(false);
				break;
		}
        return val;
	};

    // imports
    uint32_t importCount;
    std::memcpy(&importCount, ptr, sizeof(importCount));
    ptr += sizeof(importCount);
    module->imports.reserve(importCount);
    for (uint32_t i = 0; i < importCount; ++i) {
        uint32_t strIdx;
        std::memcpy(&strIdx, ptr, sizeof(strIdx));
        module->imports.push_back(stringTable[strIdx]);
        ptr += sizeof(strIdx);
    }

    // global symbols
    uint32_t globalSymCount;
    std::memcpy(&globalSymCount, ptr, sizeof(globalSymCount));
    ptr += sizeof(globalSymCount);
    module->globalSyms.reserve(globalSymCount);
    for (uint32_t i = 0; i < globalSymCount; ++i) {
        uint32_t strIdx;
        std::memcpy(&strIdx, ptr, sizeof(strIdx));
        std::string symName = stringTable[strIdx];
        ptr += sizeof(strIdx);

        Symbol sym;
        sym.valid = *ptr++;
        sym.isMutable = *ptr++;
        sym.initValue = readCompileValue(readCompileValue);
        uint32_t relocationsCount;
        std::memcpy(&relocationsCount, ptr, sizeof(relocationsCount));
        ptr += sizeof(relocationsCount);
        sym.relocations.reserve(relocationsCount);
        for (uint32_t j = 0; j < relocationsCount; ++j) {
            Pos pos;
            std::memcpy(&pos.protoIndex, ptr, sizeof(pos.protoIndex));
            ptr += sizeof(pos.protoIndex);
            std::memcpy(&pos.bytecodeOffset, ptr, sizeof(pos.bytecodeOffset));
            ptr += sizeof(pos.bytecodeOffset);
            sym.relocations.push_back(pos);
        }
        module->globalSyms.emplace(std::move(symName), std::move(sym));
    }

    // external symbols
    uint32_t externalSymCount;
    std::memcpy(&externalSymCount, ptr, sizeof(externalSymCount));
    ptr += sizeof(externalSymCount);
    module->externalSyms.reserve(externalSymCount);
    for (uint32_t i = 0; i < externalSymCount; ++i) {
        ExtSymbol esym;
        uint32_t nameIdx, moduleNameIdx;
        std::memcpy(&nameIdx, ptr, sizeof(nameIdx));
        ptr += sizeof(nameIdx);
        std::memcpy(&moduleNameIdx, ptr, sizeof(moduleNameIdx));
        ptr += sizeof(moduleNameIdx);
        esym.name = stringTable[nameIdx];
        esym.moduleName = stringTable[moduleNameIdx];

        uint32_t relocationsCount;
        std::memcpy(&relocationsCount, ptr, sizeof(relocationsCount));
        ptr += sizeof(relocationsCount);
        esym.relocations.reserve(relocationsCount);
        for (uint32_t j = 0; j < relocationsCount; ++j) {
            Pos pos;
            std::memcpy(&pos.protoIndex, ptr, sizeof(pos.protoIndex));
            ptr += sizeof(pos.protoIndex);
            std::memcpy(&pos.bytecodeOffset, ptr, sizeof(pos.bytecodeOffset));
            ptr += sizeof(pos.bytecodeOffset);
            esym.relocations.push_back(pos);
        }
        module->externalSyms.push_back(std::move(esym));
    }

    // protos
    uint32_t protoCount;
    std::memcpy(&protoCount, ptr, sizeof(protoCount));
    ptr += sizeof(protoCount);
    module->protos.reserve(protoCount);
    for (uint32_t i = 0; i < protoCount; ++i) {
        auto proto = std::make_unique<Proto>();
        std::memcpy(&proto->index, ptr, sizeof(proto->index));
        ptr += sizeof(proto->index);
        std::memcpy(&proto->arity, ptr, sizeof(proto->arity));
        ptr += sizeof(proto->arity);
        std::memcpy(&proto->localCount, ptr, sizeof(proto->localCount));
        ptr += sizeof(proto->localCount);
        std::memcpy(&proto->maxStackSize, ptr, sizeof(proto->maxStackSize));
        ptr += sizeof(proto->maxStackSize);

        uint32_t bytecodeSize;
        std::memcpy(&bytecodeSize, ptr, sizeof(bytecodeSize));
        ptr += sizeof(bytecodeSize);
        proto->bytecode.resize(bytecodeSize);
        std::memcpy(proto->bytecode.data(), ptr, bytecodeSize);
        ptr += bytecodeSize;

        uint32_t constantsCount;
        std::memcpy(&constantsCount, ptr, sizeof(constantsCount));
        ptr += sizeof(constantsCount);
        proto->constants.reserve(constantsCount);
        for (uint32_t j = 0; j < constantsCount; ++j) {
            proto->constants.push_back(readCompileValue(readCompileValue));
        }

        uint32_t lineInfoCount;
        std::memcpy(&lineInfoCount, ptr, sizeof(lineInfoCount));
        ptr += sizeof(lineInfoCount);
        proto->lineInfo.reserve(lineInfoCount);
        for (uint32_t j = 0; j < lineInfoCount; ++j) {
            uint32_t offset, line;
            std::memcpy(&offset, ptr, sizeof(offset));
            ptr += sizeof(offset);
            std::memcpy(&line, ptr, sizeof(line));
            ptr += sizeof(line);
            proto->lineInfo.emplace_back(offset, line);
        }

        module->protos.push_back(std::move(proto));
    }

    std::free(buffer);
    fclose(file);
    module->name = path.substr(0, path.find_last_of('.'));
    return module;
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
