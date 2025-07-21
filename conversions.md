# Roxal Builtin Type Conversions

## Implicit (no explicit cast/constructor)

### Non-strict (e.g. module scope)

| From↓  To→| bool | byte | int  | real | decimal | string | enum |
|-----------|------|------|------|------|---------|--------|------|
| bool      | –    | ✓^4^ | ✓^4^ | ✓^4^ | ✓^4^    | ✓^5^   | No   |
| byte      | ✓^1^ | –    | ✓    | ✓    | ✓       | ✓      | No   |
| int       | ✓^1^ | ✓^3^ | –    | ✓    | ✓       | ✓      | No   |
| real      | ✓^1^ | ✓^2^ | ✓^2^ | –    | ✓       | ✓      | No   |
| decimal   | ✓^1^ | ✓^2^ | ✓^2^ | ✓    | –       | ✓      | No   |
| string    | ✓^1^ | ✓^7^ | ✓^7^ | ✓^7^ | ✓^7^    | –      | ✓    |
| enum      | No   | No   | ✓    | No   | No      | ✓^6^   | –    |

### Strict (e.g. function scope)

| From↓  To→| bool | byte | int  | real | decimal | string | enum |
|-----------|------|------|------|------|---------|--------|------|
| bool      | –    | ✓^4^ | ✓^4^ | ✓^4^ | ✓^4^    | ✓^5^   | No   |
| byte      | No   | –    | ✓    | ✓    | ✓       | ✓      | No   |
| int       | No   | No   | –    | ✓    | ✓       | ✓      | No   |
| real      | No   | No   | No   | –    | No      | ✓      | No   |
| decimal   | No   | No   | No   | No   | –       | ✓      | No   |
| string    | No   | No   | No   | No   | No      | –      | No   |
| enum      | No   | No   | ✓    | No   | No      | ✓^6^   | –    |


^1^ Non-zero numeric values are true. The empty string is false, non-empty strings are true.
^2^ Round toward zero if in range.  Combined with wrap/modulo if out of range.
^3^ Wrap/modulo
^4^ True is 1 and False is 0
^5^ "true" or "false"
^6^ The enum label
^7^ Strings converting to numeric types parse the same formats as numeric literals (prefixes like 0x for hex). In non-strict mode invalid strings yield zero; strict mode disallows such conversions.
^8^ Converting an exception value to `string` returns only the exception's message text.


## Explicit (explicit cast/constructor used)

### Strict & Non-strict

All conversions as in the implicit non-strict table are allowed,
including those that will lose precision.


## Operators

### Primitives

Plus (+), Minus (-), Multiply (*), Divide (/) and Modulo (%)

Note: "numeric" is shorthand for byte, int, real or decimal

| LHS     | RHS     | Result  |
| ------- | ------- | ------- |
| bool    | bool    | bool    |
| bool    | byte    | int     |
| bool    | int     | int     |
| bool    | real    | real    |
| bool    | decimal | decimal |
| byte    | bool    | int     |
| byte    | byte    | int     |
| byte    | int     | int     |
| byte    | real    | real    |
| byte    | decimal | decimal |
| int     | bool    | int     |
| int     | byte    | int     |
| int     | int     | int     |
| int     | real    | real    |
| int     | decimal | decimal |
| real    | bool    | real    |
| real    | byte    | real    |
| real    | int     | real    |
| real    | real    | real    |
| real    | decimal | decimal |
| decimal | bool    | decimal |
| decimal | byte    | decimal |
| decimal | int     | decimal |
| decimal | real    | decimal |
| decimal | decimal | decimal |
| string  | bool    | string  |  (+ string concatenation - RHS -> string)
| string  | byte    | string  |  (+ string concatenation)
| string  | int     | string  |  (+ string concatenation)
| string  | real    | string  |  (+ string concatenation)
| string  | decimal | string  |  (+ string concatenation)
| string  | string  | string  |  (+ string concatenation)
| X       | string  | No      |  (error)
| vector  | vector  | vector  |  (dims must match)
| vector  | numeric | vector  |  (* and / only - scalar multiply or divide)
| numeric | vector  | vector  |  (* and / only - scalar multiply or divide)

Note1: for Modulo (%): Values are promoted to int first.


TODO: Clarify when signals and futures are resolved vs 'sampled'.  Currently inconsistient:

  * Futures & signals are resolved/sampled in builting functions like print(), but not for passing to user-defined functions
    * Passing a signal to a user-defined function actually creates a new dataflow node
    * Perhaps been some explicit .value property of signals (futures?)
  * Builtin operators like +, -, etc. aren't consistnent with user defined functions
    * Operator + on futures will resolve futures & add them instead of creating a new future that is the future sum
    * Operator + on signals will sample the signals and add them instead of creating a new node in the dataflow network who's output is the sum signal
    * Indexing a vector or list signal - though that create an index node who's output is a signal of the element type?

