#pragma once

namespace Zeta {
namespace AST {
    class Program;
    class Import;
    class Dec;
    class VarDec;
    class FuncDec;
    class ClassDec;
    class Stmt;
    class BlockStmt;
    class ExpStmt;
    class AssignStmt;
    class VarDecStmt;
    class IfStmt;
    class WhileStmt;
    class ForStmt;
    class ReturnStmt;
    class BreakStmt;
    class ContinueStmt;
    class EmptyStmt;
    class Exp;
    class ConditionalExp;
    class BinaryExp;
    class UnaryExp;
    class CallExp;
    class MethodCallExp;
    class SuperCallExp;
    class MemberAccessExp;
    class IndexAccessExp;
    class IdentifierExp;
    class ThisExp;
    class ArrayExp;
    class MapExp;
    class LiteralExp;
    class IntLitExp;
    class FloatLitExp;
    class StrLitExp;
    class BoolLitExp;
    class NullLitExp;
    class ArrayLitExp;
    class MapLitExp;
    class FuncLitExp;
}
class Visitor {
public:
    virtual void visit(AST::Program& program) = 0;
    virtual void visit(AST::Import& import) = 0;
    virtual void visit(AST::VarDec& varDec) = 0;
    virtual void visit(AST::FuncDec& funcDec) = 0;
    virtual void visit(AST::ClassDec& classDec) = 0;
    virtual void visit(AST::BlockStmt& blockStmt) = 0;
    virtual void visit(AST::ExpStmt& expStmt) = 0;
    virtual void visit(AST::AssignStmt& assignStmt) = 0;
    virtual void visit(AST::VarDecStmt& varDecStmt) = 0;
    virtual void visit(AST::IfStmt& ifStmt) = 0;
    virtual void visit(AST::WhileStmt& whileStmt) = 0;
    virtual void visit(AST::ForStmt& forStmt) = 0;
    virtual void visit(AST::ReturnStmt& returnStmt) = 0;
    virtual void visit(AST::BreakStmt& breakStmt) = 0;
    virtual void visit(AST::ContinueStmt& continueStmt) = 0;
    virtual void visit(AST::EmptyStmt& emptyStmt) = 0;
    virtual void visit(AST::ConditionalExp& conditionalExp) = 0;
    virtual void visit(AST::BinaryExp& binaryExp) = 0;
    virtual void visit(AST::UnaryExp& unaryExp) = 0;
    virtual void visit(AST::CallExp& callExp) = 0;
    virtual void visit(AST::MethodCallExp& methodCallExp) = 0;
    virtual void visit(AST::SuperCallExp& superCallExp) = 0;
    virtual void visit(AST::MemberAccessExp& memberAccessExp) = 0;
    virtual void visit(AST::IndexAccessExp& indexAccessExp) = 0;
    virtual void visit(AST::IdentifierExp& identifierExp) = 0;
    virtual void visit(AST::ThisExp& thisExp) = 0;
    virtual void visit(AST::ArrayExp& arrayExp) = 0;
    virtual void visit(AST::MapExp& mapExp) = 0;
    virtual void visit(AST::IntLitExp& intLitExp) = 0;
    virtual void visit(AST::FloatLitExp& floatLitExp) = 0;
    virtual void visit(AST::StrLitExp& strLitExp) = 0;
    virtual void visit(AST::BoolLitExp& boolLitExp) = 0;
    virtual void visit(AST::NullLitExp& nullLitExp) = 0;
    virtual void visit(AST::ArrayLitExp& arrayLitExp) = 0;
    virtual void visit(AST::MapLitExp& mapLitExp) = 0;
    virtual void visit(AST::FuncLitExp& funcLitExp) = 0;
};

} // namespace Zeta
