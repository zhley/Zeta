%option yylineno
%option noyywrap

%{
    #include "syntax.tab.h"

    #include <assert.h>
    #include <string.h>
    #include <stdint.h>

    Relop textToRelop(const char* op);
    int64_t textToInt(const char* text);
    double textToFloat(const char* text);
    char* textToStr(const char* text);
    AssignOp textToAssignOp(const char* op);

    int yycolumn = 1;
    #define YY_USER_ACTION yycolumn += yyleng;
%}

NEWLINE     \n
BLANK       [ \t\r]+
SEMI        ;
DOT         \.
COMMA       ,
COLON       :
QMARK       \?
LP          \(
RP          \)
LB          \[
RB          \]
LC          \{
RC          \}
ASSIGN      =|\+=|-=|\*=|\/=|%=|&=|\|=|\^=|<<=|>>=
PLUS        \+
MINUS       -
STAR        \*
DIV         \/
MOD         \%
AND         &&
OR          \|\|
NOT         \!
BITAND      &
BITOR       \|
BITXOR      \^
BITNOT      ~
LSHIFT      <<
RSHIFT      >>
RELOP       ==|!=|<=|>=|<|>

ID          [a-zA-Z_][a-zA-Z0-9_]*

INT         0[bB][01](_?[01])*|0[oO][0-7](_?[0-7])*|0[xX][0-9a-fA-F](_?[0-9a-fA-f])*|[1-9](_?[0-9])*|0
FLOAT       [0-9]+(_?[0-9])*\.([0-9]+(_?[0-9])*)?([eE][+-]?[0-9]+(_?[0-9])*)?
STR         \"(\\.|[^\"\n\\])*\"|\'(\\.|[^\'\n\\])*\'
BOOL        true|false
NULL_       null

VAR         var
LET         let
FN          fn
RETURN      return
IF          if
ELSE        else
WHILE       while
FOR         for
BREAK       break
CONTINUE    continue
CLASS       class
EXTENDS     extends
THIS        this
STATIC      static
IMPORT      import
AS          as

COMMMENT    (\/\/.*)|(\/\*([^*]|\*+[^\/])*\*\/)

%%
{NEWLINE}   { yycolumn = 1; }
{BLANK}     {}
{COMMMENT}  {}

{VAR}       { return VAR; }
{LET}       { return LET; }
{FN}        { return FN; }
{RETURN}    { return RETURN; }
{IF}        { return IF; }
{ELSE}      { return ELSE; }
{WHILE}     { return WHILE; }
{FOR}       { return FOR; }
{BREAK}     { return BREAK; }
{CONTINUE}  { return CONTINUE; }
{CLASS}     { return CLASS; }
{EXTENDS}   { return EXTENDS; }
{THIS}      { return THIS; }
{STATIC}    { return STATIC; }
{IMPORT}    { return IMPORT; }
{AS}        { return AS; }

{RELOP}     { yylval.int_ = textToRelop(yytext); return RELOP; }
{AND}       { return AND; }
{OR}        { return OR; }
{LSHIFT}    { return LSHIFT; }
{RSHIFT}    { return RSHIFT; }
{SEMI}      { return SEMI; }
{DOT}       { return DOT; }
{COMMA}     { return COMMA; }
{COLON}     { return COLON; }
{QMARK}     { return QMARK; }
{LP}        { return LP; }
{RP}        { return RP; }
{LB}        { return LB; }
{RB}        { return RB; }
{LC}        { return LC; }
{RC}        { return RC; }
{ASSIGN}    { yylval.int_ = textToAssignOp(yytext); return ASSIGN; }
{PLUS}      { return PLUS; }
{MINUS}     { return MINUS; }
{STAR}      { return STAR; }
{DIV}       { return DIV; }
{MOD}       { return MOD; }
{NOT}       { return NOT; }
{BITAND}    { return BITAND; }
{BITOR}     { return BITOR; }
{BITXOR}    { return BITXOR; }
{BITNOT}    { return BITNOT; }

{BOOL}      { yylval.int_ = yytext[0] == 't'; return BOOL; }
{NULL_}     { return NULL_; }
{INT}       { yylval.int_ = textToInt(yytext); return INT; }
{FLOAT}     { yylval.double_ = textToFloat(yytext); return FLOAT; }
{STR}       { yylval.str = textToStr(yytext); return STR; }

{ID}        { yylval.str = strdup(yytext); return ID; }

.           { REPORT_LEXICAL_ERROR(yylineno, yycolumn, "Misterious character: %s", yytext); }

%%

Relop textToRelop(const char* op) {
    if (strcmp(op, "==") == 0) return RP_EQ;
    if (strcmp(op, "!=") == 0) return RP_NEQ;
    if (strcmp(op, "<") == 0) return RP_LT;
    if (strcmp(op, ">") == 0) return RP_GT;
    if (strcmp(op, "<=") == 0) return RP_LEQ;
    if (strcmp(op, ">=") == 0) return RP_GEQ;
    assert(false && "Invalid relational operator");
    return RP_EQ;
}

int64_t textToInt(const char* text) {
    int64_t value = 0;
    if (text[0] == '0') {
        if (text[1] == 'b' || text[1] == 'B') {
            for (const char* p = text + 2; *p; ++p) {
                if (*p != '_') value = (value << 1) | (*p - '0');
            }
        } else if (text[1] == 'o' || text[1] == 'O') {
            for (const char* p = text + 2; *p; ++p) {
                if (*p != '_') value = (value << 3) | (*p - '0');
            }
        } else if (text[1] == 'x' || text[1] == 'X') {
            for (const char* p = text + 2; *p; ++p) {
                if (*p != '_') value = (value << 4) | ((*p >= 'a' && *p <= 'f') ? (*p - 'a' + 10) : (*p - '0'));
            }
        } else {
            for (const char* p = text + 1; *p; ++p) {
                if (*p != '_') value = value * 10 + (*p - '0');
            }
        }
    } else {
        for (const char* p = text; *p; ++p) {
            if (*p != '_') value = value * 10 + (*p - '0');
        }
    }
    return value;
}

double textToFloat(const char* text) {
    char* normal = (char*)malloc(strlen(text) + 1);
    char* p = normal;
    for (const char* q = text; *q; ++q) {
        if (*q != '_') {
            *p = *q;
            ++p;
        }
    }
    *p = '\0';
    double result = strtod(normal, NULL);
    free(normal);
    return result;
}

char* textToStr(const char* text) {
    size_t len = strlen(text);
    char* str = (char*)malloc(len - 1);
    char* p = str;
    for (size_t i = 1; i < len - 1; ++i) {
        if (text[i] == '\\') {
            ++i;
            switch (text[i]) {
                case 'n': *p++ = '\n'; break;
                case 't': *p++ = '\t'; break;
                case 'r': *p++ = '\r'; break;
                case '\\': *p++ = '\\'; break;
                case '\'': *p++ = '\''; break;
                case '\"': *p++ = '\"'; break;
                default: *p++ = '\\'; *p++ = text[i]; break; // TODO: 未知转移, 至少应该给个警告
            }
        } else {
            *p++ = text[i];
        }
    }
    *p = '\0';
    return str;
}

AssignOp textToAssignOp(const char* op) {
    if (strcmp(op, "=") == 0) return AS_ASSIGN;
    if (strcmp(op, "+=") == 0) return AS_PLUS;
    if (strcmp(op, "-=") == 0) return AS_MINUS;
    if (strcmp(op, "*=") == 0) return AS_STAR;
    if (strcmp(op, "/=") == 0) return AS_DIV;
    if (strcmp(op, "%=") == 0) return AS_MOD;
    if (strcmp(op, "&=") == 0) return AS_BITAND;
    if (strcmp(op, "|=") == 0) return AS_BITOR;
    if (strcmp(op, "^=") == 0) return AS_BITXOR;
    if (strcmp(op, "<<=") == 0) return AS_LSHIFT;
    if (strcmp(op, ">>=") == 0) return AS_RSHIFT;
    assert(false && "Invalid assignment operator");
    return AS_ASSIGN;
}
