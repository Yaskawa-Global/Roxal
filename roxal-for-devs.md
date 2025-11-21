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
  * `int`  - signed 32bit
  * `real` - IEEE 64bit float (aka C double)
  * `decimal` - (unimplemented) fixed point (designed for no roundoff error for fractions in base 10)
  * `enum` - enumerated int labelled (similar to C)

Reference types:
  * `string` - Unicode (UTF-8) (literals are interned)
    * Single quoted `'like this'` or double quoted `"like this"`
    * With double quotes, `{}` placeholders interpolate identifiers, dotted properties, and indexes using identifiers, numeric literals, or single-quoted string literals. Multiple comma-separated indices are allowed (for example `"lookup={record['name']}"` or `"matrix element={mat[row, 2]}"`).
  * `list` - [list, of, values] - heterogeneous
  * `dict` - {key:value,key2:value2} - heterogeneous (hash, map)
    * insertion order preserved
  * `vector` - [number number number] - arbitrary n dim real scalar elements
  * `matrix` - [num num num; num num num] - arbitrary n x m dim real scalar elements (can use newline between rows in literals)
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
  * Checks identity - when the operands are two (non-type) values, it compares them for being the same object (e.g. list, dict, vector, matrix, string)
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

on ButtonPressed as evt:
  print('button '+evt.buttonId+' pressed')

emit ButtonPressed(buttonId=7)   // creates a fresh event instance and delivers it
```

Each `emit` call constructs a new event instance.  Instances are immutable snapshots: the handler receives the occurrence as `evt` above and can read the payload members.  Event types can inherit from other event types to reuse payload fields:

```php
type DeviceEvent event:
  var deviceId :int

type LowBattery event extends DeviceEvent:
  var percentRemaining :real

on LowBattery as evt:
  print('device '+evt.deviceId+' low ('+evt.percentRemaining+'%)')

emit LowBattery(deviceId=42, percentRemaining=12.5)
```

The builtin `event` type is still available for cases where no extra payload is required:

```php
var generic:event

on generic:
  print('generic event occurred')

emit generic()
```

Event types also expose `.on` and `.off` helpers that mirror the statement form:

```php
var subscription = LowBattery.on(func (evt): print(evt.percentRemaining))
LowBattery.emit(deviceId=1, percentRemaining=9.0)
LowBattery.off(subscription)
```

## Signals

Signals in roxal represent values that can (spontaneously) change.  For example, for robotics, they might represent an external input.  Signals can be transformed, using functions (func) into new signals.  To create your own source signals you "call" the builtin `signal` type (e.g. `signal(freq, initial)`), while `clock(freq)` provides a signal that counts up automatically.  A signal's value can be any of the usual roxal value types, but most usefully bool, byte, int, real, vector or matrix.

Conceptually, you can think of signals as like wires in circuit, connected to various 'func' processing nodes that have input (parameter) and output (return) signals.

### Examples

A single clock signal:
```php

c = clock(freq=10)  // an int signal that counts up from 0 at 10Hz (initially stopped)

// register a signal change handler (fires when the value changes)
on c changed as evt:
  print('tick='+evt.value)

c.run()    // start the clock counting
wait(s=1)  // keep the script running so we can see ~10 prints
```

The `changed` keyword is required when watching a signal.  Supplying `as evt` binds the automatically-generated event instance that contains the sampled signal value (`evt.value`), a steady-clock duration since the engine started (`evt.timestamp`, a `sys.TimeSpan`), and the signal's own tick count (`evt.tick`).

Transforming a some signals:
```php
initial_v = [1.0 2.0 3.0]  // vector

// source signals need to be explicitly updated with their set() method
s  = signal(freq=10,initial_v)  // source signal at 10Hz with initial vector value
s2 = signal(freq=10,initial_v)  //  (these won't change value unless set() is called)

dp = s*s2    // vector dot product (real scalar signal)

on dp changed as evt: // print if dp changes
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

Responses are flattened in the opposite direction when possible: if the RPC’s response message has exactly one field, the future resolves to the field value instead of the wrapper object. If the response message is empty (has no fields) the future resolves to `nil`, allowing callers to `wait` on the RPC even though there is no payload. Responses with multiple fields continue to return the full response object.



## Builtin Modules & Functions

The functions in the sys module are always globally available (- as if `import sys.*` were used).  See `sys.rox`.

### sys
* `print(value='')` - print the string representation of `value` followed by a newline
* `len(v)` - return the length of `v` if applicable
* `help(fn)` - return signature and doc string for `fn`
* `clone(v)` - deep copy `v`
* `wait(s=0, ms=0, us=0, ns=0, for=nil)` - pause execution for the specified time and optionally await a future or list of futures afterwards
* `fork(fn)` - run `fn` in a new thread and return its id
* `join(id)` - wait for thread `id` to finish and return true if joined
* `stacktrace()` - return the current call stack as a list
* `serialize(value, protocol='default')` - serialize `value` using protocol
* `deserialize(bytes, protocol='default')` - deserialize bytes using protocol
* `to_json(value, indent=true)` - convert value to a JSON string
* `from_json(json)` - parse JSON string into a value
* `Time` - timestamp object; use `Time.wall_now(tz='local')`, `Time.steady_now()`, or `Time.parse(...)` to construct and call instance methods like `format(...)`, `components(...)`, `diff(other)`, `seconds()`, and `microseconds()`
* `TimeSpan` - duration object; construct via `TimeSpan(...)` or `TimeSpan.from_fields(...)`, query parts with `split()`, `seconds()`, `microseconds()`, and totals such as `total_seconds()` or `human()`
* `clock(freq)` - create a clock signal at `freq`
* `signal(freq, initial)` - create a source signal
* `typeof(value)` - return the type of `value`
* `loadlib(path)` - load a native library from `path`
  * relative paths are resolved against the directory of the executing script

#### Internal (likely to be removed or renamed)
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
