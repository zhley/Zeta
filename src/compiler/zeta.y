%code requires {
    #include "compiler/ast.h"
    #include "compiler/error.h"

    #include <memory>
    #include <string>
    #include <utility>
    #include <vector>
    #include <assert.h>

    enum Relop { RP_EQ, RP_NEQ, RP_LT, RP_GT, RP_LEQ, RP_GEQ, RP_IS };
    enum AssignOp { AS_ASSIGN, AS_PLUS, AS_MINUS, AS_STAR, AS_DIV, AS_MOD, AS_BITAND, AS_BITOR, AS_BITXOR, AS_LSHIFT, AS_RSHIFT };

    extern int yylineno;
    extern int yycolumn;

    Zeta::AST::AssignStmt::Op toAssignOp(AssignOp assignOp);
    Zeta::AST::BinaryExp::Op relopToBinaryOp(Relop relop);
    void yyerror(const char* msg);
    std::unique_ptr<Zeta::AST::Exp> tryExpFold(std::unique_ptr<Zeta::AST::Exp> exp);

    namespace Zeta::ParserTypes {
        using ImportList = std::vector<std::unique_ptr<Zeta::AST::Import>>;
        using DecList = std::vector<std::unique_ptr<Zeta::AST::Dec>>;
        using VarDecList = std::vector<std::unique_ptr<Zeta::AST::VarDec>>;
        using StmtList = std::vector<std::unique_ptr<Zeta::AST::Stmt>>;
        using ExpList = std::vector<std::unique_ptr<Zeta::AST::Exp>>;
        using MapEntryList = std::vector<std::pair<std::string, std::unique_ptr<Zeta::AST::Exp>>>;
        using ParamList = std::vector<std::string>;
    }

    #define SET_POS(node, loc) \
        do { \
            (node)->line = (loc).begin.line; \
            (node)->column = (loc).begin.column; \
        } while(0)
}

%code top{
    #include "compiler/ast.h"
    using namespace Zeta::AST;
}

%code {
    Zeta::Parser::symbol_type yylex();
}

%language "c++"
%locations
%define api.namespace {Zeta}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.token.constructor
%define parse.error verbose
%parse-param { std::unique_ptr<Zeta::AST::Program>& root }
%token END 0
%token <int64_t> INT
%token <double> FLOAT
%token <bool> BOOL
%token <Relop> RELOP
%token <AssignOp> ASSIGN
%token <std::string> ID STR
%token NULL_
%token VAR LET FN RETURN IF ELSE WHILE FOR BREAK CONTINUE CLASS EXTENDS THIS IMPORT AS SUPER
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
%type <Zeta::ParserTypes::DecList> ClassBody
%type <Zeta::ParserTypes::StmtList> StmtList
%type <std::unique_ptr<Zeta::AST::Stmt>> Stmt
%type <std::unique_ptr<Zeta::AST::BlockStmt>> BlockStmt
%type <std::unique_ptr<Zeta::AST::Exp>> Exp Literal ArrayLit MapLit FuncLit
%type <Zeta::ParserTypes::ExpList> ArgList ElemList
%type <Zeta::ParserTypes::MapEntryList> MapElemList
%type <std::pair<std::string, std::unique_ptr<Zeta::AST::Exp>>> MapElem

%%
Program: ImportList DecList { auto p = std::make_unique<Program>(); SET_POS(p, @$); p->imports = std::move($1); p->decs = std::move($2); $$ = std::move(p); root = std::move($$); }
    ;

ImportList: ImportList Import { $1.push_back(std::move($2)); $$ = std::move($1); }
    | { $$ = Zeta::ParserTypes::ImportList{}; }
    ;
Import: IMPORT STR SEMI { auto t = std::make_unique<Import>(); SET_POS(t, @$); t->path = std::move($2); $$ = std::move(t); }
    | IMPORT STR AS ID SEMI { auto t = std::make_unique<Import>(); SET_POS(t, @$); t->path = std::move($2); t->alias = std::move($4); $$ = std::move(t); }
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
Var: ID { auto t = std::make_unique<VarDec>(); SET_POS(t, @$); t->name = std::move($1); t->isMutable = true; $$ = std::move(t); }
    | ID ASSIGN Exp { auto t = std::make_unique<VarDec>(); SET_POS(t, @$); t->name = std::move($1); t->isMutable = true; t->init = std::move($3); $$ = std::move(t); if($2 != AS_ASSIGN) { Zeta::Parser::error(@2, "Initialization only allowed with '='"); } }
    ;

FuncDec: FN ID LP ParamList RP BlockStmt { auto f = std::make_unique<FuncDec>(); SET_POS(f, @$); f->name = std::move($2); f->params = std::move($4); $6->isFuncBody = true; f->body = std::move($6); $$ = std::move(f); }
    | FN ID LP RP BlockStmt { auto f = std::make_unique<FuncDec>(); SET_POS(f, @$); f->name = std::move($2); $5->isFuncBody = true; f->body = std::move($5); $$ = std::move(f); }
    ;
ParamList: ParamList COMMA Param { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Param { Zeta::ParserTypes::ParamList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
Param: ID { $$ = std::move($1); }
    ;

ClassDec: CLASS ID LC ClassBody RC { auto c = std::make_unique<ClassDec>(); SET_POS(c, @$); c->name = std::move($2); c->members = std::move($4); $$ = std::move(c); }
    | CLASS ID EXTENDS ID LC ClassBody RC { auto c = std::make_unique<ClassDec>(); SET_POS(c, @$); c->name = std::move($2); c->base = std::make_pair(std::string(), std::move($4)); c->members = std::move($6); $$ = std::move(c); }
    | CLASS ID EXTENDS ID DOT ID LC ClassBody RC { auto c = std::make_unique<ClassDec>(); SET_POS(c, @$); c->name = std::move($2); c->base = std::make_pair(std::move($4), std::move($6)); c->members = std::move($8); $$ = std::move(c); }
    ;
ClassBody: ClassBody VarDec SEMI { for(auto& d: $2) $1.push_back(std::unique_ptr<Dec>(std::move(d))); $$ = std::move($1); }
    | ClassBody FuncDec { $1.push_back(std::unique_ptr<Dec>(std::move($2))); $$ = std::move($1); }
    | { $$ = Zeta::ParserTypes::DecList{}; }
    ;

StmtList: StmtList Stmt { $1.push_back(std::move($2)); $$ = std::move($1); }
    | Stmt { Zeta::ParserTypes::StmtList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
BlockStmt: LC StmtList RC { auto b = std::make_unique<BlockStmt>(); SET_POS(b, @$); b->stmts = std::move($2); $$ = std::move(b); }
    | LC RC { auto b = std::make_unique<BlockStmt>(); SET_POS(b, @$); $$ = std::move(b); }
    ;
Stmt: VarDec SEMI { auto s = std::make_unique<VarDecStmt>(); SET_POS(s, @$); s->varDecs = std::move($1); $$ = std::move(s); }
    | BlockStmt { $$ = std::move($1); }
    | Exp ASSIGN Exp SEMI { auto s = std::make_unique<AssignStmt>(); SET_POS(s, @$); s->op = toAssignOp($2); s->target = std::move($1); s->value = std::move($3); $$ = std::move(s); }
    | Exp SEMI { auto s = std::make_unique<ExpStmt>(); SET_POS(s, @$); s->exp = std::move($1); $$ = std::move(s); }
    | IF LP Exp RP Stmt %prec ELSE { auto s = std::make_unique<IfStmt>(); SET_POS(s, @$); s->cond = std::move($3); s->thenBranch = std::move($5); $$ = std::move(s); }
    | IF LP Exp RP Stmt ELSE Stmt { auto s = std::make_unique<IfStmt>(); SET_POS(s, @$); s->cond = std::move($3); s->thenBranch = std::move($5); s->elseBranch = std::move($7); $$ = std::move(s); }
    | WHILE LP Exp RP Stmt { auto s = std::make_unique<WhileStmt>(); SET_POS(s, @$); s->cond = std::move($3); s->body = std::move($5); $$ = std::move(s); }
    | FOR LP ID COLON Exp RP Stmt { auto s = std::make_unique<ForStmt>(); SET_POS(s, @$); s->valVarName = std::move($3); s->iterable = std::move($5); s->body = std::move($7); $$ = std::move(s); }
    | BREAK SEMI { auto s = std::make_unique<BreakStmt>(); SET_POS(s, @$); $$ = std::move(s); }
    | CONTINUE SEMI { auto s = std::make_unique<ContinueStmt>(); SET_POS(s, @$); $$ = std::move(s); }
    | RETURN SEMI { auto s = std::make_unique<ReturnStmt>(); SET_POS(s, @$); $$ = std::move(s); }
    | RETURN Exp SEMI { auto s = std::make_unique<ReturnStmt>(); SET_POS(s, @$); s->value = std::move($2); $$ = std::move(s); }
    | SEMI { $$ = nullptr; }
    ;

Exp: Exp QMARK Exp COLON Exp { auto e = std::make_unique<ConditionalExp>(); SET_POS(e, @$); e->cond = std::move($1); e->thenBranch = std::move($3); e->elseBranch = std::move($5); $$ = tryExpFold(std::move(e)); }
    | Exp RELOP Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = relopToBinaryOp($2); e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp AND Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::And; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp OR Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Or; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp LSHIFT Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Shl; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp RSHIFT Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Shr; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp PLUS Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Add; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp MINUS Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Sub; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp STAR Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Mul; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp DIV Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Div; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp MOD Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::Mod; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp BITAND Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::BitAnd; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp BITOR Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::BitOr; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | Exp BITXOR Exp { auto e = std::make_unique<BinaryExp>(); SET_POS(e, @$); e->op = BinaryExp::Op::BitXor; e->left = std::move($1); e->right = std::move($3); $$ = tryExpFold(std::move(e)); }
    | MINUS Exp %prec MINUS_S { auto e = std::make_unique<UnaryExp>(); SET_POS(e, @$); e->op = UnaryExp::Op::Neg; e->operand = std::move($2); $$ = tryExpFold(std::move(e)); }
    | NOT Exp { auto e = std::make_unique<UnaryExp>(); SET_POS(e, @$); e->op = UnaryExp::Op::Not; e->operand = std::move($2); $$ = tryExpFold(std::move(e)); }
    | BITNOT Exp { auto e = std::make_unique<UnaryExp>(); SET_POS(e, @$); e->op = UnaryExp::Op::BitNot; e->operand = std::move($2); $$ = tryExpFold(std::move(e)); }
    | LP Exp RP { $$ = std::move($2); }
    | Exp DOT ID LP ArgList RP { auto e = std::make_unique<CallExp>(); SET_POS(e, @$); e->caller = std::move($1); e->funcName = std::move($3); e->args = std::move($5); $$ = std::move(e); }
    | Exp DOT ID LP RP { auto e = std::make_unique<CallExp>(); SET_POS(e, @$); e->caller = std::move($1); e->funcName = std::move($3); $$ = std::move(e); }
    | Exp DOT ID { auto e = std::make_unique<MemberAccessExp>(); SET_POS(e, @$); e->object = std::move($1); e->member = std::move($3); $$ = std::move(e); }
    | Exp LB Exp RB { auto e = std::make_unique<IndexAccessExp>(); SET_POS(e, @$); e->object = std::move($1); e->index = std::move($3); $$ = std::move(e); }
    // TODO: 不支持对任意表达式直接调用 (如 f()(x) 或 arr[0](x)), 只有 ID(...) 和 Exp.method(...) 两种调用形式.
    // 设计时的遗漏, 当时没有考虑到表达式可以产生函数. 需要时添加规则: | Exp LP ArgList RP / | Exp LP RP
    | ID LP ArgList RP { auto e = std::make_unique<CallExp>(); SET_POS(e, @$); e->funcName = std::move($1); e->args = std::move($3); $$ = std::move(e); }
    | ID LP RP { auto e = std::make_unique<CallExp>(); SET_POS(e, @$); e->funcName = std::move($1); $$ = std::move(e); }
    | SUPER DOT ID LP ArgList RP { auto e = std::make_unique<SuperCallExp>(); SET_POS(e, @$); e->methodName = std::move($3); e->args = std::move($5); $$ = std::move(e); }
    | SUPER DOT ID LP RP { auto e = std::make_unique<SuperCallExp>(); SET_POS(e, @$); e->methodName = std::move($3); $$ = std::move(e); }
    | ID { auto e = std::make_unique<IdentifierExp>(); SET_POS(e, @$); e->name = std::move($1); $$ = std::move(e); }
    | Literal { $$ = std::move($1); }
    | THIS { auto e = std::make_unique<ThisExp>(); SET_POS(e, @$); $$ = std::move(e); }
    ;
ArgList: ArgList COMMA Exp { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Exp { Zeta::ParserTypes::ExpList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
Literal: INT { auto e = std::make_unique<IntLitExp>(); SET_POS(e, @$); e->value = $1; $$ = std::move(e); }
    | FLOAT { auto e = std::make_unique<FloatLitExp>(); SET_POS(e, @$); e->value = $1; $$ = std::move(e); }
    | BOOL { auto e = std::make_unique<BoolLitExp>(); SET_POS(e, @$); e->value = $1; $$ = std::move(e); }
    | NULL_ { auto e = std::make_unique<NullLitExp>(); SET_POS(e, @$); $$ = std::move(e); }
    | STR { auto e = std::make_unique<StrLitExp>(); SET_POS(e, @$); e->value = std::move($1); $$ = std::move(e); }
    | ArrayLit { $$ = std::move($1); }
    | MapLit { $1 = tryExpFold(std::move($1)); $$ = std::move($1); }
    | FuncLit { $1 = tryExpFold(std::move($1)); $$ = std::move($1); }
    ;
ArrayLit: LB ElemList RB { auto e = std::make_unique<ArrayExp>(); SET_POS(e, @$); e->elements = std::move($2); $$ = std::move(e); }
    | LB RB { auto e = std::make_unique<ArrayExp>(); SET_POS(e, @$); $$ = std::move(e); }
    ;
ElemList: ElemList COMMA Exp { $1.push_back(std::move($3)); $$ = std::move($1); }
    | Exp { Zeta::ParserTypes::ExpList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
MapLit: LC MapElemList RC { auto e = std::make_unique<MapExp>(); SET_POS(e, @$); e->entries = std::move($2); $$ = std::move(e); }
    | LC COLON RC { auto e = std::make_unique<MapExp>(); SET_POS(e, @$); $$ = std::move(e); }
    ;
MapElemList: MapElemList COMMA MapElem { $1.push_back(std::move($3)); $$ = std::move($1); }
    | MapElem { Zeta::ParserTypes::MapEntryList t; t.push_back(std::move($1)); $$ = std::move(t); }
    ;
MapElem: ID COLON Exp { $$ = std::make_pair(std::move($1), std::move($3)); }
    | STR COLON Exp { $$ = std::make_pair(std::move($1), std::move($3)); }
    ;
FuncLit: FN LP ParamList RP BlockStmt { auto f = std::make_unique<FuncLitExp>(); SET_POS(f, @$); f->params = std::move($3); $5->isFuncBody = true; f->body = std::move($5); $$ = std::move(f); }
    | FN LP RP BlockStmt { auto f = std::make_unique<FuncLitExp>(); SET_POS(f, @$); $4->isFuncBody = true; f->body = std::move($4); $$ = std::move(f); }
    ;

%%

void Zeta::Parser::error(const location_type& loc, const std::string& msg) {
    REPORT_SYNTAX_ERROR(loc.begin.line, loc.begin.column, "{}", msg);
}

Zeta::AST::AssignStmt::Op toAssignOp(AssignOp assignOp) {
    switch(assignOp) {
        case AS_ASSIGN: return Zeta::AST::AssignStmt::Op::Assign;
        case AS_PLUS: return Zeta::AST::AssignStmt::Op::AddAssign;
        case AS_MINUS: return Zeta::AST::AssignStmt::Op::SubAssign;
        case AS_STAR: return Zeta::AST::AssignStmt::Op::MulAssign;
        case AS_DIV: return Zeta::AST::AssignStmt::Op::DivAssign;
        case AS_MOD: return Zeta::AST::AssignStmt::Op::ModAssign;
        case AS_BITAND: return Zeta::AST::AssignStmt::Op::BitAndAssign;
        case AS_BITOR: return Zeta::AST::AssignStmt::Op::BitOrAssign;
        case AS_BITXOR: return Zeta::AST::AssignStmt::Op::BitXorAssign;
        case AS_LSHIFT: return Zeta::AST::AssignStmt::Op::ShlAssign;
        case AS_RSHIFT: return Zeta::AST::AssignStmt::Op::ShrAssign;
        default: assert(false); return Zeta::AST::AssignStmt::Op::Assign; 
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
        case RP_IS: return Zeta::AST::BinaryExp::Op::Is;
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
    if(!exp) return nullptr;
    int line = exp->line;
    int column = exp->column;
    auto makeInt = [line, column](int64_t value) {
        auto out = std::make_unique<Zeta::AST::IntLitExp>();
        out->value = value;
        out->line = line;
        out->column = column;
        return out;
    };
    auto makeFloat = [line, column](double value) {
        auto out = std::make_unique<Zeta::AST::FloatLitExp>();
        out->value = value;
        out->line = line;
        out->column = column;
        return out;
    };
    auto makeStr = [line, column](std::string value) {
        auto out = std::make_unique<Zeta::AST::StrLitExp>();
        out->value = std::move(value);
        out->line = line;
        out->column = column;
        return out;
    };
    auto makeBool = [line, column](bool value) {
        auto out = std::make_unique<Zeta::AST::BoolLitExp>();
        out->value = value;
        out->line = line;
        out->column = column;
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
                    // NOTE: 与运行时短路求值一致 (见 Translator::visit(BinaryExp)):
                    // 左操作数为假则结果为左操作数, 否则为右操作数. 结果不一定是 Bool.
                    return boolean(leftLit) ? std::move(binaryExp->right) : std::move(binaryExp->left);
                }
                if(binaryExp->op == Zeta::AST::BinaryExp::Op::Or){
                    // NOTE: 与运行时短路求值一致: 左操作数为真则结果为左操作数, 否则为右操作数. 结果不一定是 Bool.
                    return boolean(leftLit) ? std::move(binaryExp->left) : std::move(binaryExp->right);
                }
                if(binaryExp->op == Zeta::AST::BinaryExp::Op::Is){
                    if(leftLit->type == rightLit->type){
                        switch (leftLit->type){
                            case Zeta::AST::LiteralExp::LiteralType::Int:
                                return makeBool(static_cast<const Zeta::AST::IntLitExp*>(leftLit)->value == static_cast<const Zeta::AST::IntLitExp*>(rightLit)->value);
                            case Zeta::AST::LiteralExp::LiteralType::Float:
                                return makeBool(static_cast<const Zeta::AST::FloatLitExp*>(leftLit)->value == static_cast<const Zeta::AST::FloatLitExp*>(rightLit)->value);
                            case Zeta::AST::LiteralExp::LiteralType::Str:
                                return makeBool(static_cast<const Zeta::AST::StrLitExp*>(leftLit)->value == static_cast<const Zeta::AST::StrLitExp*>(rightLit)->value);
                            case Zeta::AST::LiteralExp::LiteralType::Bool:
                                return makeBool(static_cast<const Zeta::AST::BoolLitExp*>(leftLit)->value == static_cast<const Zeta::AST::BoolLitExp*>(rightLit)->value);
                            case Zeta::AST::LiteralExp::LiteralType::Null:
                                return makeBool(true);
                            default:
                                return std::move(exp);
                        }
                    }
                    return makeBool(false);
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
                if(binaryExp->op == Zeta::AST::BinaryExp::Op::Eq) {
                    return makeBool(false);
                } else if(binaryExp->op == Zeta::AST::BinaryExp::Op::Neq) {
                    return makeBool(true);
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

