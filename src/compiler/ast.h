#pragma once

#include "visitor.h"
#include <memory>
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
};

class FuncDec: public Dec {
public:
    ACCEPT
    std::string name;
    std::vector<std::string> params;
    std::unique_ptr<BlockStmt> body;
};

class ClassDec: public Dec {
public:
    ACCEPT
    std::string name;
    std::string base;
    std::vector<std::pair<bool, std::unique_ptr<Dec>>> members; // true: static
};

class Stmt: public Node {
public:
    enum class StmtType {
        Block,
        Exp,
        VarDec,
        If,
        While,
        For,
        Return,
        Break,
        Continue
    } type;
};

class BlockStmt: public Stmt {
public:
    ACCEPT
    std::vector<std::unique_ptr<Stmt>> stmts;
};

class ExpStmt: public Stmt {
public:
    ACCEPT
    std::unique_ptr<Exp> exp;
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
    std::string idxVarName; // optional
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

class Exp: public Node {
public:
    enum class ExpType {
        Conditional,
        Binary,
        Unary,
        Call,
        MemberAccess,
        IndexAccess,
        Assign,
        Identifier,
        Literal,
        This
    } type;
};

class ConditionalExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> cond;
    std::unique_ptr<Exp> thenBranch;
    std::unique_ptr<Exp> elseBranch;
};

class BinaryExp: public Exp {
public:
    ACCEPT
    enum class Op {
        Add, Sub, Mul, Div, Mod,
        Eq, Neq, Lt, Gt, Leq, Geq,
        And, Or, 
        BitAnd, BitOr, BitXor, Shl, Shr
    } op;
    std::unique_ptr<Exp> left;
    std::unique_ptr<Exp> right;
};

class UnaryExp: public Exp {
public:
    ACCEPT
    enum class Op {
        Neg, Not, BitNot
    } op;
    std::unique_ptr<Exp> operand;
};

class CallExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> caller; // empty for normal function call
    std::string funcName;
    std::vector<std::unique_ptr<Exp>> args;
};

class MemberAccessExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> object;
    std::string member;
};

class IndexAccessExp: public Exp {
public:
    ACCEPT
    std::unique_ptr<Exp> object;
    std::unique_ptr<Exp> index;
};

class AssignExp: public Exp {
public:
    ACCEPT
    enum class Op {
        Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
        AndAssign, OrAssign, BitAndAssign, BitOrAssign, BitXorAssign,
        ShlAssign, ShrAssign
    } op;
    std::unique_ptr<Exp> target;
    std::unique_ptr<Exp> value;
};

class IdentifierExp: public Exp {
public:
    ACCEPT
    std::string name;
};

class ThisExp: public Exp {
public:
    ACCEPT
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
};

class FloatLitExp: public LiteralExp {
public:
    ACCEPT
    double value;
};

class StrLitExp: public LiteralExp {
public:
    ACCEPT
    std::string value;
};

class BoolLitExp: public LiteralExp {
public:
    ACCEPT
    bool value;
};

class NullLitExp: public LiteralExp {
public:
    ACCEPT
};

class ArrayLitExp: public LiteralExp {
public:
    ACCEPT
    std::vector<std::unique_ptr<Exp>> elements;
};

class MapLitExp: public LiteralExp {
public:
    ACCEPT
    std::vector<std::pair<std::string, std::unique_ptr<Exp>>> entries;
};

class FuncLitExp: public LiteralExp {
public:    
    ACCEPT
    std::vector<std::string> params;
    std::unique_ptr<BlockStmt> body;
};

} // namespace AST

} // namespace Zeta