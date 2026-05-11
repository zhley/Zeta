%code requires {
    #include "syntax.tab.h"
    #include "compiler/ast.h"

    #include <stdint.h>
    #include <stdlib.h>

    enum Relop { RP_EQ, RP_NEQ, RP_LT, RP_GT, RP_LEQ, RP_GEQ };
    enum AssignOp { AS_ASSIGN, AS_PLUS, AS_MINUS, AS_STAR, AS_DIV, AS_MOD, AS_BITAND, AS_BITOR, AS_BITXOR, AS_LSHIFT, AS_RSHIFT };

    extern int yylineno;
    extern int yylex();
    extern void yyerror(const char*);
}

%define parse.error verbose
%union {
    int64_t int_;
    double double_;
    char* str;
    void* node;
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

%type <node> Program ImportList Import DecList Dec VarDec VarList Var
%type <node> FuncDec ParamList Param ClassDec ClassBody ClassMember
%type <node> StmtList Stmt
%type <node> Exp ArgList Literal ArrayLit ElemList MapLit MapElemList MapElem
%type <node> FuncLit

%%
Program: ImportList DecList
    ;

ImportList: ImportList Import
    |
    ;
Import: IMPORT STR SEMI
    | IMPORT STR AS ID SEMI
    | error SEMI
    ;

DecList: DecList Dec
    |
    ;
Dec: VarDec SEMI
    | FuncDec
    | ClassDec
    | error SEMI
    ;

VarDec: LET VarList
    | VAR VarList
    ;
VarList: VarList COMMA Var
    | Var
    ;
Var: ID
    | ID ASSIGN Exp
    ;

FuncDec: FN ID LP ParamList RP LC StmtList RC
    | error LC StmtList RC
    | error RC
    ;
ParamList: ParamList COMMA Param
    |
    ;
Param: ID
    ;

ClassDec: CLASS ID LC ClassBody RC
    | CLASS ID EXTENDS ID LC ClassBody RC
    | error LC ClassBody RC
    | error RC
    ;
ClassBody: ClassBody ClassMember
    |
    ;
ClassMember: VarDec
    | FuncDec
    | STATIC FuncDec
    | STATIC VarDec
    ;

StmtList: StmtList Stmt
    |
    ;
Stmt: VarDec SEMI
    | Exp SEMI
    | IF LP Exp RP Stmt %prec ELSE
    | IF LP Exp RP Stmt ELSE Stmt
    | WHILE LP Exp RP Stmt
    | FOR LP ID COLON Exp RP Stmt
    | FOR LP ID COMMA ID COLON Exp RP Stmt
    | BREAK SEMI
    | CONTINUE SEMI
    | RETURN SEMI
    | RETURN Exp SEMI
    | LC StmtList RC
    | SEMI
    | error SEMI
    | error RC
    ;

Exp: Exp QMARK Exp COLON Exp
    | Exp ASSIGN Exp
    | Exp RELOP Exp
    | Exp AND Exp
    | Exp OR Exp
    | Exp LSHIFT Exp
    | Exp RSHIFT Exp
    | Exp PLUS Exp
    | Exp MINUS Exp
    | Exp STAR Exp
    | Exp DIV Exp
    | Exp MOD Exp
    | Exp BITAND Exp
    | Exp BITOR Exp
    | Exp BITXOR Exp
    | Minus Exp %prec MINUS_S
    | NOT Exp
    | BITNOT Exp
    | LP Exp RP
    | Exp DOT ID LP ArgList RP
    | Exp DOT ID
    | Exp LB Exp RB
    | ID LP ArgList RP
    | ID
    | Literal
    | THIS
    ;
ArgList: ArgList COMMA Exp
    |
    ;
Literal: INT
    | FLOAT
    | BOOL
    | NULL_
    | STR
    | ArrayLit
    | MapLit
    | FuncLit
    ;
ArrayLit: LB ElemList RB
    ;
ElemList: ElemList COMMA Exp
    |
    ;
MapLit: LC MapElemList RC
    ;
MapElemList: MapElemList COMMA MapElem
    |
    ;
MapElem: ID COLON Exp
    | STR COLON Exp
    ;
FuncLit: FN LP ParamList RP LC StmtList RC
    | error RC
    ;

%%

void yyerror(const char* msg) {

}
