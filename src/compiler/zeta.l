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

    static Zeta::Parser::location_type mkLoc() {
        Zeta::Parser::location_type loc;
        loc.begin.line = yylineno;
        loc.begin.column = yycolumn - yyleng;
        return loc;
    }
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
RELOP       ==|!=|<=|>=|<|>|is

ID          [a-zA-Z_][a-zA-Z0-9_]*

INT         0[bB][01](_?[01])*|0[oO][0-7](_?[0-7])*|0[xX][0-9a-fA-F](_?[0-9a-fA-f])*|[1-9](_?[0-9])*|0
FLOAT       ([0-9]+(_?[0-9])*\.([0-9]+(_?[0-9])*)?|[0-9]+(_?[0-9])*[eE][+-]?[0-9]+(_?[0-9])*)([eE][+-]?[0-9]+(_?[0-9])*)?
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
SUPER       super

COMMMENT    (\/\/.*)|(\/\*([^*]|\*+[^\/])*\*\/)

%%
{NEWLINE}   { yycolumn = 1; }
{BLANK}     {}
{COMMMENT}  {}

{VAR}       { return Zeta::Parser::make_VAR(mkLoc()); }
{LET}       { return Zeta::Parser::make_LET(mkLoc()); }
{FN}        { return Zeta::Parser::make_FN(mkLoc()); }
{RETURN}    { return Zeta::Parser::make_RETURN(mkLoc()); }
{IF}        { return Zeta::Parser::make_IF(mkLoc()); }
{ELSE}      { return Zeta::Parser::make_ELSE(mkLoc()); }
{WHILE}     { return Zeta::Parser::make_WHILE(mkLoc()); }
{FOR}       { return Zeta::Parser::make_FOR(mkLoc()); }
{BREAK}     { return Zeta::Parser::make_BREAK(mkLoc()); }
{CONTINUE}  { return Zeta::Parser::make_CONTINUE(mkLoc()); }
{CLASS}     { return Zeta::Parser::make_CLASS(mkLoc()); }
{EXTENDS}   { return Zeta::Parser::make_EXTENDS(mkLoc()); }
{THIS}      { return Zeta::Parser::make_THIS(mkLoc()); }
{IMPORT}    { return Zeta::Parser::make_IMPORT(mkLoc()); }
{AS}        { return Zeta::Parser::make_AS(mkLoc()); }
{SUPER}     { return Zeta::Parser::make_SUPER(mkLoc()); }

{RELOP}     { return Zeta::Parser::make_RELOP(textToRelop(yytext), mkLoc()); }
{AND}       { return Zeta::Parser::make_AND(mkLoc()); }
{OR}        { return Zeta::Parser::make_OR(mkLoc()); }
{LSHIFT}    { return Zeta::Parser::make_LSHIFT(mkLoc()); }
{RSHIFT}    { return Zeta::Parser::make_RSHIFT(mkLoc()); }
{SEMI}      { return Zeta::Parser::make_SEMI(mkLoc()); }
{DOT}       { return Zeta::Parser::make_DOT(mkLoc()); }
{COMMA}     { return Zeta::Parser::make_COMMA(mkLoc()); }
{COLON}     { return Zeta::Parser::make_COLON(mkLoc()); }
{QMARK}     { return Zeta::Parser::make_QMARK(mkLoc()); }
{LP}        { return Zeta::Parser::make_LP(mkLoc()); }
{RP}        { return Zeta::Parser::make_RP(mkLoc()); }
{LB}        { return Zeta::Parser::make_LB(mkLoc()); }
{RB}        { return Zeta::Parser::make_RB(mkLoc()); }
{LC}        { return Zeta::Parser::make_LC(mkLoc()); }
{RC}        { return Zeta::Parser::make_RC(mkLoc()); }
{ASSIGN}    { return Zeta::Parser::make_ASSIGN(textToAssignOp(yytext), mkLoc()); }
{PLUS}      { return Zeta::Parser::make_PLUS(mkLoc()); }
{MINUS}     { return Zeta::Parser::make_MINUS(mkLoc()); }
{STAR}      { return Zeta::Parser::make_STAR(mkLoc()); }
{DIV}       { return Zeta::Parser::make_DIV(mkLoc()); }
{MOD}       { return Zeta::Parser::make_MOD(mkLoc()); }
{NOT}       { return Zeta::Parser::make_NOT(mkLoc()); }
{BITAND}    { return Zeta::Parser::make_BITAND(mkLoc()); }
{BITOR}     { return Zeta::Parser::make_BITOR(mkLoc()); }
{BITXOR}    { return Zeta::Parser::make_BITXOR(mkLoc()); }
{BITNOT}    { return Zeta::Parser::make_BITNOT(mkLoc()); }

{BOOL}      { return Zeta::Parser::make_BOOL(yytext[0] == 't', mkLoc()); }
{NULL_}     { return Zeta::Parser::make_NULL_(mkLoc()); }
{INT}       { return Zeta::Parser::make_INT(textToInt(yytext), mkLoc()); }
{FLOAT}     { return Zeta::Parser::make_FLOAT(textToFloat(yytext), mkLoc()); }
{STR}       { return Zeta::Parser::make_STR(textToStr(yytext), mkLoc()); }

{ID}        { return Zeta::Parser::make_ID(std::string(yytext), mkLoc()); }

<<EOF>>     { return Zeta::Parser::make_END(mkLoc()); }

.           { REPORT_LEXICAL_ERROR(yylineno, yycolumn, "Misterious character: '{}'", yytext); return Zeta::Parser::make_YYUNDEF(mkLoc()); }

%%

Relop textToRelop(const char* op) {
    if (strcmp(op, "==") == 0) return RP_EQ;
    if (strcmp(op, "!=") == 0) return RP_NEQ;
    if (strcmp(op, "<") == 0) return RP_LT;
    if (strcmp(op, ">") == 0) return RP_GT;
    if (strcmp(op, "<=") == 0) return RP_LEQ;
    if (strcmp(op, ">=") == 0) return RP_GEQ;
    if (strcmp(op, "is") == 0) return RP_IS;
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
                if (*p != '_') {
                    int digit;
                    if(*p >= '0' && *p <= '9') digit = *p - '0';
                    else if(*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
                    else if(*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
                    else assert(false && "Invalid hex digit");
                    value = (value << 4) | digit;
                }
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
