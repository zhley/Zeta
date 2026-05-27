#pragma once

#include "bytecode.h"
#include "visitor.h"

#include <cstdint>
#include <memory>
#include <stack>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace Zeta {

// TODO: 加入闭包上值支持
// 局部作用域
class ScopeManager {
public:
    struct Scope{
        struct Sym{
            uint32_t index;
            bool isMutable;
        };
        std::unordered_map<std::string, Sym> symbols;
    };
    void enterScope();
    void exitScope();
    uint32_t declare(const std::string& name, bool isMutable);
    const Scope::Sym* resolve(const std::string& name) const;
    void reset();
    
    bool empty(){ return scopes.empty(); }
    uint32_t getMaxIndex() const { return maxIndex; }

private:
    std::vector<std::unique_ptr<Scope>> scopes;
    std::vector<uint32_t> savedNextIndices; 
    uint32_t nextIndex = 0;
    uint32_t maxIndex = 0;
};

class Translator : public Visitor {
public:
    std::unique_ptr<Module> translate(AST::Program* program);

    void visit(AST::Program& program) override;
    void visit(AST::Import& import) override;
    void visit(AST::VarDec& varDec) override;
    void visit(AST::FuncDec& funcDec) override;
    void visit(AST::ClassDec& classDec) override;
    void visit(AST::BlockStmt& blockStmt) override;
    void visit(AST::ExpStmt& expStmt) override;
    void visit(AST::VarDecStmt& varDecStmt) override;
    void visit(AST::IfStmt& ifStmt) override;
    void visit(AST::WhileStmt& whileStmt) override;
    void visit(AST::ForStmt& forStmt) override;
    void visit(AST::ReturnStmt& returnStmt) override;
    void visit(AST::BreakStmt& breakStmt) override;
    void visit(AST::ContinueStmt& continueStmt) override;
    void visit(AST::ConditionalExp& conditionalExp) override;
    void visit(AST::BinaryExp& binaryExp) override;
    void visit(AST::UnaryExp& unaryExp) override;
    void visit(AST::CallExp& callExp) override;
    void visit(AST::MemberAccessExp& memberAccessExp) override;
    void visit(AST::IndexAccessExp& indexAccessExp) override;
    void visit(AST::AssignExp& assignExp) override;
    void visit(AST::IdentifierExp& identifierExp) override;
    void visit(AST::ThisExp& thisExp) override;
    void visit(AST::IntLitExp& intLitExp) override;
    void visit(AST::FloatLitExp& floatLitExp) override;
    void visit(AST::StrLitExp& strLitExp) override;
    void visit(AST::BoolLitExp& boolLitExp) override;
    void visit(AST::NullLitExp& nullLitExp) override;
    void visit(AST::ArrayLitExp& arrayLitExp) override;
    void visit(AST::MapLitExp& mapLitExp) override;
    void visit(AST::FuncLitExp& funcLitExp) override;

private:
    // State
    Module* module = nullptr;
    std::unordered_map<std::string, std::string> aliasToModule; 
    
    Proto* curProto = nullptr;
    bool inMethod = false;
    ScopeManager curScopeMgr;

    std::stack<std::vector<uint32_t>> breakPosStack;
    std::stack<std::vector<uint32_t>> continuePosStack;

    uint32_t pushOpcode(Opcode opcode) {
        uint32_t offset = curProto->bytecode.size();
        curProto->bytecode.push_back(static_cast<uint8_t>(opcode));
        return offset;
    }

    uint32_t push4B(uint32_t value){
        curProto->bytecode.resize(curProto->bytecode.size() + 4);
        uint32_t offset = curProto->bytecode.size() - 4;
        std::memcpy(curProto->bytecode.data() + offset, &value, 4);
        return offset;
    }

    uint32_t set4B(uint32_t offset, uint32_t value){
        std::memcpy(curProto->bytecode.data() + offset, &value, 4);
        return offset;
    }

    uint32_t skip4B(){
        curProto->bytecode.resize(curProto->bytecode.size() + 4);
        return curProto->bytecode.size() - 4;
    }

    // TODO: 最终字节码最后需要追加一个空操作保证标签不越界
    uint32_t getLabel() {
        return curProto->bytecode.size();
    }

    void callBuiltin(Builtin builtin) {
        pushOpcode(Opcode::CallBuiltin);
        curProto->bytecode.push_back(static_cast<uint8_t>(builtin));
    }

    // const
    uint32_t makeConstIdx(const CompileValue& val) {
        for(size_t i = 0; i < curProto->constants.size(); ++i){
            if(*curProto->constants[i] == val){
                return i;
            }
        }
        curProto->constants.push_back(std::make_unique<CompileValue>(val));
        return curProto->constants.size() - 1;
    }

    void recordGlobalSym(const std::string& name, const std::string& moduleName, uint32_t pos) {
        if(moduleName.empty()){
            auto it = module->globalSyms.find(name);
            if(it == module->globalSyms.end()){
                auto it = std::find(module->externalSyms.begin(), module->externalSyms.end(), ExtSymbol{name, ""});
                if(it == module->externalSyms.end()){
                    module->externalSyms.push_back({name, ""});
                    it = module->externalSyms.end() - 1;
                }
                it->relocations.push_back({curProto->index, pos});
            } else {
                it->second.relocations.push_back({curProto->index, pos});
            }
        } else {
            auto it = std::find(module->externalSyms.begin(), module->externalSyms.end(), ExtSymbol{name, moduleName});
            if(it == module->externalSyms.end()){
                module->externalSyms.push_back({name, moduleName});
                it = module->externalSyms.end() - 1;
            }
            it->relocations.push_back({curProto->index, pos});
        }
    }

    std::unique_ptr<CompileValue> getValue(const AST::LiteralExp* exp);
};

} // namespace Zeta
