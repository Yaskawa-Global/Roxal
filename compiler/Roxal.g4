/*
 * Roxal Grammar
 *
 * Copyright (c) 2021 Yaskawa Innovation Inc.
 *
 * Author : David Jung
 */


grammar Roxal;

tokens { INDENT, DEDENT }

@parser::members {
  // File-level vs attached annotations: a leading annotation run is
  // FILE-LEVEL only when a blank line separates it from what follows;
  // an annotation glued to the next line binds to that import/declaration.
  // The indentation lexer suppresses NEWLINE tokens on blank lines, so the
  // separation cannot be seen in the token stream -- detect it by comparing
  // source line numbers instead. Evaluated per loop iteration: in
  // "@a <blank> @b decl", @a is file-level and @b binds to the declaration.
  bool leadingAnnotationIsFileLevel() {
    size_t i = 1;
    int lastLine = -1;
    while (_input->LT(i)->getType() == AT) {
      i++; // AT
      if (_input->LT(i)->getType() != IDENTIFIER)
        return false; // malformed; let normal parsing report it
      i++; // IDENTIFIER
      if (_input->LT(i)->getType() == OPEN_PAREN) {
        int depth = 0;
        do {
          auto t = _input->LT(i)->getType();
          if (t == antlr4::Token::EOF)
            return true;
          if (t == OPEN_PAREN) depth++;
          else if (t == CLOSE_PAREN) depth--;
          i++;
        } while (depth > 0);
      }
      if (_input->LT(i)->getType() != NEWLINE)
        return false;
      // Measure the gap from the annotation's last CONTENT token: when a
      // blank line follows, the lexer suppresses the annotation line's own
      // newline and the emitted NEWLINE carries the blank line's number,
      // so the NEWLINE token's line cannot reveal the gap.
      lastLine = static_cast<int>(_input->LT(i - 1)->getLine());
      i++; // NEWLINE
      auto next = _input->LT(i);
      if (next->getType() == antlr4::Token::EOF)
        return true; // nothing follows: treat as file-level
      if (static_cast<int>(next->getLine()) > lastLine + 1)
        return true; // blank line terminates the file-level group
      if (next->getType() != AT)
        return false; // glued to a non-annotation: binds to it
      // glued to another annotation: keep walking the run
    }
    return false;
  }
}

/*
 * Parser rules
 */

file_input
 : ({leadingAnnotationIsFileLevel()}? annotation)* (NEWLINE* import_stmt)* ( NEWLINE | declaration )* EOF
 ;


single_input
 : NEWLINE
 | statement
 | compound_stmt NEWLINE
 ;


import_stmt
 : annotation* IMPORT IDENTIFIER (DOT IDENTIFIER)* ( (DOT STAR)? | (DOT '[' identifier_list ']')? ) NEWLINE
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
 : expr_stmt (if_clause | until_clause)? NEWLINE
 | compound_stmt
 ;

until_clause
 : UNTIL expression
 ;

if_clause
 : IF expression
 ;

at_clause
 : {_input->LT(1)->getText() == "at"}? IDENTIFIER expression
 ;

expr_stmt
 : expression at_clause?
 ;


compound_stmt
 : block_stmt
 | return_stmt
 | break_stmt
 | continue_stmt
 | jump_stmt
 | label_stmt
 | if_stmt
 | while_stmt
 | for_stmt
 | when_stmt
 | emit_stmt
 | raise_stmt
 | assert_stmt
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


break_stmt
 : BREAK if_clause?
 ;


continue_stmt
 : CONTINUE if_clause?
 ;


// 'jump' is a hard keyword (unused as an identifier); the optional if_clause mirrors
// 'break if' / 'continue if' (INFORM's JUMP *LABEL IF idiom).
jump_stmt
 : JUMP IDENTIFIER if_clause?
 ;


// 'label' is a soft keyword (used widely as an identifier) — guarded by a semantic
// predicate exactly like at_clause, so 'label' stays a valid variable name elsewhere.
label_stmt
 : {_input->LT(1)->getText() == "label"}? IDENTIFIER IDENTIFIER
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


// 'assert' is a hard keyword.  Two spellings are accepted: the Python-style
// `assert cond, msg` and the C-style `assert(cond, msg)` -- the latter needs its
// own alternative because a parenthesised expression cannot contain the comma.
assert_stmt
 : ASSERT ( OPEN_PAREN expression COMMA expression CLOSE_PAREN
          | expression (COMMA expression)? )
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
 : annotation* (VAR | CONST) IDENTIFIER (':' const_qualifier? (builtin_type | type_name))? (EQUALS expression at_clause?)?
 | annotation* (VAR | CONST) '[' var_target (',' var_target)* ']' EQUALS expression  // declaring destructure
 ;

var_target
 : IDENTIFIER (':' const_qualifier? (builtin_type | type_name))?
 ;

ident_opt_type
 : IDENTIFIER (':' (builtin_type | type_name))?
 ;

func_decl
 : annotation* function
 ;

function
 : func_sig ':'
   suite
 ;

func_sig
 : (FUNC | PROC) (IDENTIFIER | operator_name) '(' parameters? ')' (YIELDS return_type)?
 ;

operator_name
 : (OPERATOR | LOPERATOR | ROPERATOR) operator_symbol
 | OPERATOR conversion_target
 ;

conversion_target
 : builtin_type
 | type_name
 ;

operator_symbol
 : PLUS | MINUS | STAR | MULT | DIV | REM
 | ISEQUAL | ISNOTEQUALS
 | LESS_THAN | GREATER_THAN | LT_EQ | GT_EQ
 ;

parameters
 : NEWLINE* parameter ( (COMMA | NEWLINE) NEWLINE* parameter )* COMMA? NEWLINE*
 ;

parameter
 : annotation* DOTDOT identifier_word (':' const_qualifier? (builtin_type | type_name) )?  // variadic ...rest param (no default allowed)
 | annotation* identifier_word (':' const_qualifier? (builtin_type | type_name) )? (EQUALS expression)?
 | STAR  // sole-param sugar for `proc init(*)` — synthesizes one param per public property
 ;

const_qualifier
 : CONST
 | MUTABLE
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
    (EXTENDS type_name)? (IMPLEMENTS type_name (',' type_name)*)?
    (   (':' NEWLINE INDENT (str NEWLINE)? (member_var|method|nested_type_decl)* DEDENT)
      | NEWLINE
    )
 ;

nested_type_decl
 : PRIVATE? type_decl
 ;

enum_type_decl
 : annotation* TYPE IDENTIFIER ENUM
    // only enum can extend byte or int
    (EXTENDS (type_name | BYTE | INT))?
    // for enums, allow mixture of comma & line separated labels
    (   (':' NEWLINE INDENT (enum_label (NEWLINE|COMMA) )* DEDENT)
      | (':' (enum_label COMMA)* enum_label NEWLINE)
      | NEWLINE
    )
 ;

event_type_decl
 : annotation* TYPE IDENTIFIER EVENT (EXTENDS type_name)?
    (   (':' NEWLINE INDENT (str NEWLINE)? member_var* DEDENT)
      | NEWLINE
    )
 ;

method
 : annotation* PRIVATE? implicit_kw? stmt_action_kw?
   func_sig
   ((':' suite) | NEWLINE)  // abstract methods have no body
 ;

implicit_kw
 : {_input->LT(1)->getText() == "implicit"}? IDENTIFIER
 ;

// Two-word soft keyword: 'statement action'.  Recognised only as a
// method modifier; ordinary identifiers named 'statement' or 'action'
// elsewhere are unaffected.
stmt_action_kw
 : {_input->LT(1)->getText() == "statement" && _input->LT(2)->getText() == "action"}?
   IDENTIFIER IDENTIFIER
 ;

member_var
 : annotation* PRIVATE? (VAR | CONST) IDENTIFIER (':' const_qualifier? (builtin_type | type_name))? (EQUALS expression)?
   ( NEWLINE
   | ':' NEWLINE INDENT (property_getter | property_setter)+ DEDENT
   )
 ;

property_getter
 : {_input->LT(1)->getText() == "get"}? IDENTIFIER
   ( ':' ( compound_stmt NEWLINE | suite )
   | NEWLINE
   )
 ;

property_setter
 : {_input->LT(1)->getText() == "set"}? IDENTIFIER
   ( ':' ( (compound_stmt | expr_stmt) NEWLINE | suite )
   | NEWLINE
   )
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

// Entry rule for the contents of a "{...}" string-interpolation placeholder.
// Never reached from file_input -- ASTGenerator drives it directly on the
// placeholder text extracted from a string token.  Deliberately rooted at
// logic_or rather than expression, so assignment (and its at_clause) is
// structurally impossible inside a placeholder rather than needing a check.
// The explicit EOF is what makes ANTLR report trailing junk: without it,
// "{a b}" would match just `a` and return with no syntax error.
interp_expr
 : logic_or EOF ;

// Fragment entry rules for the inspect module (ASTGenerator drives them
// directly; never reached from file_input).  The explicit EOF makes trailing
// junk a syntax error rather than a silently truncated parse.
fragment_expr
 : expression EOF ;

fragment_stmt
 : NEWLINE* statement NEWLINE* EOF ;

fragment_decl
 : NEWLINE* declaration NEWLINE* EOF ;

assignment
 : ( call DOT )? IDENTIFIER (EQUALS | COPYINTO) assignment at_clause?
 | call (EQUALS | COPYINTO) assignment at_clause?
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
 | REM unary
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
 | DOT (IDENTIFIER | WHEN | EMIT | MATCH) ('(' arguments? ')')?
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
 | vector
 | matrix
 | list
 | dict
 | IDENTIFIER
 | OPEN_PAREN expression CLOSE_PAREN
 | lambda_func
 | lambda_proc
 | SUPER '.' IDENTIFIER
 | builtin_type
 ;


return_type
 : const_qualifier? type_spec                                            // single type
 | '[' const_qualifier? type_spec (',' const_qualifier? type_spec)* ']'  // multiple types
 ;

type_spec
 : builtin_type
 | type_name
 ;

type_name
 : IDENTIFIER (DOT IDENTIFIER)*
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
 : '[' vec_elem vec_elem (vec_elem)* ']'
 ;

matrix
 : '[' row ((SEMI | NEWLINE) row)+ NEWLINE? ']'
;

row
 : vec_elem (vec_elem)*
;

vec_elem
 : signed_num
 | '(' expression ')'
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
MUTABLE : 'mutable';
PRIVATE: 'private';
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
ASSERT: 'assert';
AS: 'as';
UNTIL: 'until';
MATCH: 'match';
CASE: 'case';
DEFAULT: 'default';
BREAK: 'break';
CONTINUE: 'continue';
JUMP: 'jump';
// NOTE: 'label' is intentionally NOT a token — it is a soft keyword recognised by a
// semantic predicate in label_stmt, so it remains usable as an ordinary identifier.


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
REM : 'rem';
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

LTRUE : 'true';
LFALSE : 'false';
LNIL: 'nil';

 str
 : SINGLE_STRING
 | DOUBLE_STRING
 | TRIPLE_STRING
 | SUFFIXED_SINGLE_STRING
 | SUFFIXED_DOUBLE_STRING
 ;


num
 : integer
 | FLOAT_NUMBER
 | SUFFIXED_FLOAT
 | SUFFIXED_DECIMAL_INTEGER
 ;


integer
 : DECIMAL_INTEGER
 | OCT_INTEGER
 | HEX_INTEGER
 | BIN_INTEGER
 ;

// Suffix forms: bare (alpha, then alpha/digit/·/²³¹⁻/^//), braced {m/s}, and a
// standalone '%' for percent literals. '%' is only ever a suffix — there is no
// modulo operator (use 'rem').
//
// ORDERING: the tokens below must precede the SUFFIXED_* rules. Each can match
// the same text — '0x51' is hex or '0'+suffix 'x51'; '1e3' is a float or
// '1'+suffix 'e3' — and ANTLR breaks an equal-length tie by declaration order,
// so declared later they all lexed as suffixed literals and failed. Ordering
// only decides EXACT ties, so real suffixed literals ('1.5m', '1e3m') are
// unaffected: they are strictly longer, and longest-match still wins.
// A suffix that would be shadowed this way is rejected at registration — see
// suffixShadowedByNumericBase() in RoxalCompiler.cpp.
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

SUFFIXED_FLOAT
 : ( POINT_FLOAT | EXPONENT_FLOAT ) ( BRACED_SUFFIX | BARE_SUFFIX | '%' )
 ;

SUFFIXED_DECIMAL_INTEGER
 : ( NON_ZERO_DIGIT DIGIT* | '0'+ ) ( BRACED_SUFFIX | BARE_SUFFIX | '%' )
 ;

SUFFIXED_SINGLE_STRING
 : '\'' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f'] )* '\'' ( BRACED_SUFFIX | BARE_SUFFIX )
 ;

SUFFIXED_DOUBLE_STRING
 : '"' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f"] )* '"' ( BRACED_SUFFIX | BARE_SUFFIX )
 ;


DECIMAL_INTEGER
 : NON_ZERO_DIGIT DIGIT*
 | '0'+
 ;

// OCT_INTEGER / HEX_INTEGER / BIN_INTEGER / FLOAT_NUMBER are declared above the
// SUFFIXED_* rules, to win the equal-length tie — see the note there.

TRIPLE_STRING
 : '"""' ( . | '\r' | '\n' )*? '"""'
 ;

SINGLE_STRING
 : '\'' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f'] )* '\''
 ;

// NOTE: a '{' placeholder cannot contain a double-quoted string, because this
// token ends at the first inner '"'.  Adding a recursive INTERP_HOLE fragment
// here does lift that restriction, but at an unacceptable price: longest-match
// then merges  ("a {" + "}")  into a SINGLE token whose hole is '" + "', which
// parses successfully and silently prints 'a  + ' instead of concatenating two
// strings.  Today that same source is a clean "unterminated string
// placeholder" error.  Use single quotes inside a placeholder ({d['k']}), or a
// triple-quoted string, whose token already admits inner double quotes and so
// supports nesting for free.
DOUBLE_STRING
 : '"' ( STRING_ESCAPE_SEQ | ~[\\\r\n\f"] )* '"'
 ;


OPERATOR: 'operator';
LOPERATOR: 'loperator';
ROPERATOR: 'roperator';

// this must be below keywords so they're not matched as identifiers
IDENTIFIER
 : BACKTICK_IDENTIFIER
 | ID_START ID_CONTINUE*
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

fragment BACKTICK_IDENTIFIER
 : '`' BACKTICK_IDENTIFIER_BODY '`'
 ;

fragment BACKTICK_IDENTIFIER_BODY
 : ID_START ID_CONTINUE*
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

// Literal suffix fragments
// Bare suffix: starts with a letter, continues with letters, digits, and select
// unit-notation characters. The ASTGenerator validates further constraints
// (max 8 chars, at most one '/', no consecutive '··', etc.)
fragment SUFFIX_START
 : [\p{Letter}]
 ;

fragment SUFFIX_CONTINUE
 : [\p{Letter}\p{Decimal_Number}]
 | '\u00B7'                          // · middle dot (unit multiplication, e.g. N·m)
 | [\u00B2\u00B3\u00B9]              // ² ³ ¹ superscript digits
 | '\u207B'                          // ⁻ superscript minus
 | '^'
 | '/'
 ;

fragment BARE_SUFFIX
 : SUFFIX_START SUFFIX_CONTINUE*
 ;

// Braced suffix: {contents} for expert use, allows longer/complex suffixes.
// Restricted to letters, digits, unit-notation chars, and spaces.
// Disallows quotes, braces, parens, brackets, backslash, control chars.
fragment BRACED_SUFFIX
 : '{' BRACED_SUFFIX_CHAR+ '}'
 ;

fragment BRACED_SUFFIX_CHAR
 : [\p{Letter}\p{Decimal_Number}]
 | '\u00B7'                          // · middle dot
 | [\u00B2\u00B3\u00B9]              // ² ³ ¹
 | '\u207B'                          // ⁻
 | '^' | '/' | ' ' | '_' | '-'
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
