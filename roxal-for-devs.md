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

### Assignments

Like most programming languages, `=` is used for assignment (unlike in mathematics, where is means equality).
This works as you'd expect for value types.  For reference types, the references are usually being assigned.

```roxal
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

```roxal
func f(): // return an int, a string and a dict
  return [1, 'a', {'a':'A'}]

[i, s, d] = f()
print(i)  // 1
print(s)  // 'a'
print(d)  // {'a':'A'}
```

Advanced: The _copy into_ operation (`←` or `<-`) makes a shallow copy of the RHS and copies it into the LHS (- hence, the LHS must be compatible - e.g. the same type)

```roxal
l = [1,2,3]
l2 = []
l2 <- l   // copy elements from l into l2
print(l2) // [1,2,3]
l[0] = 11 // change l
print(l2) // [1,2,3]  (still same, list l content was changed, not l2's)
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

The operators +, -, \*, / and % work how you'd expect on builtin numeric types.  Vectors and Matrices also support +, - and \*, performing matrix multiplication, vector*matrix multiplication and dot products (two vectors).

```roxal
m = [1 2
     3 4]
v = [1 2]
print(m*v) // [5 11]
```

In addition, + can also be used for:
  * string concatenation (when the left-hand-side is a string) - "hello "+"world".  This also directly converts most types into strings: "hello "+5 → "hello 5"
  * list concatenation: [1,2,3]+[4,5,6] → [1,2,3,4,5,6]

Boolean operators and, or and not work on the bool type.  (true and true and not false) → true

Bitwise operators | (or), & (and), ~ (not) and ^ (xor) work with bool, byte and int types.
In addition, | for dict will merge two dicts into one (with precedence for the RHS keys) and & for dict will yield a dict with the intersection (common) keys (with values from the LHS in case of a common key in both)

```roxal
print({'a':1,'b':2} | {'b':3,'c':4}) // {"a": 1, "b": 3, "c": 4} - keys from LHS or RHS
print({'a':1,'b':2} & {'b':3,'c':4}) /  {"b": 2} - keys in LHS and RHS
```

The equality operators `≟` (is equal to), `≠` (not equal to), `<` / `>` (less / greater than), `≤` / `≥` (less / greater than or equal to) function as expected bool, byte, int, decimal, range, vector & matrix.  Note that the `==` and `!=` or `<>` familiar from C are also available.

```
if 5 ≥ 4:
  print('always')
v = [1 2]         // vector
print(v == [1 2]) // true
```

However, for reference types, like user-defined objects & actors (more below), equality only compares the reference.

The `is` operator:
  * Checks identity - when the operands are two (non-type) values, it compares the for being the same object (e.g. list, dict, vector, matrix, string)
  * Checks type - when the LHS is a type

```roxal
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

```roxal
print(list( range(1..5) ))   // [1, 2, 3, 4, 5]
print(list( range(1..<5) )) // [1, 2, 3, 4]
l = [1,2,3]
print( l[range(::-1)] ) // [3, 2, 1]  (indexing everything, in reverse)
print( len( range(0..<10) )) // use len() to count elements: 10
for i in range(1..10 by 2):
  print(i) // 1 3 5 ...
```

Indexing with ranges:
```roxal
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

```roxal
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
```roxal
import mymodule

mymodule.showVersion() // prints Version 1.0
```

`mymodule.rox`: (same folder)
```roxal
func showVersion():
  print('Version 1.0')
```

Notice that accessing the symbol names from the imported module required prefixing them with the module name separated by period(s).
If you want to import all of the module's names into the current module scope, you can use `import mymodule.*'

```roxal
import math.*
print(cos(0.0)) // didn't need to write math.cos
```

You can nest modules using folders in the filesystem: e.g. to import `mymodule/submodule/toplevel.rox`

```roxal
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

```roxal
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

```roxal
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

```roxal
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

```roxal
try:
  dostuff()
except e :RuntimeException:
  print("Something exceptional happened: "+e)
finally:
  print("That's all")
```


## Events

Events are useful for constructing responsive programs.  For robotics, this may be to respond to I/O, sensor data or other internal states of the program.
A variable of type `event` can be declared to refer to a type of event that can be 'triggered' to occur as desired.

```roxal
var e :event

// declare an event 'handler' that will be triggered whenever e occurs
//  (not executed here now - more like a func declaration)
on e:
  print('e occurred')

// some code in another thread (e.g. actor)
emit e   // trigger the event e to occur

// some time later in another place
emit e   // triggered again
```

## Signals

Signals in roxal represent values that can (spontaneously) change.  For example, for robotics, they might represent an external input.  Signals can be transformed, using functions (func) into new signals.  There are also builtin functions to create signals who's value has to be explicitly updated by your roxal code (`signal()`) or that automatically count up (`clock()`).  A signal's value can be any of the usual roxal value types, but most usefully bool, byte, int, real, vector or matrix.

Conceptually, you can think of signals as like wires in circuit, arious 'func' processing nodes that have input (parameter) and output (return) signals.

### Examples

A single clock signal:
```roxal

c = clock(freq=10)  // an int signal that counts up from 0 at 10Hz (initially stopped)

// register a signal change handler
// (same as handlers for events, but trigger whenever the indicated signal value changes)
on c:
  print( int(c) )  // will print an int every .1s while the script runs
                   // 'constructing' an int() from a signal samples the value at the time of evaluation

c.run()    // start the clock counting
wait(s=1)  // keep the script running so we can see ~10 prints
```

Transforming a some signals:
```roxal
c = clock(10)
... TODO ...
```


## Builtin Modules & Functions

### sys

### math

### fileio
