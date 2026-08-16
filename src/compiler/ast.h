#pragma once

#include "visitor.h"
#include <memory>
#include <utility>
#include <vector>
#include <string>

#define ACCEPT void accept(Visitor& v) override { v.visit(*this); }

namespace Zeta {

namespace AST {
// base AST node
class Node{
public:
    int line;
    int column;

    Node() = default;
    virtual ~Node() = default;
    virtual void accept(Visitor& v) = 0;
};

class Program: public Node {
public:
    ACCEPT
    std::vector<std::unique_ptr<Import>> imports;
    std::vector<std::unique_ptr<Dec>> decs;
};

class Import: public Node {
public:
    ACCEPT
    std::string path;
    std::string alias;
};

class Dec: public Node {
public:
    enum class DecType {
        Var,
        Func,
        Class
    } type;
};

class VarDec: public Dec {
public:
    ACCEPT
    bool isMutable; // var: true, let: false
    std::string name;
    std::unique_ptr<Exp> init; // can be null for 'var'

    VarDec() { type = DecType::Var; }
};

class FuncDec: public Dec {
public:
    ACCEPT
    std::string name;
    std::vector<std::string> params;
    std::unique_ptr<BlockStmt> body;

    FuncDec() { type = DecType::Func; }
};

class ClassDec: public Dec {
public:
    ACCEPT
    std::string name;
    std::pair<std::string, std::string> base;
    std::vector<std::unique_ptr<Dec>> members;

    ClassDec() { type = DecType::Class; }
};

class Stmt: public Node {
public:
    // TODO: 类型信息不一定用到
    enum class StmtType {
        Block,
        Exp,
        Assign,
        VarDec,
        If,
        While,
        For,
        Return,
        Break,
        Continue,
        Empty
    } type;
};

class BlockStmt: public Stmt {
public:
    ACCEPT
    bool isFuncBody = false;
    std::vector<std::unique_ptr<Stmt>> stmts;
};

class ExpStmt: public Stmt {
public:
    ACCEPT
    std::unique_ptr<Exp> exp;
};

class AssignStmt: public Stmt {
public:
    ACCEPT
    enum class Op {
        Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
        BitAndAssign, BitOrAssign, BitXorAssign,
        ShlAssign, ShrAssign
    } op;
    std::unique_ptr<Exp> target;
    std::unique_ptr<Exp> value;
};

class VarDecStmt: public Stmt {
public:
    ACCEPT
    std::vector<std::unique_ptr<VarDec>> varDecs;
};

class IfStmt: public Stmt {
public:
    ACCEPT
    std::unique_ptr<Exp> cond;
    std::unique_ptr<Stmt> thenBranch;
    std::unique_ptr<Stmt> elseBranch;
};

class WhileStmt: public Stmt {
public:
    ACCEPT
    std::unique_ptr<Exp> cond;
    std::unique_ptr<Stmt> body;
};

class ForStmt: public Stmt {
public:
    ACCEPT
    std::string valVarName;
    std::unique_ptr<Exp> iterable;
    std::unique_ptr<Stmt> body;
};

class ReturnStmt: public Stmt {
public:
    ACCEPT
    std::unique_ptr<Exp> value;
};

class BreakStmt: public Stmt { public: ACCEPT };
class ContinueStmt: public Stmt { public: ACCEPT };
class EmptyStmt: public Stmt { public: ACCEPT };

class Exp: public Node {
public:
    enum class ExpType {
        Conditional,
        Binary,
        Unary,
        Call,
        SuperCall,
        MemberAccess,
        IndexAccess,
        Identifier,
        Literal,
        This,
        Array,
        Map
    } type;
};

class ConditionalExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> cond;
    std::unique_ptr<Exp> thenBranch;
    std::unique_ptr<Exp> elseBranch;

    ConditionalExp() { type = ExpType::Conditional; }
};

class BinaryExp: public Exp {
public:
    ACCEPT
    enum class Op {
        Add, Sub, Mul, Div, Mod,
        Eq, Neq, Lt, Gt, Leq, Geq, Is,
        And, Or, 
        BitAnd, BitOr, BitXor, Shl, Shr
    } op;
    std::unique_ptr<Exp> left;
    std::unique_ptr<Exp> right;

    BinaryExp() { type = ExpType::Binary; }
};

class UnaryExp: public Exp {
public:
    ACCEPT
    enum class Op {
        Neg, Not, BitNot
    } op;
    std::unique_ptr<Exp> operand;

    UnaryExp() { type = ExpType::Unary; }
};

class CallExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> caller; // empty for normal function call
    std::string funcName;
    std::vector<std::unique_ptr<Exp>> args;

    CallExp() { type = ExpType::Call; }
};

class SuperCallExp: public Exp {
public:
    ACCEPT
    std::string methodName;
    std::vector<std::unique_ptr<Exp>> args;
};

class MemberAccessExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> object;
    std::string member;

    MemberAccessExp() { type = ExpType::MemberAccess; }
};

class IndexAccessExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> object;
    std::unique_ptr<Exp> index;

    IndexAccessExp() { type = ExpType::IndexAccess; }
};

class IdentifierExp: public Exp {
public:
    ACCEPT
    std::string name;

    IdentifierExp() { type = ExpType::Identifier; }
};

class ThisExp: public Exp {
public:
    ACCEPT

    ThisExp() { type = ExpType::This; }
};

class ArrayExp: public Exp {
public:
    ACCEPT
    std::vector<std::unique_ptr<Exp>> elements;

    ArrayExp() { type = ExpType::Array; }
};

class MapExp: public Exp {
public:
    ACCEPT
    std::vector<std::pair<std::string, std::unique_ptr<Exp>>> entries;

    MapExp() { type = ExpType::Map; }
};

class LiteralExp: public Exp {
public:
    enum class LiteralType {
        Int, Float, Str, Bool, Null, Array, Map, Func
    } type;
};

class IntLitExp: public LiteralExp {
public:
    ACCEPT
    int64_t value;

    IntLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Int; }
};

class FloatLitExp: public LiteralExp {
public:
    ACCEPT
    double value;

    FloatLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Float; }
};

class StrLitExp: public LiteralExp {
public:
    ACCEPT
    std::string value;

    StrLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Str; }
};

class BoolLitExp: public LiteralExp {
public:
    ACCEPT
    bool value;

    BoolLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Bool; }
};

class NullLitExp: public LiteralExp {
public:
    ACCEPT

    NullLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Null; }
};

class ArrayLitExp: public LiteralExp {
public:
    ACCEPT
    std::vector<std::unique_ptr<LiteralExp>> elements;

    ArrayLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Array; }
};

class MapLitExp: public LiteralExp {
public:
    ACCEPT
    std::vector<std::pair<std::string, std::unique_ptr<LiteralExp>>> entries;

    MapLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Map; }
};

class FuncLitExp: public LiteralExp {
public:    
    ACCEPT
    std::vector<std::string> params;
    std::unique_ptr<BlockStmt> body;

    FuncLitExp() { Exp::type = ExpType::Literal; type = LiteralType::Func; }
};

} // namespace AST

} // namespace Zeta
