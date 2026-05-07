%option yylineno

%{
    #include "syntax.tab.h"
%}

BLANK       [ \t\r\n]+
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
ASSIGN      =
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
INC         \+\+
DEC         \-\-
RELOP       ==|!=|<=|>=|<|>

ID          [a-zA-Z_][a-zA-Z0-9_]*

INT         0[bB][01](_?[01])*|0[oO][0-7](_?[0-7])*|0[xX][0-9a-fA-F](_?[0-9a-fA-f])*|[1-9](_?[0-9])*|0
FLOAT       [0-9]+(_?[0-9])*\.([0-9]+(_?[0-9])*)?([eE][+-]?[0-9]+(_?[0-9])*)?
STR         "(\\.|[^"\n\\])*"|'(\\.|[^'\n\\])*'
BOOL        true|false
NULL        null

VAR         var
LET         let
CONST       const
FUNC        fn
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
EXPORT      export
IMPORT      import

COMMMENT    (\/\/.*)|(\/\*([^*]|\*+[^\/])*\*\/)

%%

%%
