# Implementation Notes

Roxal is a dynamic language with optional static typing and various features as follows:

  * Python-like syntax
  * Objects (OOP) and Actors (concurrency)
  * Builtin bool, int, real, decimal, enum, string, list, dict, signal, vector, matrix and tensor types
  * Signal engine (dataflow)
  * Events
  * Modules

## Compilation & execution

### Grammar

The grammar is contained in the Antlr4 `Roxal.g4` file.  This generates the parser abstract interface that is implemented by the ASTGenerator.

### Parsing

The `ASTGenerator` parses the parse gree to create the AST (Abstract Syntax Tree) as represented by the core/AST classes.

The `TypeDeducer` visits the AST and deduces types where possible.

### Compiler

The `RoxalCompiler` visits the AST and generates custom VM (Virtual Machine) bytescodes - see the `Chunk` type.  Each executable function emits a Chunk of OpCodes.

### Virtual Machine (VM)

The `VM` class executes the Chunk OpCodes.  It supports multiple threads via the `Thread` class and each `Thread` maintains its own stack.


## Types & Values

Runtime values in the language are represented by the Value class, which wraps a 64bit value.  This holds builtin primitives (`bool`, `int`, `real`, `decimal`, `enum`) and references to reference types.  The implementation uses NaN-boxing, whereby the full 64bit are used as a C double for the type `real`, but if the Quiet NaN (Not-a-Number) flags are set, then, it is instead assumed to be one of the other types, as stored in the type tag.

These by-value types can be testes via the various is*() Value methods (`Value::isBool()`, `isInt()`, etc).

In the case of reference types (`list`, `dict`, `vector`, `matrix`, `signal`, user-defined objects & actors), the `Value` only indicates the builtin type, or that is is an Object or Actor for user-defined types.  For enums, the Value holds the enum numeric value and a typeid value corresponding to a global registry of enum type information.  The reference is to an instance of class `Obj`.  Value manages reference counting for `Obj` references (via `incRef()` and `decRef()`).

The reference types are implemented in `Object.h`|`cpp`.  `list` and `dict` types currently use STL `std::vector` and `std::map` of Values.

The `vector`, `matrix` and `tensor` types utilize the Eigen library.  Although these are reference types, the intention is that they behave like value types (- the current implementation is a mixture - operations create new values, but they're passed by reference and assigning elements mutates)

## Scopes

Each `.rox` file is a module by default, even if not declared as such (according to the filename).  Within a module, module-scoped variables can be used without forward declarations - references by name are resolved at runtime.

Within a function or method, parameters and variable or function declarations are local.  These are access via via offsets from the function's execution frame pointer.

Functions are first-class values and can capture variables from outer scopes, yielding a closure (`ObjClosure`), which encapsulates the function's static code (`Chunk`), and captures upvalues.  Upvalues initially refer to stack entries of enclosing function scopes, but are 'lifted up' into the heap as required before the original stack positions are unwound.


## Object & Actor types

A new object types (like a C++ class) can be declared and have its own methods (`func` or `proc`) and member variables.  Members can be declared private, in which case they're not accessible outside the scope of the type's methods.

An actor type is similar to an object, but additionally has its own thread of execution associated with it.  This thread is the only thread that can execute the actor's methods. Hence, when another thread (e.g. the main script thread or another actor's thread) calls an actor instance's methods, a future for the return value (if any) is immediately returned to the caller, which can continue to execute asynchronously.  Only if that future needs to be converted into the return value will the execution block, if necessary, until the called actor's method has completed and returned to provide the value.  Hence, execution of methods within an actor are serialized, since there is only one thread, so that developers need not worry about shared state between multiple threads.

Complex reference types passed to an actor's method (or returned from it) behave as if deep-copied.  (currently, they are deep copied; but in future a Copy-on-Write implementation can be added so that only the reference need be passed in the common case of non-mutating parameters)

## Signals and Data-Flow

The VM includes a data-flow engine (in `Dataflow/`) that can represent a set of signals (`Signal` & `ObjSignal`) of Values that interconnect as inputs and outputs to function nodes (`FuncNode`).
The dataflow engine will updates signal values as they are effected by changes to other signals via functions.  There exists a special builtin function `clock(freq)` that creates a native signal that counts up at the specified frequency.

Function nodes wrap standard functions (`func`) and execute their `Chunk` code (via a `Closure`).

The data flow engine is represented as a builtin actor instance.  Hence, the evaluation of all functions (`FuncNode`s) happens on the dataflow engine's actor thread.

Signals can be sampled to yield their current value at any time on any thread, either via the builtin `value` property, or by using them to construct their underlying value type (e.g. `vector(vecsignal)`, or `real(realsig)`)
