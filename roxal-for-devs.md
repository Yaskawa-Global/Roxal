# Roxal for Software Developers

A quick overview of the Roxal language for existing software developers.

## Syntax

The superficial syntax is similar to Python: blocks are indicated using indentation.  A statement introducing a new block ends with a colon.

<!-- markdown does not have syntax coloring for roxal, but php gives reasonable coloring and also uses // and # for comments -->

```php
  if somehing:
    print('something is true')  // C++-style comment

  print('always print')         # Python-style comment
```

Like Python/Java/Ruby and other languages, there is a distinction between by-value builtin types and by-reference types.  The builtin types are:

Value types:

  * `nil`  - non-reference (aka null, nullptr)
  * `bool` - boolean (true, false)
  * `byte` - numeric 0..255
  * `int`  - signed 64bit
  * `real` - IEEE 64bit float (aka C double)
  * `decimal` - (unimplemented) fixed point (designed for no roundoff error for fractions in base 10)
  * `enum` - enumerated int labelled (similar to C)
  * `vector` - [number number number] - arbitrary n dim real scalar elements
  * `matrix` - [num num num; num num num] - arbitrary n x m dim real scalar elements (can use newline between rows in literals)
  * `tensor` - multi-dimensional array with arbitrary shape (see tensor section below)

Reference types:
  * `string` - Unicode (UTF-8) (literals are interned)
    * Single quoted `'like this'` or double quoted `"like this"`
    * With double quotes, `{}` placeholders interpolate identifiers, dotted properties, and indexes using identifiers, numeric literals, or single-quoted string literals. Multiple comma-separated indices are allowed (for example `"lookup={record['name']}"` or `"matrix element={mat[row, 2]}"`).
  * `list` - [list, of, values] - heterogeneous
  * `dict` - {key:value,key2:value2} - heterogeneous (hash, map)
    * insertion order preserved
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

```php
var i :int  // variable i is an int (can't reassign to another type)
var b :byte = 5 // b has initial value 5
var s = "hello" // the type is optional, s can be reassigned to another type later
l = [1,2,3]     // var is optional in non-strict (e.g. file/module context)
```

## Functions and Procedures

Functions are declared with the `func` keyword.  A procedure (`proc`) is a function that has no return value.

```php
func sq(x):
  return x*x

proc show(s):
  print(s)

print(sq(2)) // 4
show('hello world') // output "hello world"
```

Advanced: Functions are 'first class' values, hence can be assigned, passed as parameters to other functions etc.  (object methods can also be used as functions and automatically bind the receiver instance)

```php
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

A Lambda is just an anonymous (unnamed) function, declared as a single expression (no `return` keyword):
```php
var f = func(a): a+1

func call_and_print(f,a):
  print(f(a))

call_and_print(f,2) // print 3

call_and_print(func (a): a+1, 5) // print 6
```

### Assignments

Like most programming languages, `=` is used for assignment (unlike in mathematics, where is means equality).
This works as you'd expect for value types.  For reference types, the references are usually being assigned.

```php
i = 1 // int is a value type
j = i // j now has value 1
i = 2
print(j) // j is still 1

l = [1,2,3] // list is a reference type
l2 = l      // l2 is now a reference to the same list as l
l[0] = 11   // change first element of the one list
print(l2)   // [11,2,3]
```

It is possible to assign multiple variables at once with a *binding assignment*.  This is convenient for returning multiple values from a function.

```php
func f(): // return an int, a string and a dict
  return [1, 'a', {'a':'A'}]

[i, s, d] = f()
print(i)  // 1
print(s)  // 'a'
print(d)  // {'a':'A'}
```

Advanced: The _copy into_ operation (`←` or `<-`) makes a shallow copy of the RHS and copies it into the LHS (- hence, the LHS must be compatible - e.g. the same type)

```php
l = [1,2,3]
l2 = []
l2 <- l   // copy elements from l into l2
print(l2) // [1,2,3]
l[0] = 11 // change l
print(l2) // [1,2,3]  (still same, list l content was changed, not l2's)
```

### Parameters & Calls

Function parameters can optionally have their type explicitly supplied.  Similarly for the return type. They can also have default values (like C++).  Calls can mix positional and named arguments (positional first)

```php
func f(a :int, b :real = 3) -> string:
  return "a is {a} and b is {b}"

print( f(1,2) ) // "a is 1 and b is 2"
print( f(1) )   // "a is 1 and b is 3"
print( f(b=7, a=2) ) // // "a is 2 and b is 7"
```

### Variadic Parameters

Functions can accept a variable number of arguments using the `...` prefix on the last parameter. The variadic arguments are collected into a list:

```php
func sum(...nums):
    var total = 0
    for n in nums:
        total = total + n
    return total

print( sum(1, 2, 3) )  // 6
print( sum() )         // 0 (empty list)
```

Variadic parameters can be combined with regular and default parameters. When a function has a variadic parameter, named arguments can appear before the positional variadic arguments:

```php
func format(sep = ", ", ...items):
    var result = ""
    var first = true
    for item in items:
        if not first:
            result = result + sep
        result = result + string(item)
        first = false
    return result

print( format("x", "y", "z") )           // "x, y, z" (sep uses default)
print( format(sep=" | ", "a", "b", "c") ) // "a | b | c"
```


## Operators

The operators +, -, \*, / and % work how you'd expect on builtin numeric types.  Vectors and Matrices also support +, - and \*, performing matrix multiplication, vector*matrix multiplication and dot products (two vectors).

```php
m = [1 2
     3 4]
v = [1 2]
print(m*v) // [5 11]
```

In addition, + can also be used for:
  * string concatenation (when the left-hand-side is a string) - "hello "+"world".  This also directly converts most types into strings: "hello "+5 → "hello 5"
  * list concatenation: [1,2,3]+[4,5,6] → [1,2,3,4,5,6]

Boolean operators `and`, `or` and `not` work on the bool type.  `(true and true and not false)` → `true`

Bitwise operators `|` (or), `&` (and), `~` (not) and `^` (xor) work with bool, byte and int types.
In addition, `|` for dict will merge two dicts into one (with precedence for the RHS keys) and `&` for dict will yield a dict with the intersection (common) keys (with values from the LHS in case of a common key in both)

```php
print({'a':1,'b':2} | {'b':3,'c':4}) // {"a": 1, "b": 3, "c": 4} - keys from LHS or RHS
print({'a':1,'b':2} & {'b':3,'c':4}) /  {"b": 2} - keys in LHS and RHS
```

The equality operators `≟` (is equal to), `≠` (not equal to), `<` / `>` (less / greater than), `≤` / `≥` (less / greater than or equal to) function as expected bool, byte, int, decimal, range, vector & matrix.  Note that the `==` and `!=` or `<>` familiar from C are also available.

```php
if 5 ≥ 4:
  print('always')
v = [1 2]         // vector
print(v == [1 2]) // true
```

However, for most reference types, like user-defined objects & actors (more below), equality only compares the reference.

The `is` operator:
  * Checks identity - when the operands are two (non-type) values, it compares them for being the same object (e.g. list & dict)
  * Checks type - when the LHS is a type

```php
l = [1,2,3]
l2 = [1,2,3]
print(l is l2)   // false, same content, different lists
print(l is list) // true, it is a list
l3 = l2
print(l3 is l2)  // true, the two variables l3 & l2 reference the same list
print(1 is 1)    // true, same as == for value types
```

The `in` operator checks for membership/containment:
  * For lists: checks if an element is in the list
  * For dicts: checks if a key exists in the dict
  * For strings: checks if a substring is present
  * For ranges: checks if a value is within the range (respecting step)

```php
print(2 in [1, 2, 3])           // true
print('key' in {'key': 'val'})  // true
print('ell' in 'hello')         // true (substring)
print(5 in range(1..10))        // true
print(5 in range(0..10 by 2))   // false (5 not on step boundary)
print(4 not in [1, 2, 3])       // true (not in)
```


## Indexing

Indexing uses the [] notation.  It works as expected for lists, dicts (by key), strings, vectors and matrices.
In addition, there is a builtin `range` type, that can be used for slicing lists.

There are two notations for ranges:
  * using `..` - range(_start_.._end_), range(_start_..<_one_past_end_) and also with an optional stride range(_start_.._end_ by _stride_)
    * without the `<` the _end_ is inclusive, with `<` it is excluded
  * or using `:` - range(_start_:_end_) - exclusive, or with a stride range(_start_:_end_:_stride_).

In each case, the _start_ or _end_ can be omitted to indicate 'from 0' or 'to the end'.  Negative values count from the end of the value being indexed rather from the beginning.  A negative stride jumps backward.  Omitting both - range(\:) is synonymous with 'everything'.

To see what is included in a range, you can construct a list from it (when definite):

```php
print(list( range(1..5) ))   // [1, 2, 3, 4, 5]
print(list( range(1..<5) )) // [1, 2, 3, 4]
l = [1,2,3]
print( l[range(::-1)] ) // [3, 2, 1]  (indexing everything, in reverse)
print( len( range(0..<10) )) // use len() to count elements: 10
for i in range(1..10 by 2):
  print(i) // 1 3 5 ...
```

Indexing with ranges:
```php
l = list(range(0..<10)) // make a list [0,1,2,...,9]
print( l[0] )         // 0
print( l[1..2] )      // [1,2] - notice don't need range() here
print( l[1..8 by 2] ) // [1,3,5,7]
print( l[5::-1] )     // [5, 4, 3, 2, 1, 0]
s = 'Hello world'
print( s[::-1] )      // "dlrow olleH" ('by -1' reverses)
print( s[:-1] )       // "Hello worl" (all but last)
m = [1 2 3            // 2x3 matrix
     4 5 6]
print( m[0..1,0..1] ) // [1 2; 4 5] submatrix
```

## Vectors, Matrices & Tensors

Roxal provides three types for numerical arrays: `vector` (1D), `matrix` (2D), and `tensor` (arbitrary dimensions).

### Vector and Matrix Literals

```php
var v = [1.0 2.0 3.0]      // 3-element vector (space-separated)
var m = [1 2 3; 4 5 6]     // 2x3 matrix (semicolon separates rows)
var m2 = [1 2              // can also use newlines between rows
          3 4]
```

### Tensor Creation

Tensors are created using the `tensor()` constructor:

```php
var t1 = tensor(10)                           // 1D tensor with 10 elements (zeros)
var t2 = tensor(2, 3, 4)                      // 3D tensor with shape [2, 3, 4]
var t3 = tensor(3, data=[1.0, 2.0, 3.0])      // 1D tensor with initial data
var t4 = tensor(2, 3, dtype='float32')        // specify data type
```

Supported `dtype` values: `'float16'`, `'float32'`, `'float64'` (default), `'int8'`, `'int16'`, `'int32'`, `'int64'`, `'uint8'`, `'bool'`

### Tensor Indexing and Properties

```php
var t = tensor(2, 3, data=[1,2,3,4,5,6])
print(t[0, 1])       // element at row 0, col 1
t[1, 2] = 99         // assign element
print(t.shape())     // [2, 3]
print(t.rank())      // 2
print(len(t))        // 6 (total elements)
print(t.dtype())     // 'float64'
```

### Value Semantics

Unlike `list` and `dict`, the mathematical types `vector`, `matrix`, and `tensor` have *value semantics*. Assignment creates an independent copy, matching mathematical intuition:

```php
var v = [1.0 2.0 3.0]
var v2 = v           // v2 is an independent copy
v2[0] = 99
print(v[0])          // 1 (original unchanged)
print(v2[0])         // 99

var t = tensor(3, data=[1.0, 2.0, 3.0])
var t2 = t           // t2 is an independent copy
t2[0] = 99
print(t[0])          // 1 (original unchanged)
```

### Arithmetic Operations

Element-wise arithmetic works with +, -, *, / for same-shaped tensors. Vectors and matrices also support matrix multiplication:

```php
var v1 = [1.0 2.0 3.0]
var v2 = [4.0 5.0 6.0]
print(v1 + v2)       // [5 7 9]
print(v1 * v2)       // dot product: 32

var t1 = tensor(3, data=[1.0, 2.0, 3.0])
var t2 = tensor(3, data=[1.0, 1.0, 1.0])
print(t1 + t2)       // element-wise addition
```

## Control Statements

Common control statements, `if`, `for`, `while`.  For can iterate over ranges, lists and dicts.

```php
if true:
  for i in range(..<10):
    print(i)

for e in [-1,-2,-3,-4]:
  print(e)

i = 10
while i > 10:
  print(i)
  i = i - 1

for [k :int,l] in [[1,2],[3,4]]:
  print("k={k} l={l}") // k=1 l=2 ; k=3 l=4

for k in d: // keys of dict d
  print(k)

for [k,v] in d:  // keys and values of dict d
  print("k={k} v={v})
```

### Match Statement

The `match` statement provides pattern matching similar to Python's match or C's switch. It works with any type and supports multiple patterns per case, range matching for numeric types, and a default case.

```php
// Simple value matching
func get_name(n :int) -> str:
  match n:
    case 1:
      return "one"
    case 2:
      return "two"
    case 3:
      return "three"
    default:
      return "other"

print(get_name(2))  // "two"
print(get_name(5))  // "other"

// Multiple patterns per case
func classify(n :int) -> str:
  match n:
    case 1, 2, 3:
      return "small"
    case 10, 20, 30:
      return "large"
    default:
      return "medium"

print(classify(2))   // "small"
print(classify(15))  // "medium"

// Range matching with naked ranges (like indexing)
func age_group(age :int) -> str:
  match age:
    case 0..12:        // closed range (0 to 12 inclusive)
      return "child"
    case 13..19:       // 13 to 19 inclusive
      return "teen"
    case 20..64:       // 20 to 64 inclusive
      return "adult"
    case 65..:         // 65 and above (open-ended)
      return "senior"
    default:
      return "unknown"

print(age_group(5))   // "child"
print(age_group(15))  // "teen"
print(age_group(70))  // "senior"

// String matching
func handle_command(cmd :str) -> str:
  match cmd:
    case "start", "begin":
      return "starting"
    case "stop", "end":
      return "stopping"
    default:
      return "unknown command"

print(handle_command("start"))  // "starting"
print(handle_command("end"))    // "stopping"
```

Match cases are evaluated in order. The first matching case executes and then control exits the match statement. Ranges in case patterns use the same syntax as indexing: `..` for closed ranges, `..<` for half-open ranges, `:` for Python-style slicing. Start or end can be omitted for open-ended ranges.


### With Statement

The `with` statement brings enum labels or object/actor members into scope, allowing you to reference them without prefixing.

```php
// With enum types - brings enum labels into scope
type Color enum:
  Red
  Green
  Blue

with Color:
  var c1 = Red      // instead of Color.Red
  var c2 = Green    // instead of Color.Green
  var c3 = Blue     // instead of Color.Blue
  print(c1)

// Combining with and match for cleaner code
with Color:
  var picked = Green
  match picked:
    case Red:
      print("Red picked")
    case Green:
      print("Green picked")
    case Blue:
      print("Blue picked")

// With object/actor instances - brings members into scope
type Point object:
  var x :int
  var y :int

var p = Point(x=10, y=20)
with p:
  print(x)  // instead of p.x
  print(y)  // instead of p.y
  x = 15    // instead of p.x = 15
```

**Important notes:**
- For **enums**, use the type name: `with EnumType:`
- For **objects/actors**, use an instance: `with instance:`
- The with statement creates a new scope
- Names from the with context are resolved before module-level names
- Currently requires compile-time known types (Phase 1 implementation)


## Modules

Similar to Python, a roxal file (`.rox`) is a module.  The variables declared at the 'top level' of the file are considered 'module scoped' variables.
You can import one module into another via the `import` statement.

`mymain.rox`:
```php
import mymodule

mymodule.showVersion() // prints Version 1.0
```

`mymodule.rox`: (same folder)
```php
func showVersion():
  print('Version 1.0')
```

Notice that accessing the symbol names from the imported module required prefixing them with the module name separated by period(s).
If you want to import all of the module's names into the current module scope, you can use `import mymodule.*'

```php
import math.*
print(cos(0.0)) // didn't need to write math.cos
```

You can nest modules using folders in the filesystem: e.g. to import `mymodule/submodule/toplevel.rox`

```php
import mymodule.submodule.toplevel
```

(the folder containing the `mymodule/` folder must be the module paths - see below)

If you need to have several .rox files in your module, you can place them in a folder containing a specially named `init.rox` file, and the import will execute that file as the module's file (the module name will be the folder name).  This file could, for example, import other files from that folder to help implement the module.

### Module search paths

Roxal resolves `import` statements by searching a list of module paths. The
paths can be supplied explicitly via the `ROXALPATH` environment variable or the
`-p` command line option. Multiple paths may be provided by repeating the option or using the platform-specific
separator (`:` on POSIX, `;` on Windows) in `ROXALPATH`.

When executing a script file, the directory containing that script is always
added implicitly as the first search path.


## Objects

While understanding Object-Oriented-Programming (OOP) may not be necessary for robotics application programmers, Roxal has a familiar set of OOP features.

An object type (aka class) can be declared and instantiated thus:

```php
type MyObjType object:

  var a :int  // member variable (sometimes called a property)
  private var b :int = 3

  proc init():  // init is the constructor (can have params)
    print('constructed')

  func double_a_by_b():
    return 2*a*b  // no need to use this.a (unlike self. in Python)


// instantiate an instance:

myobj = MyObjType()
myobj.a = 2    // a is public (b it not)
print( myobj.double_a_by_b() ) // method call - prints 12
```

Inheritance uses the `extends` keyword:

```php
type ChildObjType object extends MyObjType:

  var c :real

child = ChildObjType()
child.a = 2
child.c = 1.5
d = child.double_a_by_b()
print(child is MyObjType)        // true
print(ChildObjType is MyObjType) // true
```

(`interface` is not fully implemented, but it'll be possible to inherit from multiple interfaces and one object type)

### Property Accessors

Member variables can have custom getter and/or setter methods by using the accessor syntax with `var` or `const`:

```php
type Widget object:

  // Property with both getter and setter
  var width :int = 100:
    get:
      print("Getting width")
      return _width
    set:
      print("Setting width to {value}")
      _width = value

  // Read-only property (const with getter only)
  const height :int = 50:
    get:
      return _height

  // Write-only property (setter only)
  var depth :int = 25:
    set:
      _depth = value

  // Computed property
  var area :int :
    get:
      return _width * _height

  proc init():
    _

var w = Widget()
w.width = 200      // Calls the setter
print(w.width)     // Calls the getter
print(w.height)    // Calls the getter (read-only)
w.depth = 30       // Calls the setter (write-only)
```

**Key points:**
- A private member var `_<name>` is automatically created
- The `value` parameter is available in setters
- For `const` properties, the backing field is also marked as const (and the getter should only return a constant expression)


## Actors

Actors are similar to objects, with a key difference - each actor instance has its *own associated execution thread*.  That is the only thread that executes the actor's methods.

This is an ideal way to achieve concurrency, because actors don't share any state with other actors (or the main program thread) - so there is no need to think about reentrancy or locking.

The syntax for declaring an actor is similar to an object, except it cannot have any non-private member variables.  You can think of calling a method on an actor instance as being more like sending it a message to execute that method.

Note that the caller is not blocked when calling an actor method - instead the call returns immediately with a 'future' for the return value (sometimes called a promise).  This behaves just like the actual return value, but won't be useable until the actor method completes and provides the value.  An attempt to use the return value future before it is ready will block the using thead (- though you can pass futures to other functions, store them etc.).  A future value is always implicitly convertible to the value it is promising.

```php
type Worker actor:

  private var amount :real = 1.0

  proc addto(r :real):
    amount = amount + r
    wait(ms=200)  // 'sleep' for a bit

  func currentAmount() -> real:
    wait(ms=300)
    return amount

w = Worker()
w.addto(2.0) // doesn't block

amt = w.currentAmount()  // also doesn't block (amt is a 'future real')

// constructing a real() from a 'future real' always resolves it to the real value
//  (hence, this will block for ~300ms until the Worker currentAmount() method completes
//    - i.e. until that future promise has been fulfilled)
print( real(amt) ) // 3
```

(Note: the VM doesn't currently prohibit accessing/mutating any module scope vars from an actor, but in future it will prohibit mutating all module scope variables and prohibit access to non-const or reference module variable.  So, for now, only 'read' value-type module variables that are not modified elsewhere (i.e. logical 'constants'))

## Exceptions

```php
try:
  dostuff()
except e :RuntimeException:
  print("Something exceptional happened: "+e)
finally:
  print("That's all")
```


## Events

Events are useful for constructing responsive programs.  For robotics, this may be to respond to I/O, sensor data or other internal states of the program.

Event *types* are declared similarly to object and actor types.  An event type describes the payload that will be attached to each occurrence of the event:

```php
type ButtonPressed event:
  var buttonId :int

when ButtonPressed occurs as evt:
  print('button '+evt.buttonId+' pressed')

emit ButtonPressed(buttonId=7)   // creates a fresh event instance and delivers it
```

Each `emit` call constructs a new event instance.  Instances are immutable snapshots: the handler receives the occurrence as `evt` above and can read the payload members.  Event types can inherit from other event types to reuse payload fields:

```php
type DeviceEvent event:
  var deviceId :int

type LowBattery event extends DeviceEvent:
  var percentRemaining :real

when LowBattery occurs as evt:
  print('device '+evt.deviceId+' low ('+evt.percentRemaining+'%)')

emit LowBattery(deviceId=42, percentRemaining=12.5)
```

Event types also expose `.when` and `.remove` helpers that mirror the statement form:

```php
var subscription = LowBattery.when(func (evt): print(evt.percentRemaining))
LowBattery.emit(deviceId=1, percentRemaining=9.0)
LowBattery.remove(subscription)
```

### Event Target Filtering

All events have a built-in `target` property that can be used to filter which handlers receive an event.  This is useful for UI frameworks where many widgets may listen for the same event type (e.g., `Clicked`), but each widget only wants to handle events targeted at itself.

Use the `where` clause to filter events by target:

```php
type Clicked event:
  var x :int
  var y :int

var widget1 = "Widget1"
var widget2 = "Widget2"

// Handler only receives events where target == widget1
when Clicked occurs as evt where evt.target == widget1:
  print("Widget1 clicked at " + evt.x + "," + evt.y)

// Handler only receives events where target == widget2
when Clicked occurs as evt where evt.target == widget2:
  print("Widget2 clicked at " + evt.x + "," + evt.y)

// Handler without 'where' receives ALL events (targeted and untargeted)
when Clicked occurs as evt:
  print("Global handler: " + evt.target)

// Emit with a target
emit Clicked(target=widget1, x=10, y=20)  // only widget1 and global handlers fire

// Emit without a target
emit Clicked(x=50, y=60)  // only global handler fires (target is nil)
```

**Notes:**
- The `where` clause currently only supports the pattern `evt.target == <value>`
- The `<value>` is evaluated when the handler is registered
- Handlers without a `where` clause receive all events, including those with no target
- Events emitted without a `target=` argument have `target` set to `nil`

## Signals

Signals in roxal represent values that can (spontaneously) change.  For example, for robotics, they might represent an external input.  Signals can be transformed, using functions (func) into new signals.  To create your own source signals you "call" the builtin `signal` type (e.g. `signal(freq, initial)`), while `clock(freq)` provides a signal that counts up automatically.  A signal's value can be any of the usual roxal value types, but most usefully bool, byte, int, real, vector or matrix.

Conceptually, you can think of signals as like wires in circuit, connected to various 'func' processing nodes that have input (parameter) and output (return) signals.

### Examples

A single clock signal:
```php

c = clock(freq=10)  // an int signal that counts up from 0 at 10Hz (initially stopped)

// register a signal change handler (fires when the value changes)
when c changes as evt:
  print('tick='+evt.value)

c.run()    // start the clock counting
wait(s=1)  // keep the script running so we can see ~10 prints
```

Use `changes` to run a handler on any update. Supplying `as evt` binds the automatically-generated event instance that contains the sampled signal value (`evt.value`), a steady-clock duration since the engine started (`evt.timestamp`, a `sys.TimeSpan`), and the signal's own tick count (`evt.tick`). To fire only when a signal hits a specific value, use the `becomes` form:
```php
when c becomes 42:
  print('answer arrived')

when gripperOpen becomes true: // trigger on 'rising edge' only
  print('gripper now open')
```

Transforming a some signals:
```php
initial_v = [1.0 2.0 3.0]  // vector

// source signals need to be explicitly updated with their set() method
s  = signal(freq=10,initial_v)  // source signal at 10Hz with initial vector value
s2 = signal(freq=10,initial_v)  //  (these won't change value unless set() is called)

dp = s*s2    // vector dot product (real scalar signal)

when dp changes as evt: // print if dp changes
  print('dp='+evt.value)

// set the signals in motion
s.run()
s2.run()

// change them
s.set([1.0 3.0 3.0])  // handler above will print dp=16 on next 10Hz boundary
wait(ms=100)
s2.set([2.0 1.0 0.5]) // handler above will print dp=6.5 on next 10Hz boundary

wait(ms=500)  // don't exit until after next tick & handler runs
print('done')
```

## Until

The `until` modifier can be used as a suffix for statements to specify a condition for when the statement execution should be stopped (interrupted).

The until condition can be an event or a boolean valued signal.

```php

var e :event

func take_a_while():
  wait(s=10)  // do nothing for 10s

type MyWorker actor:
  proc triggerEventAfterDelay(e :event, aftersecs :int):
    wait(s=aftersecs)
    emit e()

worker = MyWorker()
worker.triggerEventAfterDelay(e,5) // async (immediate return)

// this will execute take_a_while for ~5s until interrupted by e triggered by worker
take_a_while() until e


c = clock(freq=10)  # 10Hz clock signal
c.run()

// this will execute take_a_while for ~2s
//   (20 10Hz ticks until signal 'c > 20' is true)
take_a_while() until c > 20

```


## Advanced: Using gRPC Protos

When Roxal is built with `ROXAL_ENABLE_GRPC=ON`, you can import Protocol Buffer schemas at runtime. Supply the directory containing your `.proto` files with `-p` (or set `ROXALPATH`) when running scripts:

```bash
./roxal -p compiler/grpc/protos my_grpc_script.rox
```

### Importing a proto

```php
import roxal_examples.*

var req = EchoRequest(payload=Everything(text="hi"))
var svc = EverythingService("127.0.0.1:50051")
var reply = svc.Echo(req)
print(reply.payload.text)
```

`import packagename.*` exposes the generated message types, enums, and services inside a module named for the proto `package` (or the filename stem if none is declared). The older `packagename.proto.*` form still works for backward compatibility.

### Type mapping

| Protobuf type                     | Roxal type                    |
|-----------------------------------|-------------------------------|
| `double`, `float`                 | `real`                        |
| `int32`, `sint32`, `uint32`, etc. | `int`                         |
| `bool`                            | `bool`                        |
| `string`                          | `string`                      |
| `bytes`                           | `string` (raw UTF-8)          |
| `enum`                            | Roxal `enum` type             |
| `message Foo`                     | object type `Foo`             |
| `repeated T`                      | `list` of the mapped `T` type |

Nested messages become nested Roxal object types, so you can treat them like any other object—read or assign fields, pass them to functions, or store them in collections.

Proto enums are emitted as real Roxal enum types, so you can refer to labels such as `Color.COLOR_RED` directly, compare them, or pass them wherever an enum is expected.

### Services as actors

Each proto `service` is emitted as a Roxal actor type. Actor instances expose:

* `init(addr="127.0.0.1:50051", opts=dict)` to configure the target endpoint and optional channel arguments (`timeout_ms`, keep-alive settings, max message sizes, etc.).
* One method per RPC. Invoking `svc.SomeRpc(req)` queues the call on the actor thread and returns a future that resolves to the RPC response. If the gRPC status is not `OK`, the future raises a Roxal `RuntimeException` (or `ProgramException` for application-level status codes) whose `detail` dict captures the gRPC status code/name/message.

RPC methods accept flattened parameters that mirror the request message fields, plus an optional `request` parameter. All of the field parameters default to `nil`, so you can call an RPC with only the fields you care about:

```php
var svc = EverythingService()
var resp = svc.Echo(payload=Everything(text="hi"))
// or reuse an existing request:
var req = EchoRequest(payload=Everything())
resp = svc.Echo(request=req)
```

If `request` is provided, the per-field parameters are ignored. Under the hood the method constructs a fresh request instance, populates the fields you specified, and sends it across gRPC. Because services and messages live in the proto package module, you can import and use them just like any other Roxal code.

Responses are flattened in the opposite direction when possible: if the RPC's response message has exactly one field, the future resolves to the field value instead of the wrapper object. If the response message is empty (has no fields) the future resolves to `nil`, allowing callers to `wait` on the RPC even though there is no payload. Responses with multiple fields continue to return the full response object.

### Streaming RPCs

gRPC streaming RPCs are supported using Roxal signals. Signals naturally represent streams: setting a signal's value sends a message, and stopping a signal closes the stream.

**Server streaming** (server sends multiple responses):

```php
// Proto: rpc ServerStream(Request) returns (stream Response);
var responseSignal = svc.ServerStream(count=5)
wait(for=responseSignal)  // Wait for future to resolve to signal

when responseSignal changes as evt:
    print("Received: " + evt.value)

// Check if stream is still active
if responseSignal.running:
    print("Stream still open")

// Wait for stream to end
wait(ms=1000)
```

The RPC returns a future that resolves to a signal. Each server message updates the signal's value, triggering `when ... changes` handlers. When the server closes the stream, the signal's `.running` property becomes `false`.

**Client streaming** (client sends multiple requests):

```php
// Proto: rpc ClientStream(stream Request) returns (Response);
var inputSignal = signal(0, StreamRequest(value=1))
var responseFuture = svc.ClientStream(value=inputSignal)

// Send messages by setting the signal
inputSignal.set(StreamRequest(value=10))
inputSignal.set(StreamRequest(value=20))

// Close the client stream
inputSignal.stop()

// Get the final response
var response = wait(for=responseFuture)
print("Sum: " + response.value)
```

When any RPC parameter is a signal, the call becomes a streaming RPC. Each `.set()` on the input signal sends a new request message. Calling `.stop()` on all input signals closes the client side of the stream.

**Bidirectional streaming**:

```php
// Proto: rpc BiStream(stream Request) returns (stream Response);
var inputSignal = signal(0, StreamRequest(value=1))
var outputSignal = svc.BiStream(value=inputSignal)
wait(for=outputSignal)

// Send and receive concurrently
when outputSignal changes as evt:
    print("Server sent: " + evt.value)

inputSignal.set(StreamRequest(value=5))
inputSignal.set(StreamRequest(value=10))
inputSignal.stop()  // Done sending

// Wait for server to finish
wait(ms=500)
```

**Signal properties for streaming**:

| Property/Method | Description |
|-----------------|-------------|
| `.running`      | `true` while the stream is active, `false` after stream ends |
| `.stop()`       | Close the client side of the stream (for input signals) |
| `.set(value)`   | Send a new message on the stream (for input signals) |


## Advanced: DDS Integration

Roxal can import DDS IDL (`.idl`) when built with `-DROXAL_ENABLE_DDS=ON`. An import like `import HelloWorldData` will locate `HelloWorldData.idl`, generate Roxal types (structs/enums), constants, and typedef aliases, and expose them as a module. Built-in functions live in the `dds` module (participants, topics, readers/writers, and convenience reader/writer signals).

Supported IDL subset (aligned with the ROS 2 profile): structs (final/appendable/mutable), enums, optional fields, bounded/unbounded strings and sequences, fixed-size arrays (including arrays of enums/structs), typedefs, and consts. 64-bit ints map to Roxal `int`. Known unsupported/unsupported-to-parse: unions, maps, bitsets/bitmasks.

Marshalling notes: bounded strings/sequences enforce bounds at write time. Fixed arrays are flattened to a single list on the Roxal side. For array-bearing types we avoid CycloneDDS’s serialized typeinfo descriptors because our manual layout doesn’t yet interpret the descriptor `m_ops` for arrays; everything else reuses the generated typeinfo for efficiency.

Common `dds` functions

- `create_participant(domain_id=0, qos=nil)`
- `create_topic(participant, name_or_type, msg_type, qos=nil)`
- `create_writer(participant, topic, qos=nil)`
- `create_reader(participant, topic, qos=nil)`
- `write(writer, msg)`
- `read(reader)` (takes one sample or returns nil)
- `close(handle_or_obj)` (participant/topic/reader/writer)
- Convenience: `writer_signal(name, msg_type, participant=nil, qos=nil, initial=nil)` and `reader_signal(name, msg_type, participant=nil, qos=nil, initial=nil)` create participant/topic/writer/reader as needed and return a Roxal signal.
- Lower level signal helpers: `create_writer_signal(writer, initial=nil)` and `create_reader_signal(reader, initial=nil)` wrap existing entities.

QoS dict keys supported (strings, case-insensitive):
- `reliability`: `"reliable"` or `"best_effort"`
- `durability`: `"volatile"` or `"transient_local"`
- `history`: number (depth) or dict `{kind: "keep_last"/"keep_all", depth: N}`
- `deadline_ms`, `lifespan_ms`, `latency_budget_ms`
- `liveliness`: dict `{kind: "automatic"/"manual_by_topic"/"manual_by_participant", lease_ms: N}`
- `ownership`: `"shared"` or `"exclusive"`
- `partition`: list of strings

Quick examples

Basic pub/sub:
```roxal
import HelloWorldData
import dds

p = dds.create_participant()
t = dds.create_topic(p, "Hello", HelloWorldData.Msg)
w = dds.create_writer(p, t)
r = dds.create_reader(p, t)

m = HelloWorldData.Msg()
m.userID = 7
m.message = "hi"
dds.write(w, m)
print(dds.read(r).message)
```

Signals (auto-create participant/topic):
```roxal
import HelloWorldData
import dds

wsig = dds.writer_signal("SignalTopic", HelloWorldData.Msg)
rsig = dds.reader_signal("SignalTopic", HelloWorldData.Msg)

var msg = HelloWorldData.Msg()
msg.userID = 99
msg.message = "hello via signal"
wsig.set(msg)  // publishes

when rsig changes as evt:
  print("received {evt.value.message}")
```

Custom QoS:
```roxal
var qos = {
  'reliability': 'reliable',
  'history': { 'kind': 'keep_last', 'depth': 5 },
  'deadline_ms': 100
}
t = dds.create_topic(p, "QoSTopic", HelloWorldData.Msg, qos)
```



## Advanced: Image Processing (media)

When Roxal is built with `ROXAL_ENABLE_MEDIA=ON`, the `media` module provides image loading, manipulation, and conversion for use with neural network inference pipelines or general image processing.

### Loading and Saving Images

```roxal
import media

var img = media.Image("photo.jpg")
print(img.width())     // e.g. 640
print(img.height())    // e.g. 480
print(img.channels())  // 3 (RGB)

img.write("output.png")             // save as PNG
img.write("output.jpg", quality=90) // save as JPEG (quality 1-100)
```

Image format (PNG or JPEG) is detected from the file extension. Internally, images are stored as tensors with shape `[H, W, C]` in uint8 (0-255) or float32 (0.0-1.0) format.

### Creating Images from Tensors

You can create an image from a tensor (e.g. to save a neural network output as an image):

```roxal
import media

// Create a 256x256 grayscale mask
var mask_data = tensor(256, 256, 1, dtype='uint8')
// ... fill in pixel values ...

var mask_img = media.Image(source=mask_data)
mask_img.write("mask.png")
```

### Geometric Transforms

All transforms modify the image in-place:

```roxal
import media

var img = media.Image("photo.jpg")

img.resize(320, 240)        // resize to 320x240 (bilinear interpolation)
img.crop(10, 20, 100, 100)  // crop 100x100 region from (10, 20)
img.pad(1024, 1024)         // pad with zeros (black) to 1024x1024, original at top-left

img.flip_horizontal()       // mirror left-right
img.flip_vertical()         // mirror top-bottom
img.rotate90()              // rotate 90 degrees clockwise
img.rotate180()
img.rotate270()
```

The `pad()` method is useful for neural networks that require fixed-size square inputs (e.g. SAM2 requires 1024x1024). Resize the longest side first, then pad to fill the remaining space:

```roxal
import media
import math

var img = media.Image("photo.jpg")
var scale = 1024.0 / math.fmax(real(img.width()), real(img.height()))
img.resize(int(real(img.width()) * scale), int(real(img.height()) * scale))
img.pad(1024, 1024)
```

### Color and Brightness Adjustments

```roxal
img.grayscale()         // convert to single-channel grayscale
img.brightness(1.5)     // brighter (>1.0) or darker (<1.0)
img.contrast(1.2)       // more contrast (>1.0) or less (<1.0)
img.saturation(0.0)     // desaturate (0=gray, 1=unchanged, >1=vivid)
```

### Format Conversion and Normalization

```roxal
img.to_float()           // uint8 (0-255) → float32 (0.0-1.0)
img.to_uint8()           // float32 (0.0-1.0) → uint8 (0-255)
img.normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225])  // ImageNet normalization (must be float32)
```

### Preparing Images for Neural Networks

The `to_tensor()` method converts an image to the `[1, C, H, W]` tensor format expected by most neural networks, combining uint8→float32 conversion and optional normalization in one step:

```roxal
import media
import ai.nn

var img = media.Image("photo.jpg")
img.resize(224, 224)

// Without normalization (just converts to [1, 3, 224, 224] float32)
var input = img.to_tensor()

// With ImageNet normalization (common for ResNet, YOLO, DETR, SAM2, etc.)
var input = img.to_tensor(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225])

var model = ai.nn.Model("classifier.onnx")
var output = model.predict(input)
```

### API Reference

#### Image Type

| Method | Description |
|--------|-------------|
| `Image(path, source=nil, channels=0)` | Create from file path or source tensor. `channels`: 0=auto, 1=gray, 3=RGB, 4=RGBA. |
| `write(path, quality=95)` | Save to PNG or JPEG (detected from extension). `quality` applies to JPEG. |
| `width()` | Image width in pixels. |
| `height()` | Image height in pixels. |
| `channels()` | Number of channels (1=gray, 3=RGB, 4=RGBA). |
| `resize(width, height)` | Resize using bilinear interpolation. In-place. |
| `crop(x, y, width, height)` | Crop rectangular region. `(x,y)` is top-left. In-place. |
| `pad(width, height)` | Pad with zeros to target size. Original at top-left. In-place. |
| `flip_horizontal()` | Mirror left-right. In-place. |
| `flip_vertical()` | Mirror top-bottom. In-place. |
| `rotate90()` | Rotate 90 degrees clockwise. In-place. |
| `rotate180()` | Rotate 180 degrees. In-place. |
| `rotate270()` | Rotate 270 degrees clockwise. In-place. |
| `grayscale()` | Convert to single-channel grayscale. In-place. |
| `brightness(factor)` | Adjust brightness (>1 brighter, <1 darker). In-place. |
| `contrast(factor)` | Adjust contrast (>1 more, <1 less). In-place. |
| `saturation(factor)` | Adjust saturation (0=gray, 1=unchanged, >1=vivid). In-place. |
| `to_float()` | Convert uint8 (0-255) to float32 (0.0-1.0). In-place. |
| `to_uint8()` | Convert float32 (0.0-1.0) to uint8 (0-255). In-place. |
| `normalize(mean, std)` | Per-channel normalization: `(pixel - mean[c]) / std[c]`. Must be float32. In-place. |
| `to_tensor(mean=nil, std=nil)` | Return `[1, C, H, W]` float32 tensor for neural network input. Optional `mean`/`std` lists apply per-channel normalization. |


## Advanced: Neural Network Inference (ai.nn)

When Roxal is built with `ROXAL_ENABLE_AI_NN=ON` (which implies `ROXAL_ENABLE_ONNX=ON`), the `ai.nn` module provides neural network inference via ONNX Runtime. Models are loaded from `.onnx` files and can run on CPU or GPU (CUDA). Inference is asynchronous — `predict()` returns a future that auto-resolves when results are accessed. Model outputs are tensors that integrate directly with Roxal's signal/dataflow engine, enabling reactive model pipelines.

### Loading and Running a Model

```roxal
import ai.nn

var model = ai.nn.Model("mnist-8.onnx")

# Inspect the model
print(model.inputs())   // [{name: "Input3", shape: [1, 1, 28, 28], dtype: "float32"}, ...]
print(model.outputs())  // [{name: "Plus214_Output_0", shape: [1, 10], dtype: "float32"}, ...]
print(model.device())   // "cpu" or "cuda"

# Create a 28x28 input image (digit "1": vertical stroke)
var img = tensor(1, 1, 28, 28, dtype='float32')
var r = 4
while r <= 23:
  img[0, 0, r, 13] = 1.0
  img[0, 0, r, 14] = 1.0
  r = r + 1

# Run inference
var output = model.predict(img)

# output is a tensor with shape [1, 10] — one score per digit
# Find the predicted digit (argmax)
var best = 0
var best_score = output[0, 0]
var i = 1
while i < 10:
  if output[0, i] > best_score:
    best = i
    best_score = output[0, i]
  i = i + 1

print(best)  // 1

model.close()
```

### Device Selection

By default, `load()` uses GPU (CUDA) when available, falling back to CPU. The `device` parameter overrides this:

```roxal
var model_gpu = ai.nn.Model("model.onnx")                    // auto (GPU if available)
var model_cpu = ai.nn.Model("model.onnx", device='cpu')      // force CPU
var model_cuda = ai.nn.Model("model.onnx", device='cuda')    // request GPU (error if unavailable)
```

The `warmup` parameter (default `true`) runs an initial dummy inference to warm caches:

```roxal
var model = ai.nn.Model("model.onnx", device='cpu', warmup=false)  // skip warm-up
```

### Multi-Input and Multi-Output Models

For models with multiple inputs, pass a dict (by name) or a list (by position):

```roxal
var model = ai.nn.Model("add-sub.onnx")  // inputs: a, b — outputs: sum_out, diff_out

var a = tensor(1, 3, dtype='float32')
a[0, 0] = 10.0
a[0, 1] = 20.0

var b = tensor(1, 3, dtype='float32')
b[0, 0] = 1.0
b[0, 1] = 2.0

# Dict-based (by input name)
var results = model.predict({'a': a, 'b': b})
print(results[0][0, 0])  // 11 (sum)
print(results[1][0, 0])  // 9  (diff)

# List-based (by position)
var results2 = model.predict([a, b])
```

When a model has multiple outputs, `predict()` returns a list of tensors. For single-output models, it returns a single tensor.

### Chaining Models

Model outputs are tensors that can be passed directly as inputs to another model. When both models run on GPU, intermediate tensors stay in GPU memory with no copies:

```roxal
var encoder = ai.nn.Model("encoder.onnx")
var decoder = ai.nn.Model("decoder.onnx")

var input = tensor(1, 10, dtype='float32')
input[0, 0] = 1.0
input[0, 3] = 42.0

var mid = encoder.predict(input)     // output tensor (GPU if available)
var result = decoder.predict(mid)    // GPU→GPU, zero-copy

print(result[0, 0])
print(result[0, 3])

encoder.close()
decoder.close()
```

### Reactive Inference with Signals

The `predict()` method integrates with Roxal's signal/dataflow engine. When called with a signal argument, it creates a derived signal that automatically re-runs inference whenever the input signal changes:

```roxal
import ai.nn

var model = ai.nn.Model("mnist-8.onnx")

# Create a source signal for input images (10 Hz)
var empty = tensor(1, 1, 28, 28, dtype='float32')
var input_sig = signal(10, empty)

# Create a derived prediction signal — re-runs model automatically on input change
var output_sig = model.predict(input_sig)

# React to new predictions
var predictions = []
when output_sig changes as evt:
  var out = evt.value
  # ... compute argmax to get predicted digit ...
  predictions.append(best)

input_sig.run()

# Update the input — the model re-runs automatically
var img1 = tensor(1, 1, 28, 28, dtype='float32')
# ... draw digit "1" ...
input_sig.set(img1)
wait(ms=500)

# Update again — triggers another prediction
var img0 = tensor(1, 1, 28, 28, dtype='float32')
# ... draw digit "0" ...
input_sig.set(img0)
wait(ms=500)

for p in predictions:
  print(p)  // 1, 0
```

### Signal-Based Model Chains

Signals chain naturally through multiple models, creating reactive GPU pipelines:

```roxal
import ai.nn

var model_a = ai.nn.Model("encoder.onnx")
var model_b = ai.nn.Model("decoder.onnx")

# Build signal chain: input → model_a → model_b
var initial = tensor(1, 10, dtype='float32')
var input_sig = signal(10, initial)
var mid_sig = model_a.predict(input_sig)      // derived signal
var output_sig = model_b.predict(mid_sig)     // chained derived signal

when output_sig changes as evt:
  print(evt.value)

input_sig.run()

# Setting input propagates through the entire chain automatically
var input = tensor(1, 10, dtype='float32')
input[0, 3] = 42.0
input_sig.set(input)
wait(ms=300)
```

When models run on GPU, intermediate tensors stay on GPU throughout the signal chain — no CPU round-trip.


### API Reference

#### Module Functions

| Function | Description |
|----------|-------------|
| `tensor_device(t)` | Return the device where a tensor resides (`'cpu'` or `'cuda'`). |
| `memory_info(device='auto')` | Return memory info dict: `{device, total, free, used}` (bytes). |

#### Model Type

| Method | Description |
|--------|-------------|
| `Model(path, device='auto', warmup=true)` | Load an ONNX model. Device: `'auto'`, `'cpu'`, or `'cuda'`. Set `warmup=false` to skip initial warm-up inference. |
| `predict(input)` | Run inference. Input: tensor, dict `{name: tensor}`, list of tensors, or a signal. Returns tensor (or list if multiple outputs). With a signal input, returns a derived signal. |
| `inputs()` | Return list of input descriptors: `[{name, shape, dtype}, ...]` |
| `outputs()` | Return list of output descriptors: `[{name, shape, dtype}, ...]` |
| `device()` | Return execution device string (`'cpu'` or `'cuda'`). |
| `close()` | Release model session and free resources. |


## Builtin Modules & Functions Reference

The functions in the sys module are always globally available (- as if `import sys.*` were used).  See `sys.rox`.

### sys

#### Variables
* `args` - list of command-line arguments passed to the script (not including the script filename)

#### Functions
* `print(value='')` - print the string representation of `value` followed by a newline
* `len(v)` - return the length of `v` if applicable
* `help(fn)` - return signature and doc string for `fn`
* `clone(v)` - deep copy `v`
* `wait(s=0, ms=0, us=0, ns=0, for=nil)` - pause execution for the specified time and optionally await a future or list of futures afterwards
* `stacktrace()` - return the current call stack as a list
* `serialize(value, protocol='default')` - serialize `value` using protocol
* `deserialize(bytes, protocol='default')` - deserialize bytes using protocol
* `to_json(value, indent=true)` - convert value to a JSON string
* `from_json(json)` - parse JSON string into a value
* `filter(items, predicate)` - return a new list containing elements for which `predicate(element)` returns true; predicate can optionally take `(element, index)`. Also a list method: `list.filter(predicate)`
* `map(items, transform)` - return a new list with `transform(element)` applied to each element; transform can optionally take `(element, index)`. Also a list method: `list.map(transform)`
* `reduce(items, reducer, initial)` - reduce list to a single value by calling `reducer(accumulator, element)` for each element; reducer can optionally take `(accumulator, element, index)`. Also a list method: `list.reduce(reducer, initial)`
* `Time` - timestamp object; use `Time.wall_now(tz='local')`, `Time.steady_now()`, or `Time.parse(...)` to construct and call instance methods like `format(...)`, `components(...)`, `diff(other)`, `seconds()`, and `microseconds()`
* `TimeSpan` - duration object; construct via `TimeSpan(...)` or `TimeSpan.from_fields(...)`, query parts with `split()`, `seconds()`, `microseconds()`, and totals such as `total_seconds()` or `human()`
* `clock(freq)` - create a clock signal at `freq`
* `signal(freq, initial)` - create a source signal
* `typeof(value)` - return the type of `value`
* `loadlib(path)` - load a native library from `path`
  * relative paths are resolved against the directory of the executing script

#### Internal (likely to be removed or renamed)
* `fork(fn)` - run `fn` in a new thread and return its id
* `join(id)` - wait for thread `id` to finish and return true if joined
* `_clock()` - return process time in seconds
* `_threadid()` - return the current thread id
* `_stackdepth()` - depth of the current call stack
* `_runtests(suite)` - run builtin tests named by `suite`
* `_weakref(value)` - create a weak reference to `value`
* `_weak_alive(value)` - true if weak reference is still valid
* `_strongref(value)` - convert weak reference back to strong reference
* `_engine_stop()` - stop the dataflow engine
* `_df_graph()` - textual representation of the dataflow graph
* `_df_graphdot(title='')` - graphviz dot of the dataflow graph

### math

Additional mathematical operations.
Use `import math` or `import math.*`.  See `math.rox`.

* `sin(x)` - sine of `x`
* `cos(x)` - cosine of `x`
* `tan(x)` - tangent of `x`
* `asin(x)` - arc sine of `x`
* `acos(x)` - arc cosine of `x`
* `atan(x)` - arc tangent of `x`
* `atan2(y, x)` - arc tangent of `y/x`
* `sinh(x)` - hyperbolic sine of `x`
* `cosh(x)` - hyperbolic cosine of `x`
* `tanh(x)` - hyperbolic tangent of `x`
* `asinh(x)` - inverse hyperbolic sine
* `acosh(x)` - inverse hyperbolic cosine
* `atanh(x)` - inverse hyperbolic tangent
* `exp(x)` - `e` raised to `x`
* `log(x)` - natural logarithm of `x`
* `log10(x)` - base-10 logarithm of `x`
* `log2(x)` - base-2 logarithm of `x`
* `sqrt(x)` - square root of `x`
* `cbrt(x)` - cube root of `x`
* `ceil(x)` - smallest integer greater than or equal to `x`
* `floor(x)` - largest integer less than or equal to `x`
* `round(x)` - round `x` to nearest integer
* `trunc(x)` - truncate fractional part of `x`
* `fabs(x)` - absolute value of `x`
* `hypot(x, y)` - square root of `x*x + y*y`
* `fmod(x, y)` - floating point remainder of `x/y`
* `remainder(x, y)` - IEEE remainder of `x/y`
* `fmax(x, y)` - maximum of `x` and `y`
* `fmin(x, y)` - minimum of `x` and `y`
* `pow(x, y)` - `x` raised to the power `y`
* `fma(x, y, z)` - fused multiply-add
* `copysign(x, y)` - `x` with the sign of `y`
* `erf(x)` - error function
* `erfc(x)` - complementary error function
* `exp2(x)` - `2` raised to `x`
* `expm1(x)` - `e**x - 1` with extra precision
* `fdim(x, y)` - positive difference of `x` and `y`
* `lgamma(x)` - log gamma of `x`
* `log1p(x)` - `log(1 + x)`
* `logb(x)` - exponent of `x`
* `nearbyint(x)` - round `x` to nearest integer
* `nextafter(x, y)` - next representable number after `x` toward `y`
* `rint(x)` - round `x` using current rounding mode
* `tgamma(x)` - gamma function of `x`
* `identity(n)` - `n` by `n` identity matrix
* `zeros(r, c)` - `r` by `c` matrix of zeros
* `ones(r, c)` - `r` by `c` matrix of ones
* `dot(a, b)` - dot product of two vectors
* `cross(a, b)` - cross product of two 3-element vectors
* `relu(x)` - rectified linear unit: `max(0, x)` applied element-wise (works on scalar, vector, matrix, or tensor)
* `softmax(x)` - softmax function: `exp(x_i) / sum(exp(x_j))` (works on vector or 1D tensor)
* `argmax(x)` - index of maximum element (works on vector or 1D tensor)

### fileio

Functions for read & writing files and managing files, directories & paths.
Use `import fileio` or `import fileio.*`.  See `fileio.sys`.
(only available when built with cmake option ROXAL_ENABLE_FILEIO is on)

* `open(path, append=false, write=false, format='text')` - open a file and return handle (write access is enabled automatically when `append` is true)
* `close(file)` - close a file handle
* `is_open(file)` - true if handle is open
* `more_data(file)` - true if more data can be read
* `read(file)` - read available data from file
* `read_line(file)` - read a line of text
* `read_file(path, format='text')` - read entire file
* `write(file, data)` - write data to file
* `flush(file)` - flush buffered writes to the underlying file
* `file_exists(path)` - true if file exists
* `dir_exists(path)` - true if directory exists
* `create_dir(path, recurse=false)` - create a directory (optionally creating parents)
* `file_size(path)` - size of file in bytes
* `absolute_file_path(path)` - absolute path of file
* `path_directory(path)` - directory portion of path
* `path_file(path)` - file name portion of path
* `file_extension(path)` - extension of path
* `file_without_extension(path)` - path without the extension
* `delete_file(path)` - delete a file, returning true if it existed
* `delete_dir(path, recurse=false)` - delete a directory, optionally recursively

**Note:** `read`, `read_line`, `read_file`, and `write` do not block, but return futures that are automatically resolved when used.


### regex

Regular expression support using PCRE2.
Use `import regex` or `import regex.*`. See `regex.rox`.
(only available when built with cmake option ROXAL_ENABLE_REGEX is on)

#### Module Functions

* `compile(pattern, flags='')` - compile a regex pattern and return a `Regex` object

#### Regex Type

The `Regex` type represents a compiled regular expression pattern.

**Constructor:**
* `Regex(pattern, flags='')` - create a new Regex from pattern string and optional flags

**Methods:**
* `test(str)` - return `true` if pattern matches anywhere in `str`
* `exec(str)` - execute match and return a dict with match details, or `nil` if no match

**Flags:**
| Flag | Description |
|------|-------------|
| `'i'` | Case-insensitive matching |
| `'m'` | Multiline mode (`^` and `$` match line boundaries) |
| `'s'` | Dotall mode (`.` matches newlines) |
| `'g'` | Global mode (affects `replace` behavior) |

**exec() Result:**

When `exec()` finds a match, it returns a dict containing:
* `'match'` - the full matched string
* `'index'` - the starting position of the match in the input string
* `'groups'` - a list of captured groups (excluding the full match)
* `'named'` - a dict of named capture groups (if any)

```php
import regex.*

var re = Regex('(\\w+)@(\\w+)')
var m = re.exec('user@host')
print(m['match'])   // "user@host"
print(m['index'])   // 0
print(m['groups'])  // ["user", "host"]

// Named capture groups
var reNamed = Regex('(?<year>\\d{4})-(?<month>\\d{2})')
var mNamed = reNamed.exec('2024-03-15')
print(mNamed['match'])  // "2024-03"
print(mNamed['named'])  // {"year": "2024", "month": "03"}
```

#### String Methods

When the regex module is enabled, strings gain the following methods that accept either a `Regex` object or a plain string pattern (which is auto-compiled):

* `match(pattern)` - find matches and return a list, or `nil` if no match
* `search(pattern)` - return the index of the first match, or `-1` if not found
* `replace(pattern, replacement)` - replace matches with `replacement` string
* `split(pattern)` - split string by pattern and return a list

```php
import regex.*

var str = 'hello world'

// Using Regex objects
print(str.search(Regex('world')))           // 6
print(str.match(Regex('\\w+')))             // ["hello"]
print(str.replace(Regex('world'), 'there')) // "hello there"

// Using plain string patterns (auto-compiled)
print('a,b,c'.split(','))                   // ["a", "b", "c"]
print('test123'.match('\\d+'))              // ["123"]
print('foo bar'.replace('bar', 'baz'))      // "foo baz"

// Global flag for replace-all
var str2 = 'foo bar boo'
print(str2.replace(Regex('o', 'g'), 'O'))   // "fOO bar bOO"
```

### socket

TCP and UDP socket networking support using POSIX sockets.
Use `import socket` or `import socket.*`. See `socket.rox`.
(only available when built with cmake option ROXAL_ENABLE_SOCKET is on - enabled by default)

**Key Design**: Blocking operations (`accept`, `connect`, `recv`, `recvfrom`, `gethostbyname`) return futures to avoid blocking. Use `wait(for=future)` or type conversion to resolve/wait.

#### Module Functions

* `tcp()` - create a TCP socket (SOCK_STREAM)
* `udp()` - create a UDP socket (SOCK_DGRAM)
* `gethostbyname(hostname)` - resolve hostname to IP address, returns `future<string>`

#### Socket Type

The `Socket` type represents a network socket for TCP or UDP communication.

**Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `bind(host, port)` | bool | Bind socket to local address |
| `listen(backlog=5)` | bool | Start listening for connections (TCP) |
| `accept()` | future<[Socket, [host, port]]> | Accept incoming connection (TCP) |
| `connect(host, port)` | future<bool> | Connect to remote address (TCP) |
| `send(data)` | int | Send data on connected socket (immediate) |
| `recv(size=4096)` | future<string> | Receive data from connected socket |
| `sendto(data, host, port)` | int | Send data to address (UDP, immediate) |
| `recvfrom(size=4096)` | future<[data, host, port]> | Receive data with sender address (UDP) |
| `close()` | nil | Close the socket |
| `settimeout(seconds)` | nil | Set timeout (nil=blocking, 0=non-blocking) |
| `setsockopt(option, value)` | bool | Set socket option |
| `getsockname()` | [host, port] | Get local address |
| `getpeername()` | [host, port] | Get remote address |
| `fileno()` | int | Get underlying file descriptor |

**Socket Options** (for `setsockopt`):

| Option | Type | Description |
|--------|------|-------------|
| `'reuseaddr'` | bool | Allow address reuse (SO_REUSEADDR) |
| `'broadcast'` | bool | Allow broadcast (SO_BROADCAST, UDP) |
| `'keepalive'` | bool | Enable keepalive (SO_KEEPALIVE, TCP) |
| `'nodelay'` | bool | Disable Nagle algorithm (TCP_NODELAY) |
| `'rcvbuf'` | int | Receive buffer size |
| `'sndbuf'` | int | Send buffer size |

#### Examples

**TCP Echo Client:**
```php
import socket

var s = socket.tcp()
wait(for=s.connect("127.0.0.1", 8080))  // Wait for connection
s.send("Hello")
var response = wait(for=s.recv(1024))
print("Got: " + response)
s.close()
```

**TCP Echo Server:**
```php
import socket

var server = socket.tcp()
server.setsockopt("reuseaddr", true)
server.bind("0.0.0.0", 8080)
server.listen(5)

while true:
  client_hostport = server.accept()
  wait(for=client_hostport)
  [client, hostport] = client_hostport
  [host,port] = hostport
  print("Connection from {host}:{port}")
  var data = client.recv(1024)
  wait(for=data)
  client.send("Echo: " + data)
  client.close()
```

**UDP Send/Receive:**
```php
import socket

// Sender
var sender = socket.udp()
sender.sendto("hello", "127.0.0.1", 9000)
sender.close()

// Receiver
var receiver = socket.udp()
receiver.bind("0.0.0.0", 9000)
var data_host_port = receiver.recvfrom(1024)
wait(for=data_host_port)
[data, host, port] = data_host_port
print("Received '{data}' from {host}:{port}")
receiver.close()
```

**DNS Lookup:**
```php
import socket

var ip = socket.gethostbyname("example.com")
wait(for=ip)

print("IP: " + ip)  // e.g., "104.18.26.120"
```
