# Roxal for Software Developers

A quick overview of the Roxal language for existing software developers.

## Syntax

The superficial syntax is similar to Python: blocks ar indicated using indentation.  A statement introducing a new block ends with a colon.

```roxal
  if somehing:
    print('something is true')

  print('always print')
```

Like Python/Java/Ruby and other languages, there is a distinction between by-value builtin types and by-reference types.  The builtin types are:

Value types:

  * `nil`  - non-reference (aka null, nullptr)
  * `bool` - boolean (true, false)
  * `byte` - numeric 0..255
  * `int`  - signed 32bit
  * `real` - IEEE 64bit float (aka C double)
  * `decimal` - (unimplemented) fixed point (designed for no roundoff error for fractions in base 10)
  * `enum` - enumerated int labelled (similar to C)

Reference types:
  * `string` - Unicode (UTF-8) (literals are interned)
    * Single quoted `'like this'` or double quoted `"like this"`
    * With double quotes, {variable} interpolations are substituted (`"myalue={myvalue}"`)
  * `list` - [list, of, values] - heterogeneous
  * `dict` - {key:value,key2:value2} - heterogeneous (hash, map)
    * insertion order preserved
  * `vector` - [number number number] - arbitrary 1-dim real elements
  * `matrix` - [num num num; num num num] - arbitrary 2-dim real elements (can use newline between rows in literals)
  * `object` - user-defined object type (aka class)
  * `actor` - user-defined actor type (similar to object type)

Other internal & advanced types:
  * `range`, `event`, `signal`, `exception`, `function`, `closure`, `future`, `type`

### Type conversions

There are two sets of rules for what type conversions are automatic - strict and non-strict.  By default, module scope (e.g. file level) is non-strict and most values will be automatically converted into other required types for convenience.

Some non-strict automatic conversions: (see conversions.md for details):
  * bool → numeric (except enum)
  * numeric → differently sized numeric, bool (0 is false)
  * string → numeric, enum (its label)
  * most values → string
  * object instance → dict (of public member variables)

Function body scope is strict by default.  To convert types in strict context, casting/constructor syntax is required. e.g. `byte(5)`, `string(6)`.  Most automatic convenience conversions available in non-strict context can be used with explicit construction in strict context.
(strict vs non-strict can be controlled via annotations)

## Variables

Variables are declared with `var`:

```roxal
var i :int  // variable i is an int (can't reassign to another type)
var b :byte = 5 // b has initial value 5
var s = "hello" // the type is optional, s can be reassigned to another type later
l = [1,2,3]     // var is optional in non-strict (e.g. file/module context)
```

## Functions and Procedures

Functions are declared with the `func` keyword.  A procedure (`proc`) is a function that has no return value.

```roxal
func sq(x):
  return x*x

proc show(s):
  print(s)

print(sq(2)) // 4
show('hello world') // output "hello world"
```

Advanced: Functions are 'first class' values, hence can be assigned, passed as parameters to other functions etc.  (object methods can also be used as functions and automatically bind the receiver instance)

```
func f(x,y):
  return x+y

g = f
print(g(1,2)) // 3

value = 7

func h():
  func i(): // local func (like local var)
    return value  // captures value var in closure
  return i  // return a function

j = h() // j is function i
value = 8
print(j()) // 8
```

#### Lambdas (Lambda functions)

A Lambda is just an anonymous (unnamed function), declared as a single expression (no `return` keyword):
```
var f = func(a): a+1

func call_and_print(f,a):
  print(f(a))

call_and_print(f,2) // print 3

call_and_print(func (a): a+1, 5) // print 6
```

### Parameters & Calls

Function parameters can optionally have their type explicitly supplied.  Similarly for the return type. They can also have default values (like C++).  Calls can mix positional and named arguments (positional first)

```
func f(a :int, b :real = 3) -> string:
  return "a is {a} and b is {b}"

print( f(1,2) ) // "a is 1 and b is 2"
print( f(1) )   // "a is 1 and b is 3"
print( f(b=7, a=2) ) // // "a is 2 and b is 7"
```

## Operators

## Control Statements



## Objects & Actors


## Modules

Similar to Python, a roxal file is a module.







### Module search paths

Roxal resolves `import` statements by searching a list of module paths. The
paths can be supplied explicitly via the `ROXALPATH` environment variable or the
`--module-paths` command line option. Multiple paths may be provided by
repeating the option or using the platform-specific separator (`:` on POSIX,
`;` on Windows) in `ROXALPATH`.

When executing a script file, the directory containing that script is always
added implicitly as the first search path.
