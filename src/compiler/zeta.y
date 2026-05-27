%code requires {
    #include "compiler/ast.h"
    #include "error.h"

    #include <memory>
    #include <string>
    #include <utility>
    #include <vector>
    #include <assert.h>

    enum Relop { RP_EQ, RP_NEQ, RP_LT, RP_GT, RP_LEQ, RP_GEQ };
    enum AssignOp { AS_ASSIGN, AS_PLUS, AS_MINUS, AS_STAR, AS_DIV, AS_MOD, AS_BITAND, AS_BITOR, AS_BITXOR, AS_LSHIFT, AS_RSHIFT };

    extern int yylineno;
    extern int yycolumn;

    Zeta::AST::AssignExp::Op toAssignOp(AssignOp assignOp);
    Zeta::AST::BinaryExp::Op relopToBinaryOp(Relop relop);
    void yyerror(const char* msg);
    std::unique_ptr<Zeta::AST::Exp> tryExpFold(std::unique_ptr<Zeta::AST::Exp> exp);

    namespace Zeta::ParserTypes {
        using ImportList = std::vector<std::unique_ptr<Zeta::AST::Import>>;
        using DecList = std::vector<std::unique_ptr<Zeta::AST::Dec>>;
        using VarDecList = std::vector<std::unique_ptr<Zeta::AST::VarDec>>;
        using ClassMemberList = std::vector<std::pair<bool, std::unique_ptr<Zeta::AST::Dec>>>;
        using StmtList = std::vector<std::unique_ptr<Zeta::AST::Stmt>>;
        using ExpList = std::vector<std::unique_ptr<Zeta::AST::Exp>>;
        using MapEntryList = std::vector<std::pair<std::string, std::unique_ptr<Zeta::AST::Exp>>>;
        using ParamList = std::vector<std::string>;
    }
}

%code top{
    #include "compiler/ast.h"
    using namespace Zeta::AST;
}

%code {
    Zeta::Parser::symbol_type yylex();
}

%language "c++"
%define api.namespace {Zeta}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.token.constructor
%define parse.error verbose
%token <int64_t> INT
%token <double> FLOAT
%token <bool> BOOL
%token <Relop> RELOP
%token <AssignOp> ASSIGN
%token <std::string> ID STR
%token NULL_
%token VAR LET FN RETURN IF ELSE WHILE FOR BREAK CONTINUE CLASS EXTENDS THIS IMPORT AS
%token AND OR LSHIFT RSHIFT SEMI DOT COMMA COLON QMARK LP RP LB RB LC RC PLUS MINUS STAR DIV MOD NOT BITAND BITOR BITXOR BITNOT

%right ASSIGN 
%right QMARK COLON
%left OR
%left AND
%left RELOP
%left LSHIFT RSHIFT
%left PLUS MINUS
%left STAR DIV MOD
%left BITAND BITOR BITXOR
%right NOT BITNOT MINUS_S
%left LP RP LB RB DOT
%right ELSE

%type <std::unique_ptr<Zeta::AST::Program>> Program
%type <Zeta::ParserTypes::ImportList> ImportList
%type <std::unique_ptr<Zeta::AST::Import>> Import
%type <Zeta::ParserTypes::DecList> DecList
%type <Zeta::ParserTypes::VarDecList> VarDec VarList
%type <std::unique_ptr<Zeta::AST::VarDec>> Var
%type <std::unique_ptr<Zeta::AST::FuncDec>> FuncDec
%type <Zeta::ParserTypes::ParamList> ParamList
%type <std::string> Param
%type <std::unique_ptr<Zeta::AST::ClassDec>> ClassDec
%type <Zeta::ParserTypes::ClassMemberList> ClassBody
%type <Zeta::ParserTypes::StmtList> StmtList
%type <std::unique_ptr<Zeta::AST::Stmt>> Stmt
%type <std::unique_ptr<Zeta::AST::BlockStmt>> BlockStmt
%type <std::unique_ptr<Zeta::AST::Exp>> Exp Literal ArrayLit MapLit FuncLit
%type <Zeta::ParserTypes::ExpList> ArgList ElemList
%type <Zeta::ParserTypes::MapEntryList> MapElemList
%type <std::pair<std::string, std::unique_ptr<Zeta::AST::Exp>>> MapElem

%%
Program: ImportList DecList { auto p = std::make_unique<Program>(); p->imports = std::move($1); p->decs = std::move($2); $$ = std::move(p); }
    ;

ImportList: ImportList Import { $1.push_back(std::move($2)); $$ = std::move($1); }
    | { $$ = Zeta::ParserTypes::ImportList{}; }
    ;
Import: IMPORT STR SEMI { auto t = std::make_unique<Import>(); t->path = std::move($2); $$ = std::move(t); }
    | IMPORT STR AS ID SEMI { auto t = std::make_unique<Import>(); t->path = std::move($2); t->alias = std::move($4); $$ = std::move(t); }
    | error SEMI { $$ = nullptr; }
    ;

DecList: DecList VarDec SEMI { for(auto& d: $2) $1.push_back(std::unique_ptr<Dec>(std::move(d))); $$ = std::move($1); }
    | DecList FuncDec { $1.push_back(std::unique_ptr<Dec>(std::move($2))); $$ = std::move($1); }
    | DecList ClassDec { $1.push_back(std::unique_ptr<Dec>(std::move($2))); $$ = std::move($1); }
    | { $$ = Zeta::ParserTypes::DecList{}; }
    ;

VarDec: LET VarList { for(auto& v: $2) v->isMutable = false; $$ = std::move($2); }
    | VAR VarList { $$ = std::move($2); }
    ;
VarList: VarList COMMA Var { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Var { Zeta::ParserTypes::VarDecList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
Var: ID { auto t = std::make_unique<VarDec>(); t->name = std::move($1); t->isMutable = true; $$ = std::move(t); }
    | ID ASSIGN Exp { auto t = std::make_unique<VarDec>(); t->name = std::move($1); t->isMutable = true; t->init = std::move($3); $$ = std::move(t); if($2 != AS_ASSIGN) { Zeta::Parser::error("Initialization only allowed with '='"); } }
    ;

FuncDec: FN ID LP ParamList RP BlockStmt { auto f = std::make_unique<FuncDec>(); f->name = std::move($2); f->params = std::move($4); $6->isFuncBody = true; f->body = std::move($6); $$ = std::move(f); }
    | FN ID LP RP BlockStmt { auto f = std::make_unique<FuncDec>(); f->name = std::move($2); $5->isFuncBody = true; f->body = std::move($5); $$ = std::move(f); }
    | error RP BlockStmt { $$ = nullptr; }
    | error RC { $$ = nullptr; }
    ;
ParamList: ParamList COMMA Param { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Param { Zeta::ParserTypes::ParamList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
Param: ID { $$ = std::move($1); }
    ;

ClassDec: CLASS ID LC ClassBody RC { auto c = std::make_unique<ClassDec>(); c->name = std::move($2); c->members = std::move($4); $$ = std::move(c); }
    | CLASS ID EXTENDS ID LC ClassBody RC { auto c = std::make_unique<ClassDec>(); c->name = std::move($2); c->base = std::move($4); c->members = std::move($6); $$ = std::move(c); }
    | error LC ClassBody RC { $$ = nullptr; }
    ;
ClassBody: ClassBody VarDec SEMI { for(auto& d: $2) $1.push_back(std::unique_ptr<Dec>(std::move(d))); $$ = std::move($1); }
    | ClassBody FuncDec { $1.push_back(std::unique_ptr<Dec>(std::move($2))); $$ = std::move($1); }
    | { $$ = Zeta::ParserTypes::ClassMemberList{}; }
    ;

StmtList: StmtList Stmt { $1.push_back(std::move($2)); $$ = std::move($1); }
    | { $$ = Zeta::ParserTypes::StmtList{}; }
    ;
BlockStmt: LC StmtList RC { auto b = std::make_unique<BlockStmt>(); b->stmts = std::move($2); $$ = std::move(b); }
    ;
Stmt: VarDec SEMI { auto s = std::make_unique<VarDecStmt>(); s->varDecs = std::move($1); $$ = std::move(s); }
    | Exp SEMI { auto s = std::make_unique<ExpStmt>(); s->exp = std::move($1); $$ = std::move(s); }
    | IF LP Exp RP Stmt %prec ELSE { auto s = std::make_unique<IfStmt>(); s->cond = std::move($3); s->thenBranch = std::move($5); $$ = std::move(s); }
    | IF LP Exp RP Stmt ELSE Stmt { auto s = std::make_unique<IfStmt>(); s->cond = std::move($3); s->thenBranch = std::move($5); s->elseBranch = std::move($7); $$ = std::move(s); }
    | WHILE LP Exp RP Stmt { auto s = std::make_unique<WhileStmt>(); s->cond = std::move($3); s->body = std::move($5); $$ = std::move(s); }
    | FOR LP ID COLON Exp RP Stmt { auto s = std::make_unique<ForStmt>(); s->valVarName = std::move($3); s->iterable = std::move($5); s->body = std::move($7); $$ = std::move(s); }
    | FOR LP ID COMMA ID COLON Exp RP Stmt { auto s = std::make_unique<ForStmt>(); s->idxVarName = std::move($3); s->valVarName = std::move($5); s->iterable = std::move($7); s->body = std::move($9); $$ = std::move(s); }
    | BREAK SEMI { auto s = std::make_unique<BreakStmt>(); $$ = std::move(s); }
    | CONTINUE SEMI { auto s = std::make_unique<ContinueStmt>(); $$ = std::move(s); }
    | RETURN SEMI { auto s = std::make_unique<ReturnStmt>(); $$ = std::move(s); }
    | RETURN Exp SEMI { auto s = std::make_unique<ReturnStmt>(); s->value = std::move($2); $$ = std::move(s); }
    | BlockStmt { $$ = std::move($1); }
    | SEMI { $$ = nullptr; }
    | error SEMI { $$ = nullptr; }
    | error RC { $$ = nullptr; }
    ;

Exp: Exp QMARK Exp COLON Exp { auto e = std::make_unique<ConditionalExp>(); e->cond = std::move($1); e->thenBranch = std::move($3); e->elseBranch = std::move($5); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp ASSIGN Exp { auto e = std::make_unique<AssignExp>(); e->op = toAssignOp($2); e->target = std::move($1); e->value = std::move($3); $$ = std::move(e); }
    | Exp RELOP Exp { auto e = std::make_unique<BinaryExp>(); e->op = relopToBinaryOp($2); e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp AND Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::And; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp OR Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Or; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp LSHIFT Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Shl; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp RSHIFT Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Shr; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp PLUS Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Add; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp MINUS Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Sub; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp STAR Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Mul; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp DIV Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Div; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp MOD Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::Mod; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp BITAND Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::BitAnd; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp BITOR Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::BitOr; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | Exp BITXOR Exp { auto e = std::make_unique<BinaryExp>(); e->op = BinaryExp::Op::BitXor; e->left = std::move($1); e->right = std::move($3); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | MINUS Exp %prec MINUS_S { auto e = std::make_unique<UnaryExp>(); e->op = UnaryExp::Op::Neg; e->operand = std::move($2); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | NOT Exp { auto e = std::make_unique<UnaryExp>(); e->op = UnaryExp::Op::Not; e->operand = std::move($2); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | BITNOT Exp { auto e = std::make_unique<UnaryExp>(); e->op = UnaryExp::Op::BitNot; e->operand = std::move($2); e = tryExpFold(std::move(e)); $$ = std::move(e); }
    | LP Exp RP { $$ = std::move($2); }
    | Exp DOT ID LP ArgList RP { auto e = std::make_unique<CallExp>(); e->caller = std::move($1); e->funcName = std::move($3); e->args = std::move($5); $$ = std::move(e); }
    | Exp DOT ID LP RP { auto e = std::make_unique<CallExp>(); e->caller = std::move($1); e->funcName = std::move($3); $$ = std::move(e); }
    | Exp DOT ID { auto e = std::make_unique<MemberAccessExp>(); e->object = std::move($1); e->member = std::move($3); $$ = std::move(e); }
    | Exp LB Exp RB { auto e = std::make_unique<IndexAccessExp>(); e->object = std::move($1); e->index = std::move($3); $$ = std::move(e); }
    | ID LP ArgList RP { auto e = std::make_unique<CallExp>(); e->funcName = std::move($1); e->args = std::move($3); $$ = std::move(e); }
    | ID LP RP { auto e = std::make_unique<CallExp>(); e->funcName = std::move($1); $$ = std::move(e); }
    | ID { auto e = std::make_unique<IdentifierExp>(); e->name = std::move($1); $$ = std::move(e); }
    | Literal { $$ = std::move($1); }
    | THIS { auto e = std::make_unique<ThisExp>(); $$ = std::move(e); }
    ;
ArgList: ArgList COMMA Exp { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Exp { Zeta::ParserTypes::ExpList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
Literal: INT { auto e = std::make_unique<IntLitExp>(); e->value = $1; $$ = std::move(e); }
    | FLOAT { auto e = std::make_unique<FloatLitExp>(); e->value = $1; $$ = std::move(e); }
    | BOOL { auto e = std::make_unique<BoolLitExp>(); e->value = $1; $$ = std::move(e); }
    | NULL_ { auto e = std::make_unique<NullLitExp>(); $$ = std::move(e); }
    | STR { auto e = std::make_unique<StrLitExp>(); e->value = std::move($1); $$ = std::move(e); }
    | ArrayLit { $$ = std::move($1); }
    | MapLit { $1 = tryExpFold(std::move($1)); $$ = std::move($1); }
    | FuncLit { $1 = tryExpFold(std::move($1)); $$ = std::move($1); }
    ;
ArrayLit: LB ElemList RB { auto e = std::make_unique<ArrayExp>(); e->elements = std::move($2); $$ = std::move(e); }
    | LB RB { auto e = std::make_unique<ArrayExp>(); $$ = std::move(e); }
    ;
ElemList: ElemList COMMA Exp { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Exp { Zeta::ParserTypes::ExpList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
MapLit: LC MapElemList RC { auto e = std::make_unique<MapExp>(); e->entries = std::move($2); $$ = std::move(e); }
    | LC RC { auto e = std::make_unique<MapExp>(); $$ = std::move(e); }
    ;
MapElemList: MapElemList COMMA MapElem { $1.push_back(std::move($3)); $$ = std::move($1); }
    | MapElem { Zeta::ParserTypes::MapEntryList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
MapElem: ID COLON Exp { $$ = std::make_pair(std::move($1), std::move($3)); }
    | STR COLON Exp { $$ = std::make_pair(std::move($1), std::move($3)); }
    ;
FuncLit: FN LP ParamList RP BlockStmt { auto f = std::make_unique<FuncLitExp>(); f->params = std::move($3); $5->isFuncBody = true; f->body = std::move($5); $$ = std::move(f); }
    | FN LP RP BlockStmt { auto f = std::make_unique<FuncLitExp>(); $4->isFuncBody = true; f->body = std::move($4); $$ = std::move(f); }
    | error RC { $$ = nullptr; }
    ;

%%

void Zeta::Parser::error(const std::string& msg) {
    REPORT_SYNTAX_ERROR(yylineno, yycolumn, msg.c_str());
}

Zeta::AST::AssignExp::Op toAssignOp(AssignOp assignOp) {
    switch(assignOp) {
        case AS_ASSIGN: return Zeta::AST::AssignExp::Op::Assign;
        case AS_PLUS: return Zeta::AST::AssignExp::Op::AddAssign;
        case AS_MINUS: return Zeta::AST::AssignExp::Op::SubAssign;
        case AS_STAR: return Zeta::AST::AssignExp::Op::MulAssign;
        case AS_DIV: return Zeta::AST::AssignExp::Op::DivAssign;
        case AS_MOD: return Zeta::AST::AssignExp::Op::ModAssign;
        case AS_BITAND: return Zeta::AST::AssignExp::Op::BitAndAssign;
        case AS_BITOR: return Zeta::AST::AssignExp::Op::BitOrAssign;
        case AS_BITXOR: return Zeta::AST::AssignExp::Op::BitXorAssign;
        case AS_LSHIFT: return Zeta::AST::AssignExp::Op::ShlAssign;
        case AS_RSHIFT: return Zeta::AST::AssignExp::Op::ShrAssign;
        default: assert(false); return Zeta::AST::AssignExp::Op::Assign; 
    }
}

Zeta::AST::BinaryExp::Op relopToBinaryOp(Relop relop) {
    switch(relop) {
        case RP_EQ: return Zeta::AST::BinaryExp::Op::Eq;
        case RP_NEQ: return Zeta::AST::BinaryExp::Op::Neq;
        case RP_LT: return Zeta::AST::BinaryExp::Op::Lt;
        case RP_GT: return Zeta::AST::BinaryExp::Op::Gt;
        case RP_LEQ: return Zeta::AST::BinaryExp::Op::Leq;
        case RP_GEQ: return Zeta::AST::BinaryExp::Op::Geq;
        default: assert(false); return Zeta::AST::BinaryExp::Op::Eq; 
    }
}

bool boolean(const Zeta::AST::LiteralExp* exp){
    switch(exp->type){
        case Zeta::AST::LiteralExp::LiteralType::Int:
            return static_cast<const Zeta::AST::IntLitExp*>(exp)->value != 0;
        case Zeta::AST::LiteralExp::LiteralType::Float:
            return static_cast<const Zeta::AST::FloatLitExp*>(exp)->value != 0.0;
        case Zeta::AST::LiteralExp::LiteralType::Str:
            return !static_cast<const Zeta::AST::StrLitExp*>(exp)->value.empty();
        case Zeta::AST::LiteralExp::LiteralType::Bool:
            return static_cast<const Zeta::AST::BoolLitExp*>(exp)->value;
        case Zeta::AST::LiteralExp::LiteralType::Null:
            return false;
        case Zeta::AST::LiteralExp::LiteralType::Array:
            return !static_cast<const Zeta::AST::ArrayLitExp*>(exp)->elements.empty();
        case Zeta::AST::LiteralExp::LiteralType::Map:
            return !static_cast<const Zeta::AST::MapLitExp*>(exp)->entries.empty();
        default:
            return true; // function literals are always truthy
    }
}

std::unique_ptr<Zeta::AST::Exp> tryExpFold(std::unique_ptr<Zeta::AST::Exp> exp) {
    auto makeInt = [](int64_t value) {
        auto out = std::make_unique<Zeta::AST::IntLitExp>();
        out->value = value;
        return out;
    };
    auto makeFloat = [](double value) {
        auto out = std::make_unique<Zeta::AST::FloatLitExp>();
        out->value = value;
        return out;
    };
    auto makeStr = [](std::string value) {
        auto out = std::make_unique<Zeta::AST::StrLitExp>();
        out->value = std::move(value);
        return out;
    };
    auto makeBool = [](bool value) {
        auto out = std::make_unique<Zeta::AST::BoolLitExp>();
        out->value = value;
        return out;
    };
    switch(exp->type) {
        case Zeta::AST::Exp::ExpType::Conditional: {
            auto* condExp = static_cast<Zeta::AST::ConditionalExp*>(exp.get());
            if(condExp->cond->type == Zeta::AST::Exp::ExpType::Literal){
                auto* literalExp = static_cast<Zeta::AST::LiteralExp*>(condExp->cond.get());
                return boolean(literalExp) ? std::move(condExp->thenBranch) : std::move(condExp->elseBranch);
            }
            return std::move(exp);
        }
        case Zeta::AST::Exp::ExpType::Binary: {
            auto* binaryExp = static_cast<Zeta::AST::BinaryExp*>(exp.get());
            if(binaryExp->left->type == Zeta::AST::Exp::ExpType::Literal && binaryExp->right->type == Zeta::AST::Exp::ExpType::Literal){
                auto* leftLit = static_cast<Zeta::AST::LiteralExp*>(binaryExp->left.get());
                auto* rightLit = static_cast<Zeta::AST::LiteralExp*>(binaryExp->right.get());

                if(binaryExp->op == Zeta::AST::BinaryExp::Op::And){
                    return makeBool(boolean(leftLit) && boolean(rightLit));
                }
                if(binaryExp->op == Zeta::AST::BinaryExp::Op::Or){
                    return makeBool(boolean(leftLit) || boolean(rightLit));
                }

                const bool leftIsInt = leftLit->type == Zeta::AST::LiteralExp::LiteralType::Int;
                const bool rightIsInt = rightLit->type == Zeta::AST::LiteralExp::LiteralType::Int;
                const bool leftIsFloat = leftLit->type == Zeta::AST::LiteralExp::LiteralType::Float;
                const bool rightIsFloat = rightLit->type == Zeta::AST::LiteralExp::LiteralType::Float;
                const bool leftIsNum = leftIsInt || leftIsFloat;
                const bool rightIsNum = rightIsInt || rightIsFloat;

                if(leftIsNum && rightIsNum){
                    if(leftIsInt && rightIsInt){
                        const int64_t leftVal = static_cast<const Zeta::AST::IntLitExp*>(leftLit)->value;
                        const int64_t rightVal = static_cast<const Zeta::AST::IntLitExp*>(rightLit)->value;
                        switch(binaryExp->op){
                            case Zeta::AST::BinaryExp::Op::Add:
                                return makeInt(leftVal + rightVal);
                            case Zeta::AST::BinaryExp::Op::Sub:
                                return makeInt(leftVal - rightVal);
                            case Zeta::AST::BinaryExp::Op::Mul:
                                return makeInt(leftVal * rightVal);
                            case Zeta::AST::BinaryExp::Op::Div:
                                if(rightVal == 0){
                                    REPORT_SEMANTIC_ERROR(binaryExp->line, binaryExp->column, "Division by zero");
                                    return std::move(exp);
                                }
                                return makeInt(leftVal / rightVal);
                            case Zeta::AST::BinaryExp::Op::Mod:
                                if(rightVal == 0){
                                    REPORT_SEMANTIC_ERROR(binaryExp->line, binaryExp->column, "Modulo by zero");
                                    return std::move(exp);
                                }
                                return makeInt(leftVal % rightVal);
                            case Zeta::AST::BinaryExp::Op::Eq:
                                return makeBool(leftVal == rightVal);
                            case Zeta::AST::BinaryExp::Op::Neq:
                                return makeBool(leftVal != rightVal);
                            case Zeta::AST::BinaryExp::Op::Lt:
                                return makeBool(leftVal < rightVal);
                            case Zeta::AST::BinaryExp::Op::Gt:
                                return makeBool(leftVal > rightVal);
                            case Zeta::AST::BinaryExp::Op::Leq:
                                return makeBool(leftVal <= rightVal);
                            case Zeta::AST::BinaryExp::Op::Geq:
                                return makeBool(leftVal >= rightVal);
                            case Zeta::AST::BinaryExp::Op::BitAnd:
                                return makeInt(leftVal & rightVal);
                            case Zeta::AST::BinaryExp::Op::BitOr:
                                return makeInt(leftVal | rightVal);
                            case Zeta::AST::BinaryExp::Op::BitXor:
                                return makeInt(leftVal ^ rightVal);
                            case Zeta::AST::BinaryExp::Op::Shl:
                                return makeInt(leftVal << rightVal);
                            case Zeta::AST::BinaryExp::Op::Shr:
                                return makeInt(leftVal >> rightVal);
                            default:
                                break;
                        }
                    } else {
                        const double leftVal = leftIsFloat
                            ? static_cast<const Zeta::AST::FloatLitExp*>(leftLit)->value
                            : static_cast<const Zeta::AST::IntLitExp*>(leftLit)->value;
                        const double rightVal = rightIsFloat
                            ? static_cast<const Zeta::AST::FloatLitExp*>(rightLit)->value
                            : static_cast<const Zeta::AST::IntLitExp*>(rightLit)->value;
                        switch(binaryExp->op){
                            case Zeta::AST::BinaryExp::Op::Add:
                                return makeFloat(leftVal + rightVal);
                            case Zeta::AST::BinaryExp::Op::Sub:
                                return makeFloat(leftVal - rightVal);
                            case Zeta::AST::BinaryExp::Op::Mul:
                                return makeFloat(leftVal * rightVal);
                            case Zeta::AST::BinaryExp::Op::Div:
                                if(rightVal == 0.0){
                                    REPORT_SEMANTIC_ERROR(binaryExp->line, binaryExp->column, "Division by zero");
                                    return std::move(exp);
                                }
                                return makeFloat(leftVal / rightVal);
                            case Zeta::AST::BinaryExp::Op::Eq:
                                return makeBool(leftVal == rightVal);
                            case Zeta::AST::BinaryExp::Op::Neq:
                                return makeBool(leftVal != rightVal);
                            case Zeta::AST::BinaryExp::Op::Lt:
                                return makeBool(leftVal < rightVal);
                            case Zeta::AST::BinaryExp::Op::Gt:
                                return makeBool(leftVal > rightVal);
                            case Zeta::AST::BinaryExp::Op::Leq:
                                return makeBool(leftVal <= rightVal);
                            case Zeta::AST::BinaryExp::Op::Geq:
                                return makeBool(leftVal >= rightVal);
                            default:
                                break;
                        }
                    }
                }

                if(leftLit->type == Zeta::AST::LiteralExp::LiteralType::Str && rightLit->type == Zeta::AST::LiteralExp::LiteralType::Str){
                    const auto& leftVal = static_cast<const Zeta::AST::StrLitExp*>(leftLit)->value;
                    const auto& rightVal = static_cast<const Zeta::AST::StrLitExp*>(rightLit)->value;
                    switch(binaryExp->op){
                        case Zeta::AST::BinaryExp::Op::Add:
                            return makeStr(leftVal + rightVal);
                        case Zeta::AST::BinaryExp::Op::Eq:
                            return makeBool(leftVal == rightVal);
                        case Zeta::AST::BinaryExp::Op::Neq:
                            return makeBool(leftVal != rightVal);
                        default:
                            break;
                    }
                }

                if(leftLit->type == Zeta::AST::LiteralExp::LiteralType::Bool && rightLit->type == Zeta::AST::LiteralExp::LiteralType::Bool){
                    const bool leftVal = static_cast<const Zeta::AST::BoolLitExp*>(leftLit)->value;
                    const bool rightVal = static_cast<const Zeta::AST::BoolLitExp*>(rightLit)->value;
                    switch(binaryExp->op){
                        case Zeta::AST::BinaryExp::Op::Eq:
                            return makeBool(leftVal == rightVal);
                        case Zeta::AST::BinaryExp::Op::Neq:
                            return makeBool(leftVal != rightVal);
                        default:
                            break;
                    }
                }

                if(leftLit->type == Zeta::AST::LiteralExp::LiteralType::Null && rightLit->type == Zeta::AST::LiteralExp::LiteralType::Null){
                    switch(binaryExp->op){
                        case Zeta::AST::BinaryExp::Op::Eq:
                            return makeBool(true);
                        case Zeta::AST::BinaryExp::Op::Neq:
                            return makeBool(false);
                        default:
                            break;
                    }
                }
                REPORT_SEMANTIC_ERROR(binaryExp->line, binaryExp->column, "Unsupported operand types for constant expression");
                return std::move(exp);
            }
            return std::move(exp);
        }
        case Zeta::AST::Exp::ExpType::Unary: {
            auto* unaryExp = static_cast<Zeta::AST::UnaryExp*>(exp.get());
            if(unaryExp->operand->type == Zeta::AST::Exp::ExpType::Literal){
                auto* literalExp = static_cast<Zeta::AST::LiteralExp*>(unaryExp->operand.get());
                switch(unaryExp->op){
                    case Zeta::AST::UnaryExp::Op::Neg:
                        if(literalExp->type == Zeta::AST::LiteralExp::LiteralType::Int){
                            return makeInt(-static_cast<const Zeta::AST::IntLitExp*>(literalExp)->value);
                        }
                        if(literalExp->type == Zeta::AST::LiteralExp::LiteralType::Float){
                            return makeFloat(-static_cast<const Zeta::AST::FloatLitExp*>(literalExp)->value);
                        }
                        break;
                    case Zeta::AST::UnaryExp::Op::Not:
                        return makeBool(!boolean(literalExp));
                    case Zeta::AST::UnaryExp::Op::BitNot:
                        if(literalExp->type == Zeta::AST::LiteralExp::LiteralType::Int){
                            return makeInt(~static_cast<const Zeta::AST::IntLitExp*>(literalExp)->value);
                        }
                        break;
                    default:
                        break;
                }
                REPORT_SEMANTIC_ERROR(unaryExp->line, unaryExp->column, "Unsupported operand type for constant expression");
                return std::move(exp);
            }
            return std::move(exp);
        }
        case Zeta::AST::Exp::ExpType::Array: {
            auto* arrayExp = static_cast<Zeta::AST::ArrayExp*>(exp.get());
            bool allLiteral = true;
            for(const auto& elem : arrayExp->elements){
                if(elem->type != Zeta::AST::Exp::ExpType::Literal){
                    allLiteral = false;
                    break;
                }
            }
            if(allLiteral){
                auto out = std::make_unique<Zeta::AST::ArrayLitExp>();
                for(auto& elem : arrayExp->elements){
                    out->elements.push_back(std::unique_ptr<Zeta::AST::LiteralExp>(static_cast<Zeta::AST::LiteralExp*>(elem.release())));
                }
                return std::move(out);
            }
            return std::move(exp);
        }
        case Zeta::AST::Exp::ExpType::Map: {
            auto* mapExp = static_cast<Zeta::AST::MapExp*>(exp.get());
            bool allLiteral = true;
            for(const auto& entry : mapExp->entries){
                if(entry.second->type != Zeta::AST::Exp::ExpType::Literal){
                    allLiteral = false;
                    break;
                }
            }
            if(allLiteral){
                auto out = std::make_unique<Zeta::AST::MapLitExp>();
                for(auto& entry : mapExp->entries){
                    out->entries.emplace_back(std::move(entry.first), std::unique_ptr<Zeta::AST::LiteralExp>(static_cast<Zeta::AST::LiteralExp*>(entry.second.release())));
                }
                return std::move(out);
            }
            return std::move(exp);
        }
        default: return std::move(exp);
    }
}

