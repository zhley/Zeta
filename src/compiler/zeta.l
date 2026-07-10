%option yylineno
%option noyywrap

%{
    #include "syntax.tab.hpp"

    #include <assert.h>
    #include <string>
    #include <string.h>
    #include <stdint.h>

    Relop textToRelop(const char* op);
    int64_t textToInt(const char* text);
    double textToFloat(const char* text);
    std::string textToStr(const char* text);
    AssignOp textToAssignOp(const char* op);

    int yycolumn = 1;
    #define YY_USER_ACTION yycolumn += yyleng;

    #define YY_DECL Zeta::Parser::symbol_type yylex()
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
IMPORT      import
AS          as

COMMMENT    (\/\/.*)|(\/\*([^*]|\*+[^\/])*\*\/)

%%
{NEWLINE}   { yycolumn = 1; }
{BLANK}     {}
{COMMMENT}  {}

{VAR}       { return Zeta::Parser::make_VAR(); }
{LET}       { return Zeta::Parser::make_LET(); }
{FN}        { return Zeta::Parser::make_FN(); }
{RETURN}    { return Zeta::Parser::make_RETURN(); }
{IF}        { return Zeta::Parser::make_IF(); }
{ELSE}      { return Zeta::Parser::make_ELSE(); }
{WHILE}     { return Zeta::Parser::make_WHILE(); }
{FOR}       { return Zeta::Parser::make_FOR(); }
{BREAK}     { return Zeta::Parser::make_BREAK(); }
{CONTINUE}  { return Zeta::Parser::make_CONTINUE(); }
{CLASS}     { return Zeta::Parser::make_CLASS(); }
{EXTENDS}   { return Zeta::Parser::make_EXTENDS(); }
{THIS}      { return Zeta::Parser::make_THIS(); }
{IMPORT}    { return Zeta::Parser::make_IMPORT(); }
{AS}        { return Zeta::Parser::make_AS(); }

{RELOP}     { return Zeta::Parser::make_RELOP(textToRelop(yytext)); }
{AND}       { return Zeta::Parser::make_AND(); }
{OR}        { return Zeta::Parser::make_OR(); }
{LSHIFT}    { return Zeta::Parser::make_LSHIFT(); }
{RSHIFT}    { return Zeta::Parser::make_RSHIFT(); }
{SEMI}      { return Zeta::Parser::make_SEMI(); }
{DOT}       { return Zeta::Parser::make_DOT(); }
{COMMA}     { return Zeta::Parser::make_COMMA(); }
{COLON}     { return Zeta::Parser::make_COLON(); }
{QMARK}     { return Zeta::Parser::make_QMARK(); }
{LP}        { return Zeta::Parser::make_LP(); }
{RP}        { return Zeta::Parser::make_RP(); }
{LB}        { return Zeta::Parser::make_LB(); }
{RB}        { return Zeta::Parser::make_RB(); }
{LC}        { return Zeta::Parser::make_LC(); }
{RC}        { return Zeta::Parser::make_RC(); }
{ASSIGN}    { return Zeta::Parser::make_ASSIGN(textToAssignOp(yytext)); }
{PLUS}      { return Zeta::Parser::make_PLUS(); }
{MINUS}     { return Zeta::Parser::make_MINUS(); }
{STAR}      { return Zeta::Parser::make_STAR(); }
{DIV}       { return Zeta::Parser::make_DIV(); }
{MOD}       { return Zeta::Parser::make_MOD(); }
{NOT}       { return Zeta::Parser::make_NOT(); }
{BITAND}    { return Zeta::Parser::make_BITAND(); }
{BITOR}     { return Zeta::Parser::make_BITOR(); }
{BITXOR}    { return Zeta::Parser::make_BITXOR(); }
{BITNOT}    { return Zeta::Parser::make_BITNOT(); }

{BOOL}      { return Zeta::Parser::make_BOOL(yytext[0] == 't'); }
{NULL_}     { return Zeta::Parser::make_NULL_(); }
{INT}       { return Zeta::Parser::make_INT(textToInt(yytext)); }
{FLOAT}     { return Zeta::Parser::make_FLOAT(textToFloat(yytext)); }
{STR}       { return Zeta::Parser::make_STR(textToStr(yytext)); }

{ID}        { return Zeta::Parser::make_ID(std::string(yytext)); }

.           { REPORT_LEXICAL_ERROR(yylineno, yycolumn, "Misterious character: '{}'", yytext); return Zeta::Parser::make_YYUNDEF(); }

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

std::string textToStr(const char* text) {
    size_t len = strlen(text);
    std::string str;
    if (len >= 2) {
        str.reserve(len - 2);
    }
    for (size_t i = 1; i + 1 < len; ++i) {
        if (text[i] == '\\') {
            ++i;
            switch (text[i]) {
                case 'n': str.push_back('\n'); break;
                case 't': str.push_back('\t'); break;
                case 'r': str.push_back('\r'); break;
                case '\\': str.push_back('\\'); break;
                case '\'': str.push_back('\''); break;
                case '\"': str.push_back('\"'); break;
                default: str.push_back('\\'); str.push_back(text[i]); break; // TODO: 未知转移, 至少应该给个警告
            }
        } else {
            str.push_back(text[i]);
        }
    }
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
