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
 : ( NEWLINE | declaration )* EOF
 ;


single_input
 : NEWLINE
 | statement
 | compound_stmt NEWLINE
 ;


declaration
 : type_decl
 | func_decl
 | var_decl
 | statement
 ;

statement
 : expr_stmt NEWLINE
 | compound_stmt
 ;


expr_stmt
 : expression  
 ;


compound_stmt
 : block_stmt
 | print_stmt
 | return_stmt
 | if_stmt 
 | while_stmt 
//  | for_stmt 
//  | with_stmt 
//  | procdef 
// /// | classdef 
 ;

block_stmt
 : SCOPE ':' suite 
 ;


print_stmt
 : PRINT OPEN_PAREN expression CLOSE_PAREN
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


var_decl
 : VAR IDENTIFIER (EQUALS expression)?
 ;

func_decl
 : function
 ;

function
 : (FUNC | PROC) IDENTIFIER '(' parameters? ')' ':' 
   suite
 ;

parameters
 : parameter (',' parameter)* 
 ;

parameter
 : IDENTIFIER (':' (builtin_type | IDENTIFIER) )?
 ;


suite 
// : expr_stmt NEWLINE
 : NEWLINE INDENT (declaration NEWLINE?)+ DEDENT
 ;

type_decl
 : TYPE (OBJECT | ACTOR) IDENTIFIER 
    (EXTENDS IDENTIFIER)? (IMPLEMENTS IDENTIFIER (',' IDENTIFIER)*)? ':' NEWLINE
   INDENT function* DEDENT
 ;




//TODO: assignment is an expression, but maybe we don't want assignments
// in places like if conditions?

expression
 : assignment ;

assignment
 : ( call DOT )? IDENTIFIER EQUALS assignment
 | logic_or 
 ;

logic_or
 : logic_and ( OR logic_and )* 
 ;

logic_and
 : equality ( AND equality )* 
 ;

equality
 : comparison equalnotequal* 
 ;

equalnotequal
 : ISEQUAL comparison
 | ISNOTEQUALS comparison
 ;

comparison
 : term ( ( GREATER_THAN | GT_EQ | LESS_THAN | LT_EQ ) term )? 
 ;

// NB: we don't need MINUS as 1-1 is (factor factor(negate 1))
//     (and hence the PLUS is optional so we don't need to write 1+-1)
term
 : factor (PLUS? factor)* 
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
 : ( NOT | MINUS ) unary | call | index
 ;

call
 : primary args_or_accessor* 
 ;

// accessor or args for callable
args_or_accessor
  : '(' arguments? ')' 
  | DOT IDENTIFIER ('(' arguments? ')')?
  ;
 
// args for indexing
index
  : primary '[' arguments ']'
  ;


arguments 
 : expression ( ',' expression )* 
 ;


primary
 : LTRUE 
 | LFALSE
 | num 
 | LNIL
 | THIS
 | str   // str+ ?
 | IDENTIFIER 
 | OPEN_PAREN expression CLOSE_PAREN
 | SUPER '.' IDENTIFIER 
 ;


builtin_type
 : LNIL 
 | BOOL | BYTE | NUMBER | INT | REAL | DECIMAL
 | STRING 
 | LIST | DICT 
 | VECTOR | MATRIX | TENSOR
 | ORIENT | STREAM
 ;




/*
 * Lexer rules
 */

TYPE: 'type';
VAR : 'var';
LET : 'let';
FUNC: 'func';
PROC: 'proc';
RETURN: 'return';
SCOPE: 'scope' ;
WITH: 'with'; // TODO
IMPLEMENTS: 'implements';
EXTENDS: 'extends';
THIS: 'this';
SUPER: 'super';


// Types
BOOL: 'bool';
BYTE: 'byte';
NUMBER: 'number'; 
INT: 'int';
REAL: 'real';
DECIMAL: 'decimal';  // dec?
STRING: 'string';
LIST: 'list';
DICT: 'dict';
VECTOR: 'vector';
MATRIX: 'matrix';
TENSOR: 'tensor';
ORIENT: 'orient';
STREAM: 'stream';
OBJECT: 'object';
ACTOR : 'actor';


// control
IF: 'if';
ELSE: 'else';
ELSEIF: 'elseif';
WHILE: 'while';
FOR : 'for';
IN : 'in';

PRINT : 'print';


NEWLINE : ( '\r'? '\n' | '\r' | '\f' ) SPACES?;


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
OR: 'or';
AND: 'and';
NOT: 'not';
LESS_THAN : '<';
GREATER_THAN : '>';
LT_EQ : '<=' | '\u2264' | '\u2A7D'; // ≤ ⩽
GT_EQ : '>=' | '\u2265' | '\u2A7E'; // ≥ ⩾
ISEQUAL: '==' | '\u225F'; // ≟
EQUALS: '=';
ISNOTEQUALS: '!=' | '<>' | '\u2260'; // ≠


OPEN_PAREN : '(';
CLOSE_PAREN : ')';
OPEN_BRACK : '[';
CLOSE_BRACK : ']';
OPEN_BRACE : '{';
CLOSE_BRACE : '}';


/*
 * Literals
 */

LTRUE : 'True'|'true'|'ON'|'On'|'on';
LFALSE : 'False'|'false'|'OFF'|'Off'|'off';
LNIL: 'nil';

str
 : STRING_LITERAL
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

 STRING_LITERAL
  : SHORT_STRING
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
 | INT_PART '.'
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

 fragment SHORT_STRING
 : '\'' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f'] )* '\''
 | '"' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f"] )* '"'
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
