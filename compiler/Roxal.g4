/*
 * Roxal Grammar
 *
 * Copyright (c) 2021 Yaskawa Innovation Inc.
 *
 * Author : David Jung
 */


grammar Roxal;

tokens { INDENT, DEDENT }


/*
 * Parser rules
 */

file_input
 : annotation* import_stmt* ( NEWLINE | declaration )* EOF
 ;


single_input
 : NEWLINE
 | statement
 | compound_stmt NEWLINE
 ;


import_stmt
 : IMPORT IDENTIFIER (DOT IDENTIFIER)* ( (DOT STAR)? | (DOT '[' identifier_list ']')? ) NEWLINE
 ;

identifier_list
 : IDENTIFIER (COMMA IDENTIFIER)*
 ;


declaration
 : type_decl
 | func_decl
 | var_decl
 | statement
 ;

statement
 : expr_stmt until_clause? NEWLINE
 | compound_stmt
 ;

until_clause
 : UNTIL expression
 ;


expr_stmt
 : expression
 ;


compound_stmt
 : block_stmt
 | return_stmt
 | if_stmt
 | while_stmt
 | for_stmt
 | when_stmt
 | emit_stmt
 | raise_stmt
 | try_stmt
 | match_stmt
 | with_stmt
;

block_stmt
 : SCOPE ':' suite
 ;


return_stmt
 : RETURN expression?
 ;


if_stmt
 : IF expression ':' suite ( ELSEIF expression ':' suite )* ( ELSE ':' suite )?
 ;


while_stmt
 : WHILE expression ':' suite
 ;


for_stmt
 // the for can either assign a single lvalue identifier, or a list of them
 //  (as in a binding assignment - for example to iterate over a dict or a list of lists)
 : FOR (  '[' ident_opt_type (COMMA ident_opt_type)* ']'   // list of idents with optional types
        |     ident_opt_type (COMMA ident_opt_type)*       // can omit the []s for convenience
       )
   IN expression ':' suite
 ;
when_stmt
 : WHEN expression ((CHANGES | OCCURS) | (BECOMES expression)) (AS IDENTIFIER)? (WHERE expression)? ':' suite
 ;

emit_stmt
 : EMIT expression
 ;

raise_stmt
 : RAISE expression?
 ;

try_stmt
 : TRY ':' suite except_clause+ finally_clause?
 ;

except_clause
 : EXCEPT IDENTIFIER ':' expression ':' suite
 | EXCEPT IDENTIFIER ':' suite
 | EXCEPT ':' suite
 ;

finally_clause
 : FINALLY ':' suite
 ;


match_stmt
 : MATCH expression ':' NEWLINE INDENT match_case+ default_case? DEDENT
 ;

match_case
 : CASE case_pattern (',' case_pattern)* ':' suite
 ;

case_pattern
 : range
 ;

default_case
 : DEFAULT ':' suite
 ;

with_stmt
 : WITH expression ':' suite
 ;


var_decl // FIXME: use ident_opt_type
 : annotation* (VAR | CONST) IDENTIFIER (':' (builtin_type | IDENTIFIER))? (EQUALS expression)?
 ;

ident_opt_type
 : IDENTIFIER (':' builtin_type | IDENTIFIER)?
 ;

func_decl
 : annotation* function
 ;

function
 : func_sig ':'
   suite
 ;

func_sig
 : (FUNC | PROC) IDENTIFIER '(' parameters? ')' (YIELDS return_type)?
 ;

parameters
 : parameter (',' parameter)*
 ;

parameter
 : annotation* DOTDOT identifier_word (':' (builtin_type | IDENTIFIER) )?  // variadic ...rest param (no default allowed)
 | annotation* identifier_word (':' (builtin_type | IDENTIFIER) )? (EQUALS expression)?
 ;


suite
// : expr_stmt NEWLINE
 : NEWLINE INDENT (declaration NEWLINE?)+ DEDENT
 | NEWLINE INDENT UNDERSCORE NEWLINE DEDENT
 ;

type_decl
 : object_type_decl
 | enum_type_decl
 | event_type_decl
 ;

object_type_decl
 : annotation* TYPE IDENTIFIER (OBJECT | ACTOR | INTERFACE)
    (EXTENDS IDENTIFIER)? (IMPLEMENTS IDENTIFIER (',' IDENTIFIER)*)?
    (   (':' NEWLINE INDENT (str NEWLINE)? (member_var|method)* DEDENT)
      | NEWLINE
    )
 ;

enum_type_decl
 : annotation* TYPE IDENTIFIER ENUM
    // only enum can extend byte or int
    (EXTENDS (IDENTIFIER | BYTE | INT))?
    // for enums, allow mixture of comma & line separated labels
    (   (':' NEWLINE INDENT (enum_label (NEWLINE|COMMA) )* DEDENT)
      | (':' (enum_label COMMA)* enum_label NEWLINE)
      | NEWLINE
    )
 ;

event_type_decl
 : annotation* TYPE IDENTIFIER EVENT (EXTENDS IDENTIFIER)?
    (   (':' NEWLINE INDENT (str NEWLINE)? member_var* DEDENT)
      | NEWLINE
    )
 ;

method
 : annotation* PRIVATE? 
   func_sig
   ((':' suite) | NEWLINE)  // abstract methods have no body
 ;

member_var
 : annotation* PRIVATE? (VAR | CONST) IDENTIFIER (':' (builtin_type | IDENTIFIER))? (EQUALS expression)?
   ( NEWLINE
   | ':' NEWLINE INDENT (property_getter | property_setter)+ DEDENT
   )
 ;

property_getter
 : {_input->LT(1)->getText() == "get"}? IDENTIFIER ':' ( compound_stmt NEWLINE | suite )
 ;

property_setter
 : {_input->LT(1)->getText() == "set"}? IDENTIFIER ':' ( (compound_stmt | expr_stmt) NEWLINE | suite )
 ;

enum_label
 : IDENTIFIER (EQUALS expression)?
 ;


annotation
 : AT IDENTIFIER
   ( OPEN_PAREN NEWLINE* (annot_argument ( (COMMA | NEWLINE) NEWLINE* annot_argument )* COMMA? NEWLINE*)? CLOSE_PAREN )?
   NEWLINE
 ;

annot_argument
 : (IDENTIFIER '=')? expression
 ;


lambda_func
 : FUNC '(' parameters? ')' (YIELDS return_type)? ':' (expression | suite)
 ;

lambda_proc
 : PROC '(' parameters? ')' ':' (compound_stmt | expression | suite)
 ;


//TODO: assignment is an expression, but maybe we don't want assignments
// in places like if conditions?

expression
 : assignment ;

assignment
 : ( call DOT )? IDENTIFIER (EQUALS | COPYINTO) assignment
 | call (EQUALS | COPYINTO) assignment
 | logic_or
 ;


logic_or
 : logic_and ( OR logic_and )*
 ;

logic_and
 : bitwise_or ( AND bitwise_or )*
 ;

bitwise_or
 : bitwise_xor ( BIT_OR bitwise_xor )*
 ;

bitwise_xor
 : bitwise_and ( BIT_XOR bitwise_and )*
 ;

bitwise_and
 : equality ( BIT_AND equality )*
 ;

equality
 : comparison equalnotequal*
 ;

equalnotequal
 : ISEQUAL comparison
 | ISNOTEQUALS comparison
 | IS comparison
 ;

comparison
 : term ( ( GREATER_THAN | GT_EQ | LESS_THAN | LT_EQ ) term
        | (NOT)? IN term
        )?
 ;


term
 : factor plusminus*
 ;

plusminus
 : (PLUS | MINUS) factor
 ;


factor
 : unary multdiv*
 ;

multdiv
 : ( MULT | STAR ) unary
 | DIV unary
 | MOD unary
 ;


unary
 : ( NOT | MINUS | BIT_NOT ) unary
 | call
 ;

call
 : primary args_or_index_or_accessor*
 ;


args_or_index_or_accessor
 : '(' arguments? ')'
 | '[' ranges ']'
 | DOT (IDENTIFIER | WHEN | EMIT) ('(' arguments? ')')?
 ;

ranges
  : range ( ',' range )*
  ;

range
  : expression  // simple index (equivelent to n:n range)
  // [start:stop] or [start:] or [:stop] or either of those with optional :step] - half open (stop is exclusive)
  | optional_expression (DOTDOT '<'|COLON) optional_expression ((COLON|BY) expression)?
  // inclusive range (closed interval)
  | optional_expression DOTDOT optional_expression ((COLON|BY) expression)?
  ;

optional_expression
  : expression?
  ;


arguments
 : NEWLINE* argument ( (COMMA | NEWLINE) NEWLINE* argument )* COMMA? NEWLINE*
 ;

argument
 : (identifier_word '=')? expression
 ;


identifier_word
 : IDENTIFIER
 | FOR
 | WHEN
 ;


primary
 : LTRUE
 | LFALSE
 | num
 | LNIL
 | THIS
 | str   // str+ ?
 | RANGE '(' range ')'
 | list
 | vector
 | matrix
 | dict
 | IDENTIFIER
 | OPEN_PAREN expression CLOSE_PAREN
 | lambda_func
 | lambda_proc
 | SUPER '.' IDENTIFIER
 | builtin_type
 ;


return_type
 : type_spec                                                            // single type
 | '[' type_spec (',' type_spec)* ']'                                   // multiple types
 ;

type_spec
 : builtin_type
 | IDENTIFIER
 ;


builtin_type
 : LNIL
 | BOOL | BYTE | NUMBER | INT | REAL | DECIMAL
 | STRING | RANGE
| LIST | DICT
| VECTOR | MATRIX | SIGNAL | TENSOR
| ORIENT | EVENT
;


list
 : '[' NEWLINE* (expression ( (COMMA | NEWLINE) NEWLINE* expression )* COMMA? NEWLINE*)? ']'
 ;

vector
 : '[' signed_num signed_num (signed_num)* ']'
 ;

matrix
 : '[' row ((SEMI | NEWLINE) row)+ NEWLINE? ']'
;

row
 : signed_num (signed_num)*
;

signed_num
 : MINUS? num
 ;


dict
 : '{' NEWLINE* ((expression ':' expression) ( (COMMA | NEWLINE) NEWLINE* expression ':' expression )* COMMA? NEWLINE*)? '}'
 ;



/*
 * Lexer rules
 */

TYPE: 'type';
VAR : 'var';
CONST : 'const';
PRIVATE: 'private';
LET : 'let';
FUNC: 'func';
PROC: 'proc';
WHEN: 'when';
EMIT: 'emit';
RETURN: 'return';
SCOPE: 'scope' ;
WITH: 'with'; // TODO
IMPLEMENTS: 'implements';
EXTENDS: 'extends';
THIS: 'this';
SUPER: 'super';
IMPORT : 'import';
CHANGES: 'changes';
BECOMES: 'becomes';
OCCURS: 'occurs';
WHERE: 'where';


// Types
BOOL: 'bool';
BYTE: 'byte';
NUMBER: 'number';
INT: 'int';
REAL: 'real';
DECIMAL: 'decimal';  // dec?
STRING: 'string';
RANGE: 'range';
ENUM: 'enum';
LIST: 'list';
DICT: 'dict';
VECTOR: 'vector';
MATRIX: 'matrix';
SIGNAL: 'signal';
TENSOR: 'tensor';
ORIENT: 'orient';
EVENT: 'event';
OBJECT: 'object';
ACTOR : 'actor';
INTERFACE : 'interface' ;


// control
IF: 'if';
ELSE: 'else';
ELSEIF: 'elseif';
WHILE: 'while';
FOR : 'for';
IN : 'in';
BY : 'by';
TRY: 'try';
EXCEPT: 'except';
FINALLY: 'finally';
RAISE: 'raise';
AS: 'as';
UNTIL: 'until';
MATCH: 'match';
CASE: 'case';
DEFAULT: 'default';


NEWLINE : ( '\r'? '\n' | '\r' | '\f' ) SPACES?;


DOTDOT : '..' | '...' | '\u2026'; // …
DOT : '.';
STAR : '*';
COMMA : ',';
COLON : ':';
SEMI: ';';
PLUS : '+';
MINUS : '-';
MULT: '\u00D7'; // ×
DIV : '/';
MOD : '%';
AT: '@';
OR: 'or';
AND: 'and';
NOT: 'not';
IS: 'is';
LESS_THAN : '<';
GREATER_THAN : '>';
LT_EQ : '<=' | '\u2264' | '\u2A7D'; // ≤ ⩽
GT_EQ : '>=' | '\u2265' | '\u2A7E'; // ≥ ⩾
ISEQUAL: '==' | '\u225F'; // ≟
EQUALS: '=';
COPYINTO: '<-'| '\u2190' ; // ←
ISNOTEQUALS: '!=' | '<>' | '\u2260'; // ≠
YIELDS: '->' | '\u2192'; // →
UNDERSCORE: '_' ;
DASH: MINUS ;

BIT_AND: '&';
BIT_OR: '|';
BIT_XOR: '^';
BIT_NOT: '~';

OPEN_PAREN : '(';
CLOSE_PAREN : ')';
OPEN_BRACK : '[';
CLOSE_BRACK : ']';
OPEN_BRACE : '{';
CLOSE_BRACE : '}';


/*
 * Literals
 */

LTRUE : 'True'|'true'|'ON'|'On';
LFALSE : 'False'|'false'|'OFF'|'Off'|'off';
LNIL: 'nil';

 str
 : SINGLE_STRING
 | DOUBLE_STRING
 | TRIPLE_STRING
 ;


num
 : integer
 | FLOAT_NUMBER
 ;


integer
 : DECIMAL_INTEGER
 | OCT_INTEGER
 | HEX_INTEGER
 | BIN_INTEGER
 ;

DECIMAL_INTEGER
 : NON_ZERO_DIGIT DIGIT*
 | '0'+
 ;

OCT_INTEGER
 : '0' [oO] OCT_DIGIT+
 ;

HEX_INTEGER
 : '0' [xX] HEX_DIGIT+
 ;

BIN_INTEGER
 : '0' [bB] BIN_DIGIT+
 ;


FLOAT_NUMBER
 : POINT_FLOAT
 | EXPONENT_FLOAT
 ;

TRIPLE_STRING
 : '"""' ( . | '\r' | '\n' )*? '"""'
 ;

SINGLE_STRING
 : '\'' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f'] )* '\''
 ;

DOUBLE_STRING
 : '"' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f"] )* '"'
 ;


// this must be below keywords so they're not matched as identifiers
IDENTIFIER
 : ID_START ID_CONTINUE*
 ;

SKIP_
 : ( SPACES | COMMENT | LINE_JOINING ) -> channel(HIDDEN)
 ;


/*
 * Fragments
 */

// see https://www.unicode.org/reports/tr31/
fragment OTHER_ID_START
 : [\u2118\u212E\u309B\u309C]
 ;

// all characters in general categories Lu, Ll, Lt, Lm, Lo, Nl, the underscore, and characters with the Other_ID_Start property
fragment ID_START
 : '_'
 | [\p{Letter}\p{Letter_Number}]
 | OTHER_ID_START
 ;

// all characters in id_start, plus characters in the categories Mn, Mc, Nd, Pc and others with the Other_ID_Continue property
fragment ID_CONTINUE
 : ID_START
 | [\p{Nonspacing_Mark}\p{Spacing_Mark}\p{Decimal_Number}\p{Connector_Punctuation}\p{Format}]
 ;

fragment DIGIT
 : [0-9]
 ;

fragment NON_ZERO_DIGIT
 : [1-9]
 ;

fragment OCT_DIGIT
 : [0-7]
 ;

fragment HEX_DIGIT
 : [0-9a-fA-F]
 ;

fragment BIN_DIGIT
 : [01]
 ;

fragment POINT_FLOAT
 : INT_PART? FRACTION
 //| INT_PART '.'
 ;

fragment EXPONENT_FLOAT
 : ( INT_PART | POINT_FLOAT ) EXPONENT
 ;

 fragment INT_PART
 : DIGIT+
 ;

fragment FRACTION
 : '.' DIGIT+
 ;

 fragment EXPONENT
 : [eE] [+-]? DIGIT+
 ;


 fragment STRING_ESCAPE_SEQ
 : '\\' .
 ;

fragment SPACES
 : [ \t]+
 ;

 fragment COMMENT
 : '#' ~[\r\n\f]*
 | '//' ~[\r\n\f]*
 ;

fragment LINE_JOINING
 : '\\' SPACES? ( '\r'? '\n' | '\r' | '\f' )
 ;
