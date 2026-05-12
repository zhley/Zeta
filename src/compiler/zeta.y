%code requires {
    #include "syntax.tab.h"
    #include "compiler/ast.h"
    #include "error.h"

    #include <stdint.h>
    #include <stdlib.h>

    enum Relop { RP_EQ, RP_NEQ, RP_LT, RP_GT, RP_LEQ, RP_GEQ };
    enum AssignOp { AS_ASSIGN, AS_PLUS, AS_MINUS, AS_STAR, AS_DIV, AS_MOD, AS_BITAND, AS_BITOR, AS_BITXOR, AS_LSHIFT, AS_RSHIFT };

    extern int yylineno;
    extern int yycolumn;
    extern int yylex();
    extern void yyerror(const char*);

    Zeta::Ast::AssignExp::Op toAssignOp(int assignOp);
    Zeta::Ast::BinaryExp::Op relopToBinaryOp(int relop); 
}

%code top{
    using namespace Zeta::Ast;
}

%define parse.error verbose
%union {
    int64_t int_;
    double double_;
    char* str;
    void* ptr;
}

%token <int_> INT BOOL NULL_ RELOP 
%token <double_> FLOAT
%token <str> ID STR
%token VAR LET FN RETURN IF ELSE WHILE FOR BREAK CONTINUE CLASS EXTENDS THIS STATIC IMPORT AS
%token AND OR LSHIFT RSHIFT SEMI DOT COMMA COLON QMARK LP RP LB RB LC RC ASSIGN PLUS MINUS STAR DIV MOD NOT BITAND BITOR BITXOR BITNOT

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

%type <ptr> Program ImportList Import DecList VarDec VarList Var
%type <ptr> FuncDec ParamList Param ClassDec ClassBody ClassMember
%type <ptr> StmtList Stmt BlockStmt
%type <ptr> Exp ArgList Literal ArrayLit ElemList MapLit MapElemList MapElem
%type <ptr> FuncLit

%%
Program: ImportList DecList { auto p = new Program(); p->imports = std::move(*((std::vector<std::unique_ptr<Import>>*)$1)); p->decs = std::move(*((std::vector<std::unique_ptr<Dec>>*)$2)); delete (std::vector<std::unique_ptr<Import>>*)$1; delete (std::vector<std::unique_ptr<Dec>>*)$2; $$ = p; }
    ;

ImportList: ImportList Import { (std::vector<std::unique_ptr<Import>>*)$1->push_back(std::unique_ptr<Import>((Import*)$2)); $$ = $1; }
    | { $$ = new std::vector<std::unique_ptr<Import>>(); }
    ;
Import: IMPORT STR SEMI { auto t = new Import(); t->path = std::string($2); $$ = t; }
    | IMPORT STR AS ID SEMI { auto t = new Import(); t->path = std::string($2); t->alias = std::string($4); $$ = t; }
    | error SEMI { $$ = nullptr; }
    ;

DecList: DecList VarDec SEMI { (std::vector<std::unique_ptr<Dec>>*)$1->insert($1->end(), (std::vector<std::unique_ptr<VarDec>>*)$2->begin(), (std::vector<std::unique_ptr<VarDec>>*)$2->end()); $$ = $1; delete (std::vector<std::unique_ptr<VarDec>>*)$2; }
    | DecList FuncDec { (std::vector<std::unique_ptr<Dec>>*)$1->push_back(std::unique_ptr<Dec>((Dec*)$2)); $$ = $1; }
    | DecList ClassDec { (std::vector<std::unique_ptr<Dec>>*)$1->push_back(std::unique_ptr<Dec>((Dec*)$2)); $$ = $1; }
    | { $$ = new std::vector<std::unique_ptr<Dec>>(); }
    ;

VarDec: LET VarLists { $$ = $2; for(auto& v: *(std::vector<std::unique_ptr<VarDec>>*)$$) v->isMutable = false; }
    | VAR VarList { $$ = $2; }
    ;
VarList: VarList COMMA Var { (std::vector<std::unique_ptr<VarDec>>*)$1->push_back(std::unique_ptr<VarDec>((VarDec*)$3)); $$ = $1; }
    | Var { auto t = new std::vector<std::unique_ptr<VarDec>>(); t->push_back(std::unique_ptr<VarDec>((VarDec*)$1)); $$ = t; }
    ;
Var: ID { auto t = new VarDec(); t->name = std::string($1); t->isMutable = true; $$ = t; }
    | ID ASSIGN Exp { auto t = new VarDec(); t->name = std::string($1); t->isMutable = true; t->init = std::unique_ptr<Exp>((Exp*)$3); $$ = t; if($2 != AS_ASSIGN) { yyerror("Initialization only allowed with '='"); } }
    ;

FuncDec: FN ID LP ParamList RP BlockStmt { auto f = new FuncDec(); f->name = std::string($2); f->params = std::move(*((std::vector<std::string>*)$4)); delete (std::vector<std::string>*)$4; f->body = std::unique_ptr<BlockStmt>((BlockStmt*)$6); $$ = f; }
    | FN ID LP RP BlockStmt { auto f = new FuncDec(); f->name = std::string($2); f->body = std::unique_ptr<BlockStmt>((BlockStmt*)$6); $$ = f; }
    | error RP BlockStmt { $$ = nullptr; }
    | error RC { $$ = nullptr; }
    ;
ParamList: ParamList COMMA Param { (std::vector<std::string>*)$1->push_back(std::string($3)); $$ = $1; }
    | Param { auto t = new std::vector<std::string>(); t->push_back(std::string($1)); $$ = t; }
    ;
Param: ID { $$ = $1; }
    ;

ClassDec: CLASS ID LC ClassBody RC { auto c = new ClassDec(); c->name = std::string($2); c->members = std::move(*((std::vector<std::pair<bool, std::unique_ptr<Dec>>>*)$4)); delete (std::vector<std::pair<bool, std::unique_ptr<Dec>>*)$4; $$ = c; }
    | CLASS ID EXTENDS ID LC ClassBody RC { auto c = new ClassDec(); c->name = std::string($2); c->base = std::string($4); c->members = std::move(*((std::vector<std::pair<bool, std::unique_ptr<Dec>>>*)$6)); delete (std::vector<std::pair<bool, std::unique_ptr<Dec>>*)$6; $$ = c; }
    | error LC ClassBody RC { $$ = nullptr; }
    | error RC { $$ = nullptr; }
    ;
ClassBody: ClassBody ClassMember { (std::vector<std::pair<bool, std::unique_ptr<Dec>>>*)$1->push_back(std::move(*((std::pair<bool, std::unique_ptr<Dec>>*)$2))); delete (std::pair<bool, std::unique_ptr<Dec>>*)$2; $$ = $1; }
    | { $$ = new std::vector<std::pair<bool, std::unique_ptr<Dec>>>(); }
    ;
ClassMember: VarDec { auto m = new std::pair<bool, std::unique_ptr<VarDec>>(false, std::unique_ptr<VarDec>((VarDec*)$1)); $$ = m; }
    | FuncDec { auto m = new std::pair<bool, std::unique_ptr<FuncDec>>(false, std::unique_ptr<FuncDec>((FuncDec*)$1)); $$ = m; }
    | STATIC FuncDec { auto m = new std::pair<bool, std::unique_ptr<FuncDec>>(true, std::unique_ptr<FuncDec>((FuncDec*)$2)); $$ = m; }
    | STATIC VarDec { auto m = new std::pair<bool, std::unique_ptr<VarDec>>(true, std::unique_ptr<VarDec>((VarDec*)$2)); $$ = m; }
    ;

StmtList: StmtList Stmt { (std::vector<std::unique_ptr<Stmt>>*)$1->push_back(std::unique_ptr<Stmt>((Stmt*)$2)); $$ = $1; }
    | { $$ = new std::vector<std::unique_ptr<Stmt>>(); }
    ;
BlockStmt: LC StmtList RC { auto b = new BlockStmt(); b->stmts = std::move(*((std::vector<std::unique_ptr<Stmt>>*)$2)); delete (std::vector<std::unique_ptr<Stmt>>*)$2; $$ = b; }
    ;
Stmt: VarDec SEMI { auto s = new VarDecStmt(); s->varDecs = std::move(*((std::vector<std::unique_ptr<VarDec>>*)$1)); delete (std::vector<std::unique_ptr<VarDec>>*)$1; $$ = s; }
    | Exp SEMI { auto s = new ExpStmt(); s->exp = std::unique_ptr<Exp>((Exp*)$1); $$ = s; }
    | IF LP Exp RP Stmt %prec ELSE { auto s = new IfStmt(); s->cond = std::unique_ptr<Exp>((Exp*)$3); s->thenBranch = std::unique_ptr<Stmt>((Stmt*)$5); $$ = s; }
    | IF LP Exp RP Stmt ELSE Stmt { auto s = new IfStmt(); s->cond = std::unique_ptr<Exp>((Exp*)$3); s->thenBranch = std::unique_ptr<Stmt>((Stmt*)$5); s->elseBranch = std::unique_ptr<Stmt>((Stmt*)$7); $$ = s; }
    | WHILE LP Exp RP Stmt { auto s = new WhileStmt(); s->cond = std::unique_ptr<Exp>((Exp*)$3); s->body = std::unique_ptr<Stmt>((Stmt*)$5); $$ = s; }
    | FOR LP ID COLON Exp RP Stmt { auto s = new ForStmt(); s->valVarName = std::string($3); s->iterable = std::unique_ptr<Exp>((Exp*)$5); s->body = std::unique_ptr<Stmt>((Stmt*)$7); $$ = s; }
    | FOR LP ID COMMA ID COLON Exp RP Stmt { auto s = new ForStmt(); s->idxVarName = std::string($3); s->valVarName = std::string($5); s->iterable = std::unique_ptr<Exp>((Exp*)$7); s->body = std::unique_ptr<Stmt>((Stmt*)$9); $$ = s; }
    | BREAK SEMI { auto s = new BreakStmt(); $$ = s; }
    | CONTINUE SEMI { auto s = new ContinueStmt(); $$ = s; }
    | RETURN SEMI { auto s = new ReturnStmt(); $$ = s; }
    | RETURN Exp SEMI { auto s = new ReturnStmt(); s->value = std::unique_ptr<Exp>((Exp*)$2); $$ = s; }
    | BlockStmt { $$ = $1; }
    | SEMI { $$ = nullptr; }
    | error SEMI { $$ = nullptr; }
    | error RC { $$ = nullptr; }
    ;

Exp: Exp QMARK Exp COLON Exp { auto e = new ConditionalExp(); e->cond = std::unique_ptr<Exp>((Exp*)$1); e->thenBranch = std::unique_ptr<Exp>((Exp*)$3); e->elseBranch = std::unique_ptr<Exp>((Exp*)$5); $$ = e; }
    | Exp ASSIGN Exp { auto e = new AssignExp(); e->op = toAssignOp($2); e->target = std::unique_ptr<Exp>((Exp*)$1); e->value = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp RELOP Exp { auto e = new BinaryExp(); e->op = relopToBinaryOp($2); e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp AND Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::And; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp OR Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Or; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp LSHIFT Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Shl; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp RSHIFT Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Shr; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp PLUS Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Plus; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp MINUS Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Minus; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp STAR Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Star; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp DIV Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Div; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp MOD Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::Mod; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp BITAND Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::BitAnd; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp BITOR Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::BitOr; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Exp BITXOR Exp { auto e = new BinaryExp(); e->op = BinaryExp::Op::BitXor; e->left = std::unique_ptr<Exp>((Exp*)$1); e->right = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | Minus Exp %prec MINUS_S { auto e = new UnaryExp(); e->op = UnaryExp::Op::Neg; e->operand = std::unique_ptr<Exp>((Exp*)$2); $$ = e; }
    | NOT Exp { auto e = new UnaryExp(); e->op = UnaryExp::Op::Not; e->operand = std::unique_ptr<Exp>((Exp*)$2); $$ = e; }
    | BITNOT Exp { auto e = new UnaryExp(); e->op = UnaryExp::Op::BitNot; e->operand = std::unique_ptr<Exp>((Exp*)$2); $$ = e; }
    | LP Exp RP { $$ = $2; }
    | Exp DOT ID LP ArgList RP { auto e = new CallExp(); e->caller = std::unique_ptr<Exp>((Exp*)$1); e->funcName = std::string($3); e->args = std::move(*((std::vector<std::unique_ptr<Exp>>*)$5)); delete (std::vector<std::unique_ptr<Exp>>*)$5; $$ = e; }
    | Exp DOT ID LP RP { auto e = new CallExp(); e->caller = std::unique_ptr<Exp>((Exp*)$1); e->funcName = std::string($3); $$ = e; }
    | Exp DOT ID { auto e = new MemberAccessExp(); e->object = std::unique_ptr<Exp>((Exp*)$1); e->member = std::string($3); $$ = e; }
    | Exp LB Exp RB { auto e = new IndexAccessExp(); e->object = std::unique_ptr<Exp>((Exp*)$1); e->index = std::unique_ptr<Exp>((Exp*)$3); $$ = e; }
    | ID LP ArgList RP { auto e = new CallExp(); e->funcName = std::string($1); e->args = std::move(*((std::vector<std::unique_ptr<Exp>>*)$3)); delete (std::vector<std::unique_ptr<Exp>>*)$3; $$ = e; }
    | ID LP RP { auto e = new CallExp(); e->funcName = std::string($1); $$ = e; }
    | ID { auto e = new IdentifierExp(); e->name = std::string($1); $$ = e; }
    | Literal { $$ = $1; }
    | THIS { auto e = new ThisExp(); $$ = e; }
    ;
ArgList: ArgList COMMA Exp { (std::vector<std::unique_ptr<Exp>>*)$1->push_back(std::unique_ptr<Exp>((Exp*)$3)); $$ = $1; }
    | Exp { auto t = new std::vector<std::unique_ptr<Exp>>(); t->push_back(std::unique_ptr<Exp>((Exp*)$1)); $$ = t; }
    ;
Literal: INT { auto e = new IntLitExp(); e->value = $1; $$ = e; }
    | FLOAT { auto e = new FloatLitExp(); e->value = $1; $$ = e; }
    | BOOL { auto e = new BoolLitExp(); e->value = $1; $$ = e; }
    | NULL_ { auto e = new NullLitExp(); $$ = e; }
    | STR { auto e = new StrLitExp(); e->value = std::string($1); $$ = e; }
    | ArrayLit { $$ = $1; }
    | MapLit { $$ = $1; }
    | FuncLit { $$ = $1; }
    ;
ArrayLit: LB ElemList RB { auto e = new ArrayLitExp(); e->elements = std::move(*((std::vector<std::unique_ptr<Exp>>*)$2)); delete (std::vector<std::unique_ptr<Exp>>*)$2; $$ = e; }
    | LB RB { auto e = new ArrayLitExp(); $$ = e; }
    ;
ElemList: ElemList COMMA Exp { (std::vector<std::unique_ptr<Exp>>*)$1->push_back(std::unique_ptr<Exp>((Exp*)$3)); $$ = $1; }
    | Exp { auto e = new std::vector<std::unique_ptr<Exp>>(); e->push_back(std::unique_ptr<Exp>((Exp*)$1)); $$ = e; }
    ;
MapLit: LC MapElemList RC { auto e = new MapLitExp(); e->entries = std::move(*((std::vector<std::pair<std::string, std::unique_ptr<Exp>>>*)$2)); delete (std::vector<std::pair<std::string, std::unique_ptr<Exp>>*)$2; $$ = e; }
    | LC RC { auto e = new MapLitExp(); $$ = e; }
    ;
MapElemList: MapElemList COMMA MapElem { (std::vector<std::pair<std::string, std::unique_ptr<Exp>>>*)$1->push_back(std::move(*((std::pair<std::string, std::unique_ptr<Exp>>*)$3))); delete (std::pair<std::string, std::unique_ptr<Exp>>*)$3; $$ = $1; }
    | MapElem { auto t = new std::vector<std::pair<std::string, std::unique_ptr<Exp>>>(); t->push_back(std::move(*((std::pair<std::string, std::unique_ptr<Exp>>*)$1))); delete (std::pair<std::string, std::unique_ptr<Exp>>*)$1; $$ = t; }
    ;
MapElem: ID COLON Exp { auto m = new std::pair<std::string, std::unique_ptr<Exp>>(std::string($1), std::unique_ptr<Exp>((Exp*)$3)); $$ = m; }
    | STR COLON Exp { auto m = new std::pair<std::string, std::unique_ptr<Exp>>(std::string($1), std::unique_ptr<Exp>((Exp*)$3)); $$ = m; }
    ;
FuncLit: FN LP ParamList RP BlockStmt { auto f = new FuncLitExp(); f->params = std::move(*((std::vector<std::string>*)$3)); delete (std::vector<std::string>*)$3; f->body = std::unique_ptr<BlockStmt>((BlockStmt*)$6); $$ = f; }
    | FN LP RP BlockStmt { auto f = new FuncLitExp(); f->body = std::unique_ptr<BlockStmt>((BlockStmt*)$4); $$ = f; }
    | error RC { $$ = nullptr; }
    ;

%%

void yyerror(const char* msg) {
    REPORT_SYNTAX_ERROR(yylineno, yycolumn, msg);
}

Zeta::Ast::AssignExp::Op toAssignOp(int assignOp) {
    switch(assignOp) {
        case AS_ASSIGN: return Zeta::Ast::AssignExp::Op::Assign;
        case AS_PLUS: return Zeta::Ast::AssignExp::Op::PlusAssign;
        case AS_MINUS: return Zeta::Ast::AssignExp::Op::MinusAssign;
        case AS_STAR: return Zeta::Ast::AssignExp::Op::StarAssign;
        case AS_DIV: return Zeta::Ast::AssignExp::Op::DivAssign;
        case AS_MOD: return Zeta::Ast::AssignExp::Op::ModAssign;
        case AS_BITAND: return Zeta::Ast::AssignExp::Op::BitAndAssign;
        case AS_BITOR: return Zeta::Ast::AssignExp::Op::BitOrAssign;
        case AS_BITXOR: return Zeta::Ast::AssignExp::Op::BitXorAssign;
        case AS_LSHIFT: return Zeta::Ast::AssignExp::Op::ShlAssign;
        case AS_RSHIFT: return Zeta::Ast::AssignExp::Op::ShrAssign;
        default: yyerror("Unknown assignment operator"); return Zeta::Ast::AssignExp::Op::Assign; 
    }
}

Zeta::Ast::BinaryExp::Op relopToBinaryOp(int relop) {
    switch(relop) {
        case RP_EQ: return Zeta::Ast::BinaryExp::Op::Eq;
        case RP_NEQ: return Zeta::Ast::BinaryExp::Op::Neq;
        case RP_LT: return Zeta::Ast::BinaryExp::Op::Lt;
        case RP_GT: return Zeta::Ast::BinaryExp::Op::Gt;
        case RP_LEQ: return Zeta::Ast::BinaryExp::Op::Leq;
        case RP_GEQ: return Zeta::Ast::BinaryExp::Op::Geq;
        default: yyerror("Unknown relational operator"); return Zeta::Ast::BinaryExp::Op::Eq; 
    }
}
