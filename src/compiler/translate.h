#pragma once

#include "bytecode.h"
#include "visitor.h"
#include <memory>
#include <sys/types.h>
#include <unordered_map>

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
    int declare(const std::string& name, bool isMutable);
    const Scope::Sym* resolve(const std::string& name) const;

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
    Module* module;
    std::unordered_map<std::string, std::string> aliasToModule; 
    bool inGlobalScope = true;

    Value value;
    bool isConstExpr = false;
};

} // namespace Zeta
