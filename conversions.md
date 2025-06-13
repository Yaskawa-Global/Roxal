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
| string    | ✓^1^ | ✓    | ✓    | ✓    | ✓       | –      | ✓    |
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


^1^ Non-zero numeric values are true. A non-empty string is true
^2^ Round toward zero if in range.  Combined with wrap/modulo if out of range.
^3^ Wrap/modulo
^4^ True is 1 and False is 0
^5^ "true" or "false"
^6^ The enum label


## Explicit (explicit cast/constructor used)

### Strict & Non-strict

All conversions as in the implicit non-strict table are allowed,
including those that will lose precision.
