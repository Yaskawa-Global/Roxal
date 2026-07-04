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

The main execution loop is `VM::execute(TimePoint deadline)`, which processes bytecode instructions until one of the following conditions:
- The outermost frame returns (`OpCode::Return`)
- A runtime error occurs
- The deadline is reached (returns `ExecutionStatus::Yielded`)
- An exit is requested

The deadline parameter enables incremental execution for real-time integration, where the VM can be run for a bounded time period and then yield control back to the caller with its state preserved for later resumption.


## Calling Convention

The VM is stack-based. All function/method calls follow a push-args-then-call
pattern, but the details differ between Roxal closures and native (C++) functions.

### Roxal Closures

**Caller** (bytecode emitted by the compiler):
1. Push the **callee** value (closure or bound method) onto the stack
2. Push **arguments** left-to-right
3. Emit `OpCode::Call` (or `OpCode::Invoke` for method calls)

Stack before call: `[...][callee][arg0][arg1]...[argN-1] ← stackTop`

**`call(ObjClosure*, CallSpec)`** (VM.cpp):
- Handles named-arg reordering, default parameters (including closure-evaluated
  defaults pushed via temporary `defValFrames`), and variadic arg collection
- Creates a `CallFrame` with `slots` pointing at the callee slot:
  `slots = stackTop - argCount - 1`
- The callee slot (`slots[0]`) serves as `this` / the closure value; locals
  start at `slots[1]`
- Pushes the frame onto `thread->frames` and returns `true`
- The dispatch loop continues executing the callee's bytecode

**`opReturn()`** (on `OpCode::Return` / `OpCode::ReturnStore`):
- Pops the return value from the stack
- Closes upvalues for the returning frame
- Pops the frame from `thread->frames`
- Pops all values from `stackTop` down to `frame.slots` (inclusive), which
  removes the callee slot and all locals/temporaries
- Returns the result value

Back in the `Return` opcode handler, the result is pushed onto the stack.
Net effect: the entire call footprint (callee + args + locals) is replaced by
one result value.

### Native (C++) Functions

Native functions are registered as `NativeFn` (a `std::function`) and wrapped
in `ObjNative` (standalone) or `ObjBoundNative` (method with receiver). They
are dispatched through `callNativeFn()`.

**Key difference from closures:** no `CallFrame` is pushed. The native
function executes inline within `callNativeFn` and returns a `Value` directly.

**`callNativeFn(fn, funcType, defaults, callSpec, includeReceiver, receiver, ...)`:**

*Typed path* (when `funcType` is non-null):
1. Scan original args on the stack for params needing async user-defined
   conversion (via `needsAsyncConversion()`). If found, defer the native call
   via `NativeParamConversionState` (see below).
2. Otherwise, `marshalArgs()` copies arguments from the stack into a local
   buffer, reordering named params, applying defaults, and performing sync
   builtin type conversions via `toType()`
3. The native `fn` is called with an `ArgsView` into that local buffer
4. Cleanup: `*(stackTop - argCount - 1) = result; popN(argCount);` — writes
   the result into the callee slot, then pops the args

*Untyped path* (when `funcType` is null):
1. An `ArgsView` is constructed pointing directly into the stack:
   `base = stackTop - argCount - (includeReceiver ? 1 : 0)`
2. The native `fn` is called with this view
3. Same cleanup pattern as the typed path

In both cases, the callee+args footprint is replaced by the result, matching
the net stack effect of a Roxal closure call.

### Async Parameter Conversion for Native Functions

When a native function has typed parameters and an argument is an object/actor
with a user-defined conversion operator (e.g., `print(obj)` where print
declares `value:string` and the object has `@implicit operator string()`),
the conversion requires executing Roxal code. Since `callNativeFn` can't
re-enter the dispatch loop, it uses a deferred call pattern via
`NativeParamConversionState` (in `Thread.h`):

1. `callNativeFn` scans original args against param types using
   `needsAsyncConversion()` — checks for `findConversionMethod()` matches
   or constructor auto-conversion eligibility
2. If async params found: marshals args (skipping conversion for async params),
   pushes state onto `nativeParamConversionStack`, pushes a `NativeContinuation`
   with `onComplete = processNativeParamConversion`, and pushes the first
   conversion frame via `pushParamConversionFrame()`
3. Each conversion frame returns to `processNativeParamConversion()` which
   stores the converted value in the args buffer and pushes the next
   conversion frame (or calls the native when all conversions are done)
4. After the native returns, the original callee+args are cleaned up

This is the same pattern as `NativeDefaultParamState` (for closure-evaluated
default parameters) — both defer the native call until async pre-call work
completes.

All continuation states are stack-based (vectors), supporting arbitrary nesting.
For example, `print(obj)` inside an `operator->string` body triggers nested
param conversion — the inner conversion pushes its own state onto the stacks
without clobbering the outer's.

Note: for `@builtin` functions declared in `.rox` files, the compiled closure's
bytecode (including parameter conversion opcodes) is never executed — the
native implementation runs via `builtinInfo`. The `funcType` must be provided
explicitly when registering the builtin via `addSys` / `defineNative` for async
parameter conversion to work.

### Parameter Conversion at `frameStart`

All parameter type conversion and const-freezing for Roxal closures is handled
at runtime in `frameStart` (the `if (thread->frameStart)` block in the dispatch
loop), not via compiler-emitted opcodes. When a new frame begins execution,
`frameStart` scans `funcType->func->params` and for each typed parameter:

1. **Future pass-through**: If the value is a future whose promised type matches
   the param type, it passes through without resolution.
2. **Async conversion check**: If the value needs a user-defined conversion
   (operator→T or @implicit constructor), it's queued for async handling via
   `ClosureParamConversionState`.
3. **Sync conversion**: Builtin type coercions (e.g., string→int) are applied
   in-place to the frame's param slot via `toType()`.
4. **Object/Actor type check**: For user-defined target types, the type name is
   resolved from the function's module vars and checked via `Value::is()`.
5. **Const-freezing**: After all conversions complete, params with
   `type->isConst` are frozen via `createFrozenSnapshot()`. This covers both
   explicit `const` params and implicit actor method const (isolation boundary).

For async conversions, `ClosureParamConversionState` is pushed onto
`closureParamConversionStack` with the target frame depth and param indices
needing conversion. Conversion frames are pushed one at a time via
`pushParamConversionFrame()`, and `processClosureParamConversion()` routes
each result into the target frame's param slot. Const-freezing runs after all
async conversions complete.

### Parameter Conversion Strict Context

Argument conversion conceptually happens at the call site, in the caller's
lexical scope. `frameStart` uses `frame->callerStrict` (the caller's lexical
strict setting) rather than the current frame's strict flag. This means a
non-strict caller can pass `"2"` to a strict function's `int` parameter — the
string-to-int conversion is evaluated in the caller's non-strict context.

`callerStrict` is set on `CallFrame` during frame push from the calling frame's
`strict` flag. The `implicit` modifier on a conversion method is stored as
`ObjFunction::isImplicit` (and mirrored on `ObjObjectType::Method::isImplicit`),
read by `findConversionMethod()` to gate implicit invocation.

### Return Type Conversion

When a function has a declared return type (`-> T`), the compiler emits
`ToType` / `ToTypeSpec` before `OpCode::Return` in `visit(ReturnStatement)`.
This uses the callee's strict setting (the function's own context). The same
conversion is emitted for expression-body lambdas in `visit(Function)`.

Skipped for: procs (no return value), initializers (return `this`), and
conversion operators (`operator->T` — the operator IS the conversion, emitting
a conversion on its return would be redundant or recursive).

### Constructor Auto-Conversion

Constructors are **explicit by default** — a 1-argument `init` is not eligible
for auto-conversion unless marked with the `implicit` modifier. This matches
modern language conventions (C++ recommends `explicit` on single-arg
constructors; Rust and Swift have no implicit constructors).

Auto-conversion eligibility is checked in `tryConvertValue()`. The `implicit`
flag is stored on each method as a bit in the `ast::MethodModifiers` bitset
(see Object & Actor types: Method modifiers).

### Native Functions with Continuations

When a native function needs to execute Roxal code iteratively (e.g.,
`list.map()` calling a predicate for each element), it cannot re-enter
the dispatch loop. Instead it uses the `NativeContinuation` mechanism:

1. The native pushes a `NativeContinuation` onto `nativeContinuationStack`
   (state + `onComplete` callback) and sets `resultSlotIndex` / `stackBaseIndex`
   for stack cleanup (stored as indices, not pointers, to survive reallocation)
2. It calls `pushContinuationCall()`, which pushes closure + args and calls
   `call()`, creating a new frame marked `isContinuationCallback = true`.
   The continuation's `callbackFrameDepth` is set to the current frame depth.
3. The native returns a dummy value — `callNativeFn` detects that new frames
   were pushed (`thisCallPushedFrames`) and skips the normal callee+args
   cleanup, since the continuation frames sit on top of that area
4. If the continuation did not set a `resultSlotIndex`, `callNativeFn` fills one
   in automatically to cover the original callee+args area
5. The dispatch loop executes the Roxal callback naturally
6. When the callback returns, `opReturn` sets `continuationCallbackReturned`
7. `processContinuationDispatch()` pops the result, calls `onComplete`, and
   either pushes the next iteration's frame or finalizes: pops the original
   call's footprint (via `resultSlotIndex` / `stackBaseIndex`) and pushes the
   final result

Continuations nest correctly — e.g., a `list.map` callback can itself call
`list.filter`. `processContinuationDispatch` uses `callbackFrameDepth` to
distinguish "this continuation pushed another iteration frame" (frame depth
matches) from "an outer continuation's callback frame is on top" (shallower
depth = this continuation is done). The `NativeDefaultParamState` and
`NativeParamConversionState` handlers do not call `clearContinuation()`
themselves — `processContinuationDispatch` handles popping the continuation
stack after `onComplete` returns.

On exception unwind, `unwindFrame()` detects continuation callback frames and
extends the pop range to include the original call's footprint using
`resultSlotIndex`, then pops the continuation stack.


## Types & Values

Runtime values in the language are represented by the Value class, which wraps a 64bit value.  This holds builtin primitives (`bool`, `int`, `real`, `decimal`, `enum`) and references to reference types.  The implementation uses NaN-boxing, whereby the full 64bit are used as a C double for the type `real`, but if the Quiet NaN (Not-a-Number) flags are set, then, it is instead assumed to be one of the other types, as stored in the type tag.

These by-value types can be tested via the various is*() Value methods (`Value::isBool()`, `isInt()`, etc).

In the case of reference types (`list`, `dict`, `vector`, `matrix`, `signal`, user-defined objects & actors), the `Value` only indicates the builtin type, or that is is an Object or Actor for user-defined types.  For enums, the Value holds the enum numeric value and a typeid value corresponding to a global registry of enum type information.  The reference is to an instance of class `Obj`.  Value manages reference counting for `Obj` references (via `incRef()` and `decRef()`).

The reference types are implemented in `Object.h`|`cpp`.  The `dict` type uses an STL `std::map` of Values; the `list` type uses an STL `std::vector` of Values, or a packed `std::vector<uint8_t>` when it holds only bytes (see the ObjList notes below).

The `vector`, `matrix` and `tensor` types utilize the Eigen library.  Although these are reference types, the intention is that they behave like value types (- the current implementation is a mixture - operations create new values, but they're passed by reference and assigning elements mutates)

## Scopes

Each `.rox` file is a module by default, even if not declared as such (according to the filename).  Within a module, module-scoped variables can be used without forward declarations - references by name are resolved at runtime.

Within a function or method, parameters and variable or function declarations are local.  These are access via via offsets from the function's execution frame pointer.

Functions are first-class values and can capture variables from outer scopes, yielding a closure (`ObjClosure`), which encapsulates the function's static code (`Chunk`), and captures upvalues.  Upvalues initially refer to stack entries of enclosing function scopes, but are 'lifted up' into the heap as required before the original stack positions are unwound.


## Object & Actor types

A new object type (like a C++ class) can be declared and have its own methods (`func` or `proc`) and member variables.  Members can be declared private, in which case they're not accessible outside the scope of the type's methods.

An actor type is similar to an object, but additionally has its own thread of execution associated with it.  This thread is the only thread that can execute the actor's methods. Hence, when another thread (e.g. the main script thread or another actor's thread) calls an actor instance's methods, a future for the return value (if any) is immediately returned to the caller, which can continue to execute asynchronously.  Only if that future needs to be converted into the return value will the execution block, if necessary, until the called actor's method has completed and returned to provide the value.  Hence, execution of methods within an actor are serialized, since there is only one thread, so that developers need not worry about shared state between multiple threads.

Complex reference types passed to an actor's method (or returned from it) behave as if deep-copied (cloned).  In practice they use Multi-Version Concurrency Control (MVCC), as discussed below, to avoid actually copying.

### Method modifiers

Methods can carry zero or more compiler-recognised modifiers stored as a bitset
(`ast::MethodModifiers`, defined in `core/AST.h`):

- `Implicit` — set by the `implicit` keyword. Allows the method (typically a
  `init` constructor or `operator T()` conversion) to be invoked implicitly
  during type coercion. See *Constructor Auto-Conversion* and `isImplicitMethod()`
  in `VM.cpp`.
- `StatementAction` — set by the two-word `statement action` modifier.
  Designates the method as the type's *statement-action handler* (see next
  section).

The same bitset lives in three layers — AST (`core::ast::Function::methodModifiers`),
the static type system (currently still pair-based in `core/types.h` with the
modifier carried in the AST), and runtime metadata
(`ObjObjectType::Method::methodModifiers` in `compiler/Object.h`). The runtime
representation is the load-bearing one for dispatch.

### Statement Action

A method on an object/actor type marked with the `statement action` modifier is
invoked automatically by the VM whenever an instance of that type appears in
expression-statement position (i.e. the discarded value of an `expr_stmt`).
This drives builder-style domain APIs (e.g. a robotics `Motion` type whose
construction is cheap and whose `execute()` is the action triggered by writing
the motion as a statement).

**Compile-time validation** (in `defineMethod` / `VM.cpp:~4457`):
- At most one `statement action` per type. Inheriting types must re-mark the
  override.
- Cannot be combined with `private`.
- Method takes no parameters beyond `self` (`arity == 0`).

The hash of the method name is cached on the type as
`ObjObjectType::statementActionMethodHash` so the runtime hot path does not
need to scan the methods map.

**Codegen.** `RoxalCompiler::visit(ast::ExpressionStatement)` emits
`OpCode::StmtAction` instead of `OpCode::Pop` for non-assignment expression
statements. Assignments still emit `OpCode::Pop` because their RHS-leftover
value is a calling-convention artefact, not a meaningful "statement value", so
auto-trigger would fire on values like `next` in `this.nextStep = next`.

**`OpCode::StmtAction` runtime semantics.** The opcode peeks the stack top and
dispatches:

- `nil` or any value with no statement-action method on its type → pop and
  exit.
- An `ObjectInstance` / `ActorInstance` whose type (walking supertype chain)
  has a `statement action` method → invoke it. Re-fires the opcode after the
  call returns so the method's return value is itself processed (chaining).

A *future* at expression-statement position falls through to the terminal
pop. This is intentional: the future's local refcount drops, the actor's
underlying `returnPromise` continues independently. An earlier design
auto-awaited futures here (treating them as having an implicit
"statement-action of await") but was reverted — that policy was inconsistent
with unused futures held by ordinary locals (which already silently
fire-and-forget at scope exit) and forced motion-style APIs that prefer per-call
`wait=true/false` parameters into a global rule that didn't fit. See
`project_stmt_action_reverted_future_await.md` in the project memory.

**Per-thread session stack.** Statement-action sessions can nest: the action
method invoked by an outer `StmtAction` may itself contain expression
statements with their own `StmtAction` opcodes. Iter-counter and last-receiver
state therefore live on a per-thread stack
(`Thread::stmtActionStack`), keyed by `(opcodeIp, frameDepth)`. Re-entries at
the same key continue the same session; entries at different keys push new
sessions; stale sessions whose `frameDepth` exceeds the current frame depth
(left over from inner sessions that errored without popping) are pruned on
entry.

**Termination protections.** Each session has an iteration cap
(`Thread::kStmtActionIterCap = 1024`) and a same-instance cycle check that
fires immediately if a method's return value is reference-equal to the
previous receiver (catches the common `return this` mistake with a clear
diagnostic). Indirect cycles still rely on the iter cap.

**Method invocation pattern.** The opcode rewinds `frame->ip` to its own
`instructionStart` *before* calling the action method via `call(ObjClosure*,
CallSpec(0))`. The receiver already sits at `peek(0)` and becomes slot 0
(`self`) of the new frame per the standard method-call convention. When the
called frame returns, dispatch resumes at the rewound IP and re-fires
`StmtAction`, observing the return value as the new top.

**GC.** The session stack's `lastReceiver` Values are visited by
`SimpleMarkSweepGC.cpp` so the receiver's `Obj*` address remains stable
across the call (otherwise GC could free and reallocate at the same address,
producing a false cycle match).

**`ignore(value)` builtin.** Registered in `compiler/ModuleSys.cpp`
alongside `wait()` with an untyped (`std::nullopt`) parameter so the
argument bypasses the implicit-coercion path in `OpCode::ToTypeSpec`. The
builtin's only effect is its strict argument check: it raises a runtime
error unless the value is a future, an instance with a `statement action`
method, or `nil`. Nil is accepted silently to tolerate the actor-proc case
(procs return `nilVal` directly, so `ignore(actor.someproc())` shouldn't
break).


## Function and Method Overloading

A name (function, proc, object/actor method, or interface method) may be
declared multiple times in the same scope when the parameter signatures
distinguish each declaration. Roxal discriminates only by positional
parameter types and arity (NOT named-arg names); two declarations with
identical positional signatures and arity are an error.

The feature spans four runtime data structures, the `OverloadResolver`
class (compile-time and runtime ranker), four new bytecode opcodes, and
small surgical changes to TypeDeducer and call-site emission. A name with
exactly one declaration takes the existing fast path and pays no overhead.

### Data structures

#### `ObjOverloadSet` — runtime overload set for functions and locals

Defined in `compiler/Object.h`. A heap-allocated `Obj` subclass:

```cpp
struct ObjOverloadSet : public Obj {
    icu::UnicodeString  name;            // for diagnostics
    std::vector<Value>  closures;        // each is an ObjClosure*
    bool                importedFromModule = false;
    void  add(const Value& closure);
    Value asSingle() const;              // for the size==1 fast path
};
```

`ObjType::OverloadSet` is its enum tag. From `Obj::valueType()` it returns
`ValueType::Closure`, so all existing `isClosure`-style first-class
predicates still work — `g = foo` (foo overloaded) gives a normal-looking
"function" value to the user. `objTypeName` returns "function".

Lifetime:

* Constructed at module init by `OpCode::DefineModuleOverload` (module
  scope) or `OpCode::DefineLocalOverload` (local scope). Stored in the
  module's `vars` map or the function frame's local slot.
* `bindMethod` allocates an OverloadSet on the fly when binding an
  overloaded method — these have no other strong root, so the BoundMethod
  ctor stores a *strong* ref to the OverloadSet (vs the usual weak ref to
  closures, which are strong-rooted by the type's method map).
* Trace walks `closures`. Not serializable as a Value (overload sets are
  rebuilt by opcodes on each module init); `write`/`read` throw
  defensively.

#### `MethodOverloadSet` and `MethodInfo` — methods on object/actor types

Defined inside `ObjObjectType` in `compiler/Object.h`:

```cpp
struct Method {
    icu::UnicodeString    name;
    Value                 closure;
    ast::Access           access;
    ast::MethodModifiers  methodModifiers;
    Value                 ownerType;       // weak ref
};
struct MethodOverloadSet { std::vector<Method> overloads; };
std::unordered_map<int32_t, MethodOverloadSet> methods;
```

The map is keyed by name hash. A name declared once still goes through
this structure, with `overloads.size() == 1`. Two helpers gate access by
call sites that pre-date overloading:

* `findUniqueMethod(hash)` — returns the single overload's `Method*` if
  exactly one exists, else nullptr. Used by getter/setter synthesis,
  statement-action lookup, operator dispatch, builtin modules — sites
  that never need to discriminate by signature.
* `firstOverload(hash)` — returns the first overload of any size set, or
  nullptr. Used by chain walks looking for "is the name declared on this
  type at all?" (init lookup, BoundMethod construction, remote-actor
  binding).

The compile-time analog lives in `core/types.h` on
`type::Type::ObjectType`:

```cpp
struct MethodInfo {
    icu::UnicodeString  name;
    ptr<FuncType>       funcType;
    uint8_t             methodModifiers;   // bits match ast::MethodModifier
    Access              access;
};
std::vector<MethodInfo> methods;
```

Populated by TypeDeducer when it visits a TypeDecl. Carries the
parameter-list FuncType (so the resolver can rank candidates statically),
plus modifiers and access — used by `OverloadResolver` for proper
implicit-conversion detection (`implicit operator->T`,
`implicit init(S)`) without re-walking the AST.

#### `OverloadResolver` — the ranker

Defined in `compiler/OverloadResolver.{h,cpp}`. Class with nested types:

```cpp
class OverloadResolver {
public:
    struct ArgInfo {
        ptr<type::Type>  type;       // nullptr = unknown at compile time
        bool             isNamed = false;
        int32_t          nameHash = 0;
    };
    struct Candidate {
        ptr<type::Type>  funcType;   // FuncType (params + returns)
        Value            target;     // ObjClosure*; nilVal at compile time
        bool             isMethod;   // affects 'this' arity bookkeeping
    };
    struct Score {
        bool      feasible          = false;
        uint32_t  totalRank         = 0;   // sum of per-arg ArgRank values
        uint16_t  defaultsActivated = 0;   // tie-breaker
    };
    struct ResolveResult {
        enum Kind { ResolvedUnique, Ambiguous, NoMatch, NeedsRuntime };
        Kind                  kind;
        uint16_t              chosenIndex;
        std::vector<uint16_t> tiedIndices;
    };

    explicit OverloadResolver(VM* vm = nullptr);
    ResolveResult resolve(const std::vector<Candidate>&,
                          const std::vector<ArgInfo>&,
                          bool staticDispatchAttempt,
                          bool strictMode);
    Score scoreOne(const Candidate&, const std::vector<ArgInfo>&, bool strict);
    static bool isBetter(const Score& a, const Score& b);
    std::string ambiguityDiagnostic(...);
    std::string noMatchDiagnostic(...);
    static bool signatureCompatibleForOverride(const ptr<type::Type>& abstract,
                                               const ptr<type::Type>& concrete);
    static std::string signatureToString(const icu::UnicodeString&,
                                         const ptr<type::Type>&);
};
```

The same instance is used for both compile-time resolution (`vm_ ==
nullptr`, args may have nullptr types) and runtime resolution (`vm_ !=
nullptr`, args carry types derived from actual Values via the free
helper `valueRuntimeType`).

Indices are `uint16_t` for portability and to keep `ResolveResult`
compact. The struct is in-memory only — never serialized.

### Per-arg ranking

`scoreOne` classifies each argument against its candidate parameter and
sums rank values into `Score::totalRank`. Lower rank = better match.

| Rank | Name | Triggered by |
|---|---|---|
| 0 | Exact | builtin equality, or same `ObjectType::name` |
| 1 | Subtype | arg's object/actor type chains via `extends`/`implements` to param's name |
| 2 | StrictImplicitConv | `type::convertibleTo(from, to, /*strict=*/true)` — safe widening (`byte→int`, `int→real`) — valid in any context |
| 3 | Untyped | param has no declared type — wildcard |
| 4 | NonStrictImplicitConv | `convertibleTo(from, to, /*strict=*/false)` minus strict cases — only feasible when the call site is non-strict |
| 5 | UserDefinedImplicitConv | one side is object/actor; `userDefinedImplicitConvFeasible` checks for `implicit operator->T()` on the source or `implicit init(S)` on the target. Permissive when type info is incomplete; runtime `tryConvertValue` is the final gatekeeper |
| 6 | VariadicAbsorb | arg consumed by a `...args` variadic param |

`isBetter(a, b)` is `feasible > !feasible`, then lower `totalRank`, then
lower `defaultsActivated`. Equal scores → tied → ambiguity error.

### Hybrid dispatch

`visit(Call)` consults per-scope `localOverloadCandidates` /
`moduleOverloadCandidates` maps populated by pre-passes in `visit(File)`
and `visit(Function)`. When all arg types are TypeDeducer-known AND the
resolver returns `ResolvedUnique`, the compiler emits a direct opcode
encoding the chosen overload index — runtime does zero dispatch work:

* **Functions/local funcs** — `OpCode::GetOverloadAt <name-const>
  <overload-index>` or `GetLocalOverloadAt <slot> <overload-index>`,
  followed by args and the existing `Call`.
* **Object/actor methods** — `OpCode::InvokeOverloadAt <name-const>
  <overload-index> <CallSpec>`. Runtime walks the receiver's type chain
  to the named method's overload set and calls `overloads[index]`
  directly.

When some arg types are unknown OR the resolver returns `NeedsRuntime`
(another candidate could be promoted by the unknown), the compiler emits
the existing `GetModuleVar`/`GetLocal` + `Call` (or
`GET_PROP_CHECK + CALL` for methods) sequence — the OverloadSet flows
to `VM::callValue`'s OverloadSet branch (or `VM::invokeFromType` for
methods) which dispatches via the same resolver against actual stack
values. `valueRuntimeType` synthesizes a minimal `type::Type` from a
runtime Value.

`NoMatch` and `Ambiguous` results with all types known surface as
compile errors; otherwise they become runtime errors with a candidate
listing rendered by `signatureToString`.

The compile-time path's reach depends on TypeDeducer's coverage. Three
narrow improvements landed alongside this work:

* Properties typed with a builtin (`var x :int`) or a user TypeName
  (`var engine :Engine`, resolved by `lookupVar` during the TypeDecl
  visit) populate `PropType::type`.
* `visit(UnaryOp)` for the Accessor case propagates the property type
  to the accessor expression — enabling chains like
  `obj.prop.method(args)`.
* `MethodInfo` carries the full FuncType param list (vs the previous
  `pair<name, isProc-only-FuncType>`).

### Cross-module imports

`OpCode::ImportModuleVars` clones any imported `ObjOverloadSet` and tags
the clone with `importedFromModule = true`. A subsequent local
`DefineModuleOverload` for the same name discards the imported set
rather than appending — local declarations take precedence; no
cross-module merging.

### Interface conformance

`VM::checkInterfaceConformance` iterates per-overload of each abstract
method on the interface (and any extended interface). For each abstract
overload signature, the implementer chain must contain a concrete
overload that passes `OverloadResolver::signatureCompatibleForOverride`
— invariant parameter types (same builtin, same `ObjectType::name`) and
a covariant return type (same builtin; for object/actor returns,
accepted permissively here since the compile-time
`type::Type::ObjectType.extends` chain isn't reliably populated on a
FuncType return ref — runtime type-assignment at the call site enforces
the actual subtype safety).

The diagnostic distinguishes "missing method 'X'" (no overload at all)
from "missing method overload 'X(types)'" (some overloads exist but
this signature isn't satisfied).

### Bytecode opcodes added

| Opcode | Operands | Effect |
|---|---|---|
| `DefineModuleOverload` | name-const | pop closure; create or append to module-scope OverloadSet under name |
| `GetOverloadAt` | name-const + 2-byte index | push module OverloadSet's `closures[index]` directly |
| `DefineLocalOverload` | slot | pop closure; first call wraps the slot's closure in a fresh OverloadSet, subsequent calls append |
| `GetLocalOverloadAt` | slot + 2-byte index | push local-slot OverloadSet's `closures[index]` |
| `InvokeOverloadAt` | name-const + 2-byte index + CallSpec | walk receiver chain to find method, call `overloads[index]` directly |

### Limitations / future work

* Object→object return-type covariance in interface conformance is
  permissive (admit-and-trust); a stricter check requires threading the
  runtime `ObjObjectType` chain through to the resolver.
* Statement-action methods with overloaded names: the existing per-type
  invariant ("at most one statement-action method") is preserved; the
  semantics of multiple overloads where some are statement-action are
  not yet pinned down.
* Operator method overloading (multiple `operator+` on the same type):
  out of scope for the current implementation. The existing
  `tryDispatchBinaryOperator` keeps its bespoke parameter-type check
  using the first overload; refactoring it to call
  `OverloadResolver::resolve` over an operator overload set is a clean
  follow-up — designed in the plan as Phase 5 (operator overloading).
* Cross-thread actor invocation through a BoundMethod wrapping an
  OverloadSet substitutes the resolved closure into the BoundMethod
  before queueing — adequate but not the cleanest factoring; a small
  refactor could route via a fresh BoundMethod per call.


## Futures

When a non-proc actor method is called from another thread, the caller receives
an `ObjFuture` wrapping a `std::shared_future<Value>`. The actor thread fulfils
the underlying promise when the method completes. Some native builtins (file IO,
sockets, gRPC, neural network inference) also return futures for non-blocking
operation.

### Promised Type

Each `ObjFuture` stores a `promisedType` (`ptr<type::Type>`) indicating the type
of the value it will resolve to. This is extracted from the actor method's
declared return type (via `funcType->func->returnTypes[0]`) at future creation
time in `ActorInstance::queueCall()`. Native builtins can also supply a promised
type via `Value::futureVal(future, promisedType)`. When the type is unknown
(nullptr), the future is treated conservatively at typed boundaries.

### Resolution Rules

Futures are resolved (awaited) lazily — only when a concrete value is needed:

- **Typed function parameters:** If the future's promised type matches the
  parameter type (identity or subtype via `isSubtypeOf`), the future passes
  through without resolution. Otherwise it is resolved first, then converted.
  This is checked in the `Call` opcode handler, `marshalArgs`, `frameStart`
  parameter conversion, and the `ToType`/`ToTypeSpec` handlers.
- **Untyped parameters:** Futures pass through as-is.
- **Operators, conditions, iteration, property access:** The VM resolves futures
  at the point of use — binary ops, `JumpIfFalse`/`JumpIfTrue`, `IfDictToKeys`,
  `Invoke`, `SetProp`, `SetIndex`, `Throw`, etc. all call `tryAwaitFuture()` or
  `tryAwaitValue()` before operating on the value.
- **Explicit casts:** `T(future)` resolves the future (like signal sampling).

### Non-blocking Awaiting

Resolution never blocks the C++ dispatch loop. `tryAwaitFuture()` checks if the
future is ready (zero-wait `wait_for`). If not:
1. The thread registers as a waiter on the future (`ObjFuture::addWaiter`)
2. `thread->awaitedFuture` is set and the instruction pointer is rewound
3. The dispatch loop yields to `postInstructionDispatch` which sleeps on the
   thread's condition variable (1ms polling fallback)
4. When the promise is fulfilled, `ObjFuture::wakeWaiters()` signals waiting
   threads
5. On the next loop iteration, the future is ready and resolved in-place

This allows the VM to process events, respect `execute(deadline)` deadlines, and
yield control back to the caller during the wait.

### Actor Return Resolution

Actor methods always resolve any futures in their return value before fulfilling
the caller's promise (`Thread.cpp`, in the `act()` return paths). This ensures
the promise value is always concrete — the caller's future wraps a resolved
value, not a nested future. The caller's future gets its own `promisedType` from
the method's declared return type.

### resolveReturn Flag

`BuiltinFuncInfo` has a `resolveReturn` flag. When set, `callNativeFn` triggers
non-blocking resolution of the returned future before the caller resumes. This
allows native functions to use futures internally for non-blocking IO while
presenting a synchronous API to the user (e.g., file `close()`).


## Combinators: `sys.allof` / `sys.anyof`

`sys.allof(...items)` and `sys.anyof(...items)` await multiple things at once.
Inputs may be futures, event types, or bool signal expressions (`c > 20`),
freely mixed. `allof` resolves to a list of values when all inputs resolve;
`anyof` resolves to `{"index": i, "value": v}` when the first input resolves.

A "combinator" here is an `ObjCombinator` (`Object.h`): a small runtime object
that owns a `std::promise<Value>`, a list of slots, and a mode (`All` / `Any`).
The promise's `shared_future` is wrapped in an `ObjFuture` returned to user
code — the combinator is *itself a future*, so it can be passed to
`wait(for=...)`, fed into another `allof`/`anyof`, or used anywhere a future
is accepted. Composability falls out for free.

### Slot wiring

Each input awaitable becomes one `Slot`. Wakeup wiring depends on the kind
(`wireCombinatorSlot` in `ModuleSys.cpp`):

- **Future slot:** the combinator registers as a waiter on the input
  `ObjFuture` via `addCombinatorWaiter` (a weak Value ref + slot index).
  `ObjFuture::wakeWaiters` already runs after `set_value`; we extended its
  `waiters` vector to a `Waiter` variant (Thread *or* Combinator, in
  `Object.h`) so combinator wakeups go through the same path.
- **Event-type slot:** registers a one-shot `HandlerRegistration` on the
  calling thread whose closure wraps a sentinel ObjFunction
  `__combinator_relay` (kept on `VM::combinatorRelayFunction`, alongside
  `__conditional_interrupt`). The closure is per-registration so its
  `handlerThread` is correct, but identity is checked by underlying
  ObjFunction so dispatch can recognise the relay regardless of which
  closure carries it.
- **Bool signal slot:** identical to event-type slot but uses the signal's
  change event (`ensureChangeEventType`) with a `becomes`-filter
  (`matchValue = trueVal`). Reuses the existing signal/event filtering in
  the dispatcher — no grammar additions needed for `c > 20` to work as an
  awaitable, since signal comparisons already produce derived bool signals.

When the dispatcher (`processPendingEvents` / `invokeNextEventHandler`) sees
a relay closure, it routes to `ObjCombinator::notifySlotReady` instead of
running user code. `notifySlotReady` is idempotent under a mutex:
- **Any** mode → first slot wins, builds the `{index, value}` dict, fulfils
  the promise, calls `cancel()`.
- **All** mode → decrements `pendingCount`; on zero, builds the value list,
  fulfils, calls `cancel()`.
- An `isException` value short-circuits both modes (forwards the exception
  through the output future).

After fulfilling, `notifySlotReady` calls `wakeWaiters` on the *output*
ObjFuture (held weakly via `ObjCombinator::outputFuture`). Without this,
nested combinators wouldn't propagate — the outer's future-slot waiter
relies on this wake-up.

### Lifetime and cleanup

The combinator is kept alive while its output future is reachable: the
output `ObjFuture` has a `producer` field (`Object.h`) that holds the
combinator strongly. Conversely, slots hold their input awaitable strongly,
plus the per-registration relay closure for event/signal slots, so the
inputs aren't reclaimed mid-flight.

Subscriptions are cleaned up two ways so long-running programs don't
accumulate dead registrations:

1. **Fire-time cleanup.** When the dispatcher fires a relay, the matching
   `oneShot` HandlerRegistration is removed from `thread->eventHandlers`
   and the matching weak entry is dropped from `evt->subscribers`. Safe
   because dispatch runs on the registering thread.
2. **Prune-time cleanup.** `Thread::pruneEventRegistrations`, run after
   every GC, additionally removes any HandlerRegistration whose
   `combinatorTarget` weak ref is dead or whose combinator is `fulfilled`
   (and drops the matching subscriber entry). This catches the
   never-fires-again case — e.g. an `AbortRequested` that was a losing
   slot in an `anyof` and is no longer needed.

`cancel()` itself just drops the slot's strong refs (input + relay
closure). It is callable from any thread; the cross-thread cleanup of
event-handler maps deliberately runs only on the registering thread via
the pathways above, avoiding any need for a mutex on `eventHandlers`.

### Exception forwarding from actors

Actor methods that `raise` had previously aborted the entire VM
(via `runtimeError`); for combinators (and `wait(for=fut)` generally) to
catch the exception on the awaiting thread, the actor must forward the
exception through its return future instead.

`raiseException` (and the inline `OpCode::Throw` exception path) save the
unwound exception in `Thread::pendingUncaughtException` before deciding
what to do. If the thread is an actor thread inside a method invocation
(`isActorThread() && currentActorCall.isNonNil()`), we skip the global
`runtimeErrorFlag` and just `resetStack()`. The actor's main loop sees the
failed `execute()` result, picks up the saved exception value, and
fulfils the return promise with it (Roxal exceptions travel through
futures as plain `Value`s passing `isException(v)` — no `set_exception`).
The actor stays healthy and continues serving subsequent calls.

Awaiting code resolves the future, sees `isException`, and re-raises via
`vm.raiseException`. The wait dispatcher's future-resolved-with-exception
path was also fixed to *not* prematurely return `errorReturn` so the
handler frame state set up by `raiseException` actually runs.


## Signals and Data-Flow

The VM includes a data-flow engine (in `Dataflow/`) that can represent a set of signals (`Signal` & `ObjSignal`) of Values that interconnect as inputs and outputs to function nodes (`FuncNode`).
The dataflow engine will updates signal values as they are effected by changes to other signals via functions.  There exists a special builtin function `clock(freq)` that creates a native signal that counts up at the specified frequency.

Function nodes wrap standard functions (`func`) and execute their `Chunk` code (via a `Closure`).

The data flow engine is represented as a builtin actor instance.  Hence, the evaluation of all functions (`FuncNode`s) happens on the dataflow engine's actor thread.

Signals can be sampled to yield their current value at any time on any thread, either via the builtin `value` property, or by using them to construct their underlying value type (e.g. `vector(vecsignal)`, or `real(realsig)`)

A signal's time->value history map (`Signal::values`) is written by the engine
thread (ticks), script threads (`set`) and the DDS reader-signal thread, and
read by sampling threads and GC tracing, so it is guarded by a per-signal
recursive mutex (`m_valuesMutex`).  Locking discipline: where both are held,
the engine's `m_mutex` is taken first, then the signal mutex; change
callbacks and `DataflowEngine` notifications are always invoked *outside*
the signal mutex (they can be arbitrarily heavy and take the engine mutex
themselves).

## DDS Module (ModuleDDS)

`import dds` exposes CycloneDDS pub/sub (`compiler/dds/`).  IDL files are
parsed with libidl (`DdsAdapter`, which splices `#include`s itself since the
mcpp preprocessor lives in the idlc binary, not the library) into `StructInfo`
/ `FieldType` descriptions, from which Roxal object types are generated.  The
`@ros` import annotation applies ROS 2 (rmw_cyclonedds) wire-name mangling
(`pkg::msg::Type` -> `pkg::msg::dds_::Type_`).

Topic creation builds a complete static-style `dds_topic_descriptor_t` at
runtime (`ModuleDDS::buildTopicDescriptor`): the `m_ops` marshalling bytecode
(the same format idlc generates at compile time -- see CycloneDDS's
`dds_opcodes.h`; run idlc on an IDL and read the generated `.c` for a
reference), sample size/alignment and member offsets from `computeLayout`,
plus serialized XTypes typeinfo/typemap blobs from libidlc's
`generate_type_meta_ser`.  Because the ops offsets and the marshalling code
(`fillSampleFromValue` / `valueFromSample`) both derive from `computeLayout`,
descriptor and marshalling agree by construction.  The earlier implementation
used CycloneDDS's `dds_dynamic_type_*` API instead; that API's typelib dedup
(`dynamic_type_complete_locked`) frees just-constructed types out from under
live handles whenever the process type library is already populated -- a
use-after-free that aborts the process, fatal when embedding libroxal in a
host with statically registered types (reported upstream to Eclipse
CycloneDDS).

Reader signals (`dds.reader_signal` / `dds.ros_reader_signal`) are serviced
by one waitset-driven thread (`ModuleDDS::readerThreadLoop`): a per-reader
readcondition (level-triggered while samples remain in the reader cache) is
attached to a `dds_waitset`, and a guard condition wakes the thread for
binding changes and shutdown.  Sample delivery is QoS-aware, using the
reader's history kind queried at registration: **keep_last** readers drain
the cache and set only the newest valid sample (matching the QoS contract
and bounding pressure on the dataflow engine); **keep_all** readers get
every sample in order, one bounded batch per wake (the level-triggered
readcondition re-wakes while a backlog remains).  Note that a signal is
last-value semantics end to end -- scripts that need guaranteed
per-message processing should loop on `dds.read`/`dds.take` instead.
Sample conversion (`valueFromSample`) runs on the reader thread, off any
RT-budgeted (`runFor`) thread; handler bodies (`when sig changes`) run as
pending events on their script threads as usual.

### Supported IDL subset / future enhancements

The adapter, marshaller, and descriptor emitter must move together: a
construct is only supported once all three handle it (`DdsAdapter::classifyType`
+ `FieldType`, `ModuleDDS::buildTopicDescriptor`, and the marshal/layout
functions).  Currently supported: structs (final / appendable / mutable,
nested), bool / byte / int32 / int64 / uint64 / float64, enums, bounded and
unbounded strings and sequences (including sequences of structs and nested
collections), fixed arrays (multi-dimensional and typedef'd; prim / enum /
string / struct elements), top-level `@key`, typedefs.  Unsupported
constructs are rejected with a runtime error rather than silently
mis-encoded.  Not yet supported -- candidates for later enhancement:

- **`@optional` members** -- currently marshalled as plain required fields.
  CycloneDDS's convention stores optionals as pointers (`DDS_OP_FLAG_OPT` |
  `DDS_OP_FLAG_EXT` + a `DDS_OP_MID` member-id section); supporting it means
  pointer storage in `computeLayout`/marshalling plus nil <-> absent mapping.
- **Narrow primitives: `int16`/`uint16`, `float32` (and `wchar`,
  `long double`)** -- widened to int32/float64 in memory *and on the wire*
  (`FieldType::widened`), which diverges from the IDL; XTypes metadata blobs
  are therefore skipped for types containing them (they fall back to
  name-based endpoint matching).  Proper support = new `FieldType` kinds +
  2BY/4BY-FP ops + marshal cases.  (Tensors already support `uint16` --
  `dtype='uint16'` plus `astype(dtype, scale=)` for e.g. 16-bit depth
  images; this gap is only about DDS IDL field widths.)
- **Unions** -- ops `DDS_OP_TYPE_UNI` + `DDS_OP_JEQ4` case labels; needs a
  Roxal-side representation for the discriminator/active-member.
- **Maps** -- IDL `map<K,V>`; no `FieldType` representation.
- **Bitmasks / bitsets** -- `DDS_OP_TYPE_BMK` etc.
- **Struct inheritance** -- XTypes base types (`DDS_OP_FLAG_BASE`).
- **Wide strings** (`wstring`) -- `DDS_OP_TYPE_WSTR`/`BWSTR`.
- **`@key` on nested-struct members** (key chains) -- KOF offset chains and
  key-order rules; only top-level keys are honoured today (multi-key ordering
  follows definition order, untested against idlc's for >1 key).
- **Explicit member ids** (`@id`/`@hashid`, `@autoid(hash)`) -- mutable types
  currently assume sequential ids (matches the previous behaviour).
- **`@external`** (pointer-stored members) -- `DDS_OP_FLAG_EXT` storage.

## Serialization

Values are persisted using the `Value::write` and `Value::read` helpers, which
implement the VM's binary format.  Primitive types are written directly, while
reference types delegate to their specific `Obj` subclass implementation.  The
built‑in `serialize(value)` function returns this binary representation as a
`list` of bytes and `deserialize(bytes)` performs the inverse operation.

To retain object identity and support cycles, a `SerializationContext` is passed
through the write/read calls.  Each object pointer is assigned a unique
64‑bit identifier.  The first time an object is seen its id and full contents
are written and recorded in the context; subsequent references emit only the id
flagged as an existing instance.

Deserialization reverses this process, reconstructing objects from the id map so
that shared references and cycles are preserved.  Actor instances only persist
their declared properties—runtime queues and threads are reinitialised when the
actor is restored.  Functions and closures serialise their `Chunk` bytecode and
captured upvalues so they can be executed after being deserialised.


## Module loading, caching, and reconciliation

### Builtin modules

A C++ `BuiltinModule` subclass (e.g. `ModuleSys`, `ModuleNN`, `ModuleRegex`)
pairs a native implementation with a `.rox` "companion" script. The companion
declares the module's surface area in Roxal — top‑level functions, object
types, methods, properties — typically with `@builtin` annotations and empty
or stub bodies. The C++ side then *links* native implementations to those
declarations via `BuiltinModule::link()` (for top‑level functions) and
`BuiltinModule::linkMethod()` (for object methods). Linking sets the
`builtinInfo` field on the underlying `ObjFunction`, which the VM's dispatch
paths (`bindMethod`, `callValue`'s Closure branch) check to route the call to
the native implementation instead of executing the Roxal stub body.

Builtin modules are registered in one of two ways (`VM::VM`):

- **Eagerly** via `registerBuiltinModule(make_ptr<ModuleX>())`. Their `.rox`
  is executed during VM construction (`executeBuiltinModuleScript`) and
  `registerBuiltins(vm)` runs via `defineBuiltinFunctions()` — both happen
  before user scripts compile.

- **Lazily** via `lazyModuleRegistry.registerFactory(name, factory)`. The
  module instance, its `.rox` execution, and `registerBuiltins` all fire on
  the first `import name.*` from user code (`LazyModuleRegistry::doLoad`).
  Lazy loading is preferred for optional features so the cost is only paid
  when used.

`LazyModuleRegistry::ensureLoaded` holds a *per-module* mutex across `doLoad`
to serialize concurrent loads of the same module, but releases the
registry-wide mutex before calling `doLoad` — otherwise nested imports inside
the loading script (which re-enter the registry to resolve `import` targets)
would deadlock. `doLoad` itself never holds the registry mutex across script
execution: it re-acquires the mutex briefly for each entry mutation
(constructing the instance, marking `loaded=true`) and works against a local
`ptr<BuiltinModule>` the rest of the time.

### Native module plugins (the `qt` module)

Most native modules are compiled into `roxalcore` and so into the `roxal`
binary. The **`qt` module is different**: it builds as a separate shared object
**`libroxalqt.so`** that the binary `dlopen`s only on the first `import qt`. The
goal is a *single distributable binary that runs on machines without Qt
installed* — the `roxal` binary carries **no `NEEDED` Qt entry**; Qt (and the
plugin) are touched only when a script actually uses the UI.

How it fits together:

- **Build split** (`CMakeLists.txt`). `compiler/qt/*.cpp` compile into the
  `roxalqt` SHARED target (links `Qt6::*`, **not** `roxalcore`), output beside
  the binary. `roxalcore` no longer compiles or links any Qt. The Roxal-level
  `import` is already lazy; this just changes *where the code lives*.

- **Factory** (`loadQtPluginModule()` in `VM.cpp`). The lazy `qt` factory, on
  first import, `dlopen`s `libroxalqt.so` — searching the executable's directory
  first, then the module search paths, then the bare name (loader rpath /
  `LD_LIBRARY_PATH`) — with `RTLD_NOW | RTLD_LOCAL`, then `dlsym`s the C entry
  point and wraps the result. The handle is cached for the process lifetime and
  **never `dlclose`d** (Qt installs static state + `atexit` handlers).

- **C entry point** (`compiler/qt/QtPlugin.cpp`).
  `extern "C" roxal::BuiltinModule* roxal_qt_create_module()` returns
  `new ModuleQt()`; the core adopts it via `ptr<BuiltinModule>::from_raw` (the
  `shared_ptr` control block crosses the `.so` boundary safely, and `ModuleQt`'s
  virtual destructor dispatches the eventual `delete` back into the plugin).

- **Host-exports — single-singleton invariant.** The plugin references core
  symbols (VM/GC/Object/…) but doesn't link `roxalcore`; they resolve at
  `dlopen` time from the **executable**, which is linked with `ENABLE_EXPORTS`
  (`-rdynamic`). This is load-bearing: `VM::instance()`, `SimpleMarkSweepGC::
  instance()`, and `DataflowEngine::instance()` are Meyers singletons, and
  `-rdynamic` makes the plugin bind to the executable's copies (the VM static is
  even emitted `STB_GNU_UNIQUE`), so there is exactly **one** GC/VM across the
  boundary. Without `-rdynamic` the plugin would get its *own* second GC/VM —
  silent corruption.

- **ABI consistency — `roxal_abi`.** Because the plugin shares core C++ *types*
  with the host across the boundary, it must be compiled with the **identical**
  layout-affecting preprocessor defines as `roxalcore` — any `#ifdef`-guarded
  member (e.g. under `ROXAL_ENABLE_GRPC`, `ROXAL_COMPUTE_SERVER`) shifts class
  layouts and corrupts memory. This is enforced structurally: the `roxal_abi`
  INTERFACE target is the single source of truth for those defines, consumed by
  `roxalcore` (PUBLIC, so the exe inherits) **and** `roxalqt`. (This bug bit once
  during development — `ModuleQt::onModuleLoaded` wrote `VM::m_hostEventLoop` at
  a layout offset that differed between plugin and core — which is why the shared
  target exists.)

- **Clean failure.** `loadQtPluginModule()` throws `std::runtime_error` if the
  plugin or its Qt runtime can't be loaded; `RoxalCompiler`'s import resolution
  catches it and emits a normal `import 'qt' failed: …` compile error rather than
  crashing. `runtests.py` guards the distributable property by asserting the
  `roxal` binary has no direct Qt `NEEDED` entry whenever the build supports qt.

This is Linux/ELF-specific (host-exports). A Windows or fully-decoupled port
would instead make `roxalcore` itself a shared library that the binary and the
plugin both link.

### Bytecode cache (`.roc`)

Compiled modules are cached as `.roc` files next to their `.rox` source (the
dot-prefix is just to keep the directory listing tidy). The compiler reads
the cache when source mtime ≤ cache mtime; otherwise it recompiles and
overwrites. `--recompile` deletes all caches under the source root before
running. Cache reads happen via `compiler.loadFileCache` (top-level scripts
and builtin-module companions) or `RoxalCompiler::loadModuleFromCache`
(nested `import`s during compilation).

Each cache read creates a *fresh* `SerializationContext` and reconstructs
every `Obj` in the file's reachable graph — including `ObjModuleType`s,
`ObjObjectType`s, `ObjFunction`s, and the `Chunk` constants those functions
reference. There is **no cross-file dedup**: loading `foo.roc` and `bar.roc`
where both reference the same `foo.module` produces two distinct
`ObjModuleType*` instances. This is by design — the cache file is self-
contained — but it means an extra pass is needed to glue the deserialized
fragments back into a coherent module graph.

### `reconcileModuleReferences`

After a successful `loadModuleFromCache`, `reconcileModuleReferences`
([compiler/RoxalCompiler.cpp](compiler/RoxalCompiler.cpp)) walks every
function in the deserialized chunk and substitutes "duplicate" instances
with the **canonical** one — the live ObjModuleType already held by either a
loaded `BuiltinModule`, a global, or a previously-canonicalized peer.

The two invariants that hold after reconcile:

1. **One canonical `ObjModuleType` per module name** for the duration of
   the program. `canonicalizeModuleValue` is *memoized* per reconcile pass
   (a `unordered_map<ObjModuleType*, Value>` keyed on the fresh input
   pointer). The first decision sticks, and both directions of the mapping
   are recorded — when input X resolves to canonical Y, future queries with
   either X *or* Y as input return Y. This eliminates non-determinism
   where two duplicates each pick the other as "canonical" depending on
   transient `vars` snapshot state.

2. **Merging is non-destructive.** `mergeModuleTypes` walks `source->vars`
   and stores only entries the target doesn't already have. If source and
   target both carry a same-named type (`ObjObjectType` / `ObjEventType`)
   with a different pointer, the source pointer is recorded in a
   `canonicalTypeMemo` so chunk-constant occurrences of the dup can be
   substituted later. This preserves "live" state already attached to the
   canonical module's types — most importantly, `builtinInfo` patched onto
   method functions by `linkMethod`.

After the per-function walk, a second sweep over each function's
`chunk->constants` substitutes any `ObjObjectType` / `ObjEventType` constant
that appears in `canonicalTypeMemo` with its canonical Value, so bytecode
that references types by chunk-constant index sees the same pointer as
runtime dispatch.

Builtin-module developers can compile with `-DDEBUG_BUILTINS` (or
uncommenting `DEBUG_BUILTINS` in `CMakeLists.txt`'s `add_compile_definitions`
block) to get a `[builtins] linked sys.Time.kind`–style confirmation line
per successful `link` / `linkMethod` call.

### REPL commands and `/reload` semantics

The interactive REPL uses chat-style `/`-prefixed commands (mirrors
Slack/Discord/Notion/IPython-magic conventions):

- `/help` — list available REPL commands.
- `/run <file>` — compile and execute a Roxal script file against the REPL
  module. The script body re-runs on every `/run`; its imports are subject
  to the user-module cache below.
- `/reload` — drop the VM-level user-module registry and the REPL
  `RoxalCompiler`'s `importedModules` map. The next `import` (or the next
  `/run` that does an import) recompiles dependency modules from source —
  picks up `.rox` file edits made between runs.
- `/quit` — exit the REPL. Ctrl-D also works (linenoise EOF).

Because the user-module registry is process-lifetime, an interactive REPL
session caches every imported dependency after first use — a subsequent
`/run` of an editor-tweaked script picks up edits to the *script itself*
but **not** edits to its dependencies' `.rox` files unless `/reload` is
issued first.

To make re-imports actually rebind in the REPL, `OpCode::ImportModuleVars`
uses `overwrite=true` *only* when the target module is the REPL module
(`replModuleValue`). For non-REPL modules the historical "first import
wins" behaviour is preserved.

**Known limitation — Python `reload` semantics, not IPython
`%autoreload 2`:** existing user-created instances retain their *old*
`instanceType` pointer and old method tables after `/reload`. New calls
through `Probe()` after `/reload` produce instances of the freshly-loaded
type, but `var p = Probe(); /reload; p.fire()` will run the old `fire`.
`p is Probe` returns false against the new type. Migration of live
instances across type-identity swaps is a future task (would require
in-place mutation of `ObjObjectType::methods` etc. while preserving the
existing pointer — analogous to what IPython's autoreload does by patching
`__class__` and class dicts on existing instances).


## Continuations

The VM uses continuation-based execution to handle operations that require
calling Roxal closures from native code. Rather than recursively calling
`execute()`, native code sets up continuation state and returns control to
the main `execute()` loop. When the closure completes, a handler processes
the result.

All continuation states are stored as **stacks** (vectors) on the `Thread`
object, supporting arbitrary nesting depth. For example, a `list.map` callback
can itself call `list.filter`, and an `operator->string` body can call `print`
which triggers another param conversion. Each mechanism pushes state when
activated and pops when complete.

### EventDispatchState

Handles event handler dispatch. When an event is emitted, `processEventDispatch()`
captures a snapshot of registered handlers and pushes each handler closure as a
call frame (marked with `isEventHandler = true`). After each handler returns,
the next handler is pushed until all have executed.

```cpp
struct EventDispatchState {
    bool active;
    PendingEvent currentEvent;
    std::vector<HandlerRegistration> handlerSnapshot;
    size_t nextHandlerIndex;
    bool prevThreadSleep;
    TimePoint prevThreadSleepUntil;
};
```

### NativeContinuation

A general-purpose continuation for native functions that call Roxal closures
iteratively, such as `list.filter()`, `list.map()`, and `list.reduce()`. Also
used as the dispatch trampoline for `NativeDefaultParamState` and
`NativeParamConversionState`.

```cpp
struct NativeContinuation {
    std::function<bool(VM&, Value)> onComplete;
    Value state;
    bool active;
    ptrdiff_t resultSlotIndex;   // Index into value stack (-1 = not set)
    ptrdiff_t stackBaseIndex;    // Index into value stack (-1 = not set)
    size_t callbackFrameDepth;   // Frame depth when callback frames are pushed
};
```

Stored as `std::vector<NativeContinuation> nativeContinuationStack` on Thread,
with helpers `pushContinuation()`, `currentContinuation()`, `popContinuation()`,
`hasContinuation()`.

Stack positions use **indices** (not raw pointers or iterators) because the
value stack vector may reallocate during nested operations. `callbackFrameDepth`
is set by `pushContinuationCall()` and used by `processContinuationDispatch()`
to distinguish "this continuation pushed another iteration frame" (depth matches)
from "an outer continuation's callback frame is on top" (shallower depth = done).

### NativeDefaultParamState

Handles closure-based default parameter evaluation for native functions.
Piggybacks on `NativeContinuation` with
`onComplete = processNativeDefaultParamDispatch`.

Stored as `std::vector<NativeDefaultParamState> nativeDefaultParamStack`.

`callNativeFn()` detects closure defaults via `getClosureDefaultParamIndices()`,
partially marshals args with `marshalArgsPartial()`, and pushes default closure
frames one at a time. `processNativeDefaultParamDispatch()` stores each result
and either pushes the next closure or invokes the native function with
complete args. It does not call `clearContinuation()` — `processContinuationDispatch`
handles popping the continuation stack.

### NativeParamConversionState

Handles async user-defined type conversion for native function parameters.
Piggybacks on `NativeContinuation` with
`onComplete = processNativeParamConversion`.

Stored as `std::vector<NativeParamConversionState> nativeParamConversionStack`.

`callNativeFn()` detects params needing async conversion via
`needsAsyncConversion()`, marshals args (storing originals for async params),
and pushes conversion frames one at a time via `pushParamConversionFrame()`.
`processNativeParamConversion()` stores each converted value and either
pushes the next conversion frame or invokes the native function with
complete args. Like `NativeDefaultParamState`, it does not call
`clearContinuation()` — the dispatch handles stack popping.

### ClosureParamConversionState

Handles async type conversion for Roxal function parameters (activated in
`frameStart`). Stored as
`std::vector<ClosureParamConversionState> closureParamConversionStack`.

When a closure param conversion frame returns inside a native continuation
(e.g., calling a typed Roxal function inside an `operator->string` body),
`processContinuationDispatch` checks frame depths to route the return to
`processClosureParamConversion()` instead of the native continuation's handler.

### CallFrame Fields

Call frames carry context for the dispatch loop:
- `strict`: The callee's lexical strict setting (from `ObjFunction::strict`)
- `callerStrict`: The caller's lexical strict setting (set during frame push).
  Used by `frameStart` parameter conversion and `findConversionMethod()`.
- `isEventHandler`: Return triggers next event handler
- `isContinuationCallback`: Return triggers `onComplete` handler

After `OpCode::Return`/`OpCode::ReturnStore`, `execute()` checks
`thread->continuationCallbackReturned` to dispatch to the appropriate handler.


## Real-Time Integration

The VM supports incremental execution for real-time control loops via the
deadline parameter to `execute()`.

### execute() with Deadline

```cpp
std::pair<ExecutionStatus, Value> VM::execute(TimePoint deadline = TimePoint::max())
```

The dispatch loop checks `TimePoint::currentTime()` against the deadline.
When reached, `execute()` returns `ExecutionStatus::Yielded` with all state
preserved. The caller can resume by calling `execute()` again.

### Blocking Operations

Operations that can block the thread:
- `wait(ms=N)`: Sleeps on condition variable
- Future awaiting: Polls/waits for resolution
- Actor method calls: Cross-thread calls return futures

Blocked threads yield at the deadline and resume when the blocking condition
clears or time elapses.


## Constness and MVCC

Roxal supports transitive immutability via the `const` keyword. When a mutable value is converted to const (`T → const T`), it becomes a **frozen snapshot** — an isolated view of the object graph as it existed at conversion time, immune to subsequent mutations through other references. The reverse conversion (`const T → T`) is prohibited; `clone()` returns a mutable deep copy, and `move()` can transfer sole ownership.

### Value-Level Const: ConstMask (bit 48)

Constness is tracked at the `Value` level using a single bit in the NaN-boxed representation:

```cpp
const uint64_t ConstMask = uint64_t(1) << 48;
```

The `asObj()` and `asControl()` extraction masks strip this bit (along with SignBit, QNAN and WeakMask) to recover the raw pointer. Const Values participate in normal strong ref counting — they keep the object alive like any other reference.

Key methods on Value: `isConst()` checks the bit; `constRef()` returns a copy with the bit set (and increments the refcount); `mutableRef()` strips the bit (used internally, never exposed to user code).

### Transitive Constness

Constness is **transitive**: accessing a property of a const object yields a const value. This is enforced at the VM level — `GetProp`, `GetPropCheck`, and index opcodes check whether the receiver is const and, if so, ensure the returned child is also const. This contrasts with C++, where constness of a pointer member does not propagate.

### Mutation Blocking

`SetProp` on a const Value raises a runtime error: `"Cannot mutate const: assignment to '<name>'"`. Similarly, all mutating builtin methods (e.g., `list.append()`, `dict.store()`) check the receiver's const bit via `noMutateSelf` / `noMutateArgs` flags and error if it is set.

At compile time, the compiler rejects reassignment of `const`-declared identifiers (using existing `constVars` tracking). The `MakeConst` opcode calls `createFrozenSnapshot()` on the top-of-stack value.

### MVCC: Why Not Eager Freeze?

The naive approach to `T → const T` — walking the entire reachable object graph to copy or mark every sub-object — is O(n) in graph size. For a `const c = bigList`, this would copy thousands of elements even if only one is ever read through `c`.

A lazy approach (incrementing a "const ref count" on children only when accessed through a const ref) also fails: if a mutable alias mutates a child *before* any const read, the child has no const-ref count, no copy-on-write triggers, and the mutation leaks through. The `const-interior-mutation.rox` test demonstrates this exact scenario.

MVCC resolves this by **versioning mutations** rather than eagerly copying the graph. The cost is redistributed: `T → const T` is O(#root-properties), mutations pay O(#properties) only when snapshots are active, and const reads pay O(version-chain-length) only on first access (then cached).

### Global Write Epoch and Snapshot Tracking

Three global atomics coordinate versioning:

- **`globalWriteEpoch`** (starts at 1): bumped on each mutation to any object while snapshots are active. Each bump via `fetch_add(1)` returns a unique epoch value assigned to the mutated object.
- **`activeSnapshotCount`**: when 0, mutations skip the version-save path entirely (one well-predicted branch per mutation — zero overhead in the common case).
- **`latestSnapshotCreationEpoch`**: used for version-save deduplication — if an object has already saved a version since the last snapshot was created, redundant saves are skipped.

These are declared in `ObjControl.h` as `inline` globals.

### ObjControl: Per-Object MVCC State

Each `Obj` has an `ObjControl` block (used for ref counting and GC). The MVCC extension adds:

- **`writeEpoch`** (atomic uint64): the epoch at which this object was last mutated. Starts at 0 for newly created objects.
- **`snapshotToken`** (pointer): non-null only for frozen clones — points to the `SnapshotToken` for the snapshot this clone belongs to.
- **`versionChain`** (atomic pointer): linked list of `ObjVersion` nodes, newest first. Each node holds: `epoch` (the object's writeEpoch *before* the mutation — i.e. when it entered this state), `snapshot` (a shallow clone capturing the pre-mutation state), and `prev` (link to older version).
- **`lastSaveEpoch`**: for deduplication — compared against `latestSnapshotCreationEpoch`.

### SnapshotToken: Per-Snapshot Identity

When a `T → const T` conversion creates a frozen snapshot, a `SnapshotToken` is allocated. It holds:

- **`epoch`**: the `globalWriteEpoch` at snapshot creation time. This is the "as-of" timestamp for all const reads through this snapshot.
- **`cloneMap`**: maps live `Obj*` → weak `Value` refs to frozen clones. This preserves alias identity within a snapshot: if `o.a is o.b` (same underlying object), then `c.a is c.b` (same frozen clone). It also handles cycles.
- **`refcount`** (atomic): all frozen clones from the same snapshot (root + lazily materialized children) hold a ref to the token. When the last frozen clone dies, the token is deleted and `activeSnapshotCount` decremented.

### `createFrozenSnapshot()`: The T → const T Path

Called by the `MakeConst` opcode, and internally by event emission and `var x: const T` reassignment. The implementation (`Object.cpp`):

1. **Passthrough**: if already const, return as-is (no re-snapshot).
2. **Primitives**: return directly (value types are inherently immutable).
3. **Sole-owner fast path**: if `control->strong <= 1`, no other live reference exists — just set the const bit, no clone needed. This makes `move()` → actor truly zero-copy.
4. **Otherwise**: shallow-clone the root object (copies property slots; children remain shared refs to live objects). Allocate a `SnapshotToken` with `epoch = globalWriteEpoch`. Attach the token to the clone. Increment `activeSnapshotCount`.

Cost: O(#direct-properties-of-root), NOT O(reachable-graph).

### `saveVersion()`: Capturing Pre-Mutation State

Every mutation method on `Obj` subtypes (`ObjList::setElement`, `ObjDict::store`, `ObjectInstance::setProperty`, etc.) follows this sequence:

1. Check `activeSnapshotCount > 0`. If zero, skip versioning entirely.
2. Call `saveVersion()`:
   - **Deduplication**: skip if `lastSaveEpoch >= latestSnapshotCreationEpoch` (no new snapshot since last save).
   - Shallow-clone the object's current state → version node with `epoch = control->writeEpoch` (the "birth epoch" of the state being saved).
   - CAS-prepend the node to the version chain (lock-free, append-only).
3. Apply the mutation in place.
4. Bump the object's epoch: `control->writeEpoch = globalWriteEpoch.fetch_add(1)`. Done *after* mutation so readers see the new epoch only after the new state is fully written.

### `resolveConstChild()`: Lazy Materialization on Const Reads

When `GetProp` (or index access) reads a reference-type child through a const receiver, it calls `resolveConstChild()`. This is the core of lazy snapshot materialization:

1. If the child is a primitive or already const: return directly.
2. Check the `SnapshotToken::cloneMap` — if a frozen clone for this live `Obj*` already exists in this snapshot, reuse it (alias/cycle preservation).
3. Call `findVersionForEpoch(childObj, epoch)`:
   - If the child's `writeEpoch < snapshotEpoch`: it was never mutated since the snapshot — the current state is valid. Clone from current.
   - If `writeEpoch >= snapshotEpoch`: walk the version chain to find the newest version with `epoch < snapshotEpoch`. Clone from that version's snapshot.
4. Shallow-clone the source → frozen clone. Attach the same `SnapshotToken` (incrementing its refcount). Register the weak ref in `cloneMap`.
5. **Cache** the frozen clone back into the parent's property slot (or list element, or dict entry) so subsequent reads are O(1).

The strict `<` comparison is important: `writeEpoch == snapshotEpoch` means a mutation consumed the same global epoch value as the snapshot (via `fetch_add`), so it may have occurred after the snapshot and must be resolved via the version chain.

### Walkthrough: Interior Mutation Isolation

```roxal
var o = Outer(Mid(Leaf(1)))
var m = o.m                   // mutable alias to Mid
const c: Outer = o            // snapshot at epoch E=5
m.l.i = 2                    // mutate Leaf.i
print(c.m.l.i)               // → 1 (isolated)
```

- **Snapshot**: shallow-clone Outer → `Outer'` (epoch=5). `Outer'.m` still points to live `Mid`.
- **Mutation**: `Leaf.i = 2` triggers `saveVersion()` on Leaf (saves version with epoch=0, the birth epoch). Sets `Leaf.writeEpoch = 5`.
- **Const read** `c.m.l.i`: `Outer'` (frozen) → resolve `Mid` (writeEpoch=0 < 5, not mutated, clone current) → resolve `Leaf` (writeEpoch=5 ≥ 5, walk version chain, find epoch=0 version with `i=1`, clone that) → read `i` → returns 1.

### Copy-on-Write (COW) for Containers

`shallowClone()` is called both by `createFrozenSnapshot()` (for the root) and by `saveVersion()` (for pre-mutation snapshots). Making this O(1) is crucial for performance. Three container types use COW via shared `ptr<>` (wraps `std::shared_ptr`):

**ObjList**: has two internal storage representations selected by a `repr_` flag. A list holding only `byte` values keeps them packed as raw octets in `ptr<std::vector<uint8_t>> packed_` (1 byte/element); any other list uses the boxed `ptr<std::vector<Value>> elts_` (one 64-bit `Value`/element). Exactly one pointer is active. `shallowClone()` copies `repr_` plus both pointers (the inactive one is null); `ensureUniqueStorage()` COW-copies whichever vector is shared. This pattern is already used by `ObjMatrix`, `ObjVector`, and `ObjTensor`.

The representation is **invisible to language semantics**: every accessor (`getElement`, `index`, iteration, `equals`, printing, `in`) returns/compares elements identically regardless of representation, and no operation errors because of it. Fresh and empty lists start packed-capable. Transition rules (modelled on PyPy list-strategies / V8 element-kinds):
- *Element-wise* mutation that writes a non-byte value (`setElement`/`setIndex`/`append`/`insert`) calls `unpack()` — it boxes every byte into a fresh `elts_` and then proceeds. This is one-way: content later becoming all-bytes again does **not** auto-repack (avoids thrash; keeps costs predictable).
- *Bulk replacement and construction* (`setElements`, slice `index(range)`, `concatenate`, `read`) re-evaluate packability from scratch, so a slice/concat/deserialize of all-byte content comes back packed.
- `unpack()` builds a new `elts_` rather than mutating `packed_` in place, so MVCC version snapshots (which `saveVersion()` captures via `shallowClone`, sharing the old `packed_`) stay valid. It must run inside the mutator's existing `ensureMutable`/`CowGuard`/`saveVersion` bracket. **Caution**: routing a fresh sublist through the MVCC-guarded `setElements` while a snapshot is active bumps its `writeEpoch` and breaks `resolveConstChild`'s epoch check for const range-indexing — `index(range)` therefore populates the sublist's storage directly.
- `trace()` is a no-op when packed (no `Value` references), like `ObjTensor`. Serialization writes a high-bit-flagged count for packed lists (raw octet run) and the ordinary per-element format for boxed lists; the reader re-packs an all-byte boxed stream. `isPackedBytes()`/`packedBytes()`/`adoptPackedBytes()`/`stealPackedBytes()` are the C++ producer/consumer entry points (used by `fileio` binary reads, `serialize`, `to_bytes`, and tensor `bytes=`); the `sys._list_repr(l)` builtin exposes the representation to tests.

**ObjTensor storage** is dtype-native raw bytes in both builds. With ONNX (`ROXAL_ENABLE_ONNX`) the buffer lives inside a `shared_ptr<Ort::Value>`; without it, in `ptr<std::vector<uint8_t>>` (`numel * dtypeSize` bytes) accessed through `rawElementAsDouble`/`rawSetElementFromDouble` (with small IEEE-754 half-float converters for `float16`). This gives identical element semantics — including low-precision dtype quantization — with or without ORT. The `tensor(bytes=…)` constructor reinterprets a byte list as this raw buffer (zero-copy adopt in the non-ORT build when the source is a sole-owner packed list; one `memcpy` otherwise), and `tensor.to_bytes()` copies it back out to a packed byte list. Tensor serialization still streams `double`s per element for cross-build portability.

**ObjDict**: storage is `ptr<DictData> data_` where `DictData` bundles the `std::map` of entries and the `std::vector` of insertion-ordered keys. Same COW pattern. The per-object mutex (previously needed for thread safety) was removed — COW + atomic shared_ptr handles concurrent access.

**ObjectInstance**: property storage is `ptr<PropertyMap> properties_` where `PropertyMap = std::unordered_map<int32_t, MonitoredValue>`. Same COW pattern.

All three have `ensureUnique()` methods called by every mutation path and by `cacheElement`/`cacheValue`/non-const `findProperty` (since frozen clones share the ptr and const-read caching writes back through these accessors).

### Builtin No-Mutate Optimization

Many builtin methods (e.g., `list.length()`, `dict.contains()`, `string.find()`) are read-only. Requiring a frozen snapshot for every call would add unnecessary allocation overhead. Instead, builtins can be annotated at registration time:

- **`noMutateSelf`**: the method does not mutate the receiver.
- **`noMutateArgs`** (bitmask): each argument independently annotated as non-mutating.

For annotated builtins, the VM sets the ConstMask bit on a stack copy of the Value — just a bit-flip, O(1) — without creating a frozen clone. If the builtin (incorrectly) tried to mutate, the const flag would catch it at runtime. This eliminates clone overhead on hot paths like `len()`, `contains()`, and `indexOf()`. These annotations are declared in `.rox` module files alongside the parameter declarations.

**Note for builtin implementors — const arguments with reference-type children:**

When a native C++ function receives a const frozen snapshot (e.g., a const list of objects), the root object is a shallow clone with stable storage (COW). The direct elements are the Values as they were at snapshot time. However, if those elements are reference types (ObjectInstance, nested List, etc.), they point to the *original* live objects. If those objects are mutated after the snapshot was created, native code that accesses them directly (e.g., `asList(args[0])->getElement(i)`) will see the current (mutated) state, not the snapshot state.

The VM handles this transparently via `resolveConstChild()` in its opcode handlers (GetIndex, GetProp), but native code bypasses this. Two options for native implementors:

1. **Use `BuiltinModule::resolveConstChildValue(parent, child)`** — a helper that wraps the MVCC resolution logic. Pass the const parent and the raw child extracted from it; it returns the correctly resolved value at the snapshot's epoch. Zero overhead when the parent has no snapshot token.

2. **Use `clone()` on the argument** — for implementations where performance is not a concern or containers are small, a deep copy avoids the issue entirely. The native code gets its own independent copy and doesn't need to worry about MVCC resolution.

In practice, most current builtins are unaffected because they operate on the receiver's own data (Image pixels, Socket fd, etc.) or on primitive-valued elements. The concern only arises for native functions that deeply traverse an object graph received as a const argument.

### Actor Boundary Semantics

Actor method parameters are **implicitly const** — `frameStart` applies `createFrozenSnapshot()` for each param whose `funcType` type has `isConst` set (which includes implicit actor const). At the actor boundary (`queueCall()`), non-primitive arguments also use `createFrozenSnapshot()` for MVCC-based isolation:

- **Sole-owner with no Obj children** (e.g., list of primitives): `createFrozenSnapshot()` just sets the const bit — zero-copy transfer via `move()`.
- **Sole-owner with Obj children**: falls through to the shared path below. The root's sole-ownership alone doesn't guarantee interior objects aren't aliased elsewhere, so the MVCC path is required for safety.
- **Shared**: `createFrozenSnapshot()` shallow-clones the root object (O(#properties)). Children remain as shared refs to live objects and are lazily resolved via `resolveConstChild()` on the actor thread.

This avoids the O(graph-size) deep-clone that was previously required at actor boundaries. Lazy resolution on the actor thread is safe because a **per-object spinlock** (`cowLock_` in `ObjControl`) protects the COW `ptr<>` members against concurrent read (shallowClone) + write (ensureUnique). Mutation methods acquire the lock (via `CowGuard` RAII) around `saveVersion + ensureUnique + mutation + epoch bump` when `activeSnapshotCount > 0`. `resolveConstChild` acquires the lock on the live child object when cloning from current state (not needed for immutable version-chain snapshots), and re-checks `writeEpoch` under the lock to handle the TOCTOU window where a mutation may have raced between the initial epoch check and lock acquisition. Zero overhead when no snapshots are active.

Return types default to mutable (deep-clone for caller isolation). `-> const T` returns a frozen snapshot via `createFrozenSnapshot()`. For mutable returns, the actor thread (in `Thread::act()`) checks sole-ownership *and* interior isolation via `isIsolatedGraph()` — if the root is sole-owner but interior objects are aliased within the actor's state, the return value is deep-cloned to prevent cross-thread sharing of interior objects.

The `mutable` keyword on an actor parameter opts out of implicit const. The caller must use `move()` to transfer sole ownership; if the root value is aliased, a runtime error is raised at the actor call site (not at the `move()` site). Additionally, `queueCall()` runs `isIsolatedGraph()` — a two-pass graph traversal that verifies every mutable interior object (List, Dict, Instance) has no external aliases. If any do, the call is rejected: `"Cannot pass value with aliased interior objects as mutable actor parameter"`.

### Dataflow Engine Const Safety

Dataflow function nodes (`FuncNode`) execute on the dataflow engine's actor thread, not the main thread. They can access module-scope variables via `GetModuleVar`/`SetModuleVar` opcodes, creating potential data races.

Two protections are in place:

**Thread-local flag**: `VM::onDataflowThread_` is a `thread_local bool`, set via an RAII `DataflowThreadGuard` around the three VM entry points in `FuncNode.cpp` (`invokeClosure` in `conditionallyExecute()`, `runFor` in `resumeExecution()`, and `invokeClosure` in the non-deadline path). When the flag is set:
- `GetModuleVar` wraps the returned Value with `constRef()` — the DF func sees a const view.
- `SetModuleVar`, `SetNewModuleVar`, and `MoveModuleVar` raise a runtime error: `"Cannot modify module variable '<name>' from dataflow function"`.

**Closure capture check**: when a closure is registered as a dataflow function node (in `VM::callValue()`), the VM iterates its upvalues. If any captured value is a non-const reference type, a runtime error is raised: `"Dataflow function '<name>' captures a mutable reference variable"`. This check happens at registration time on the main thread, preventing the unsafe state from ever reaching the DF thread.

### Event Implicit Const

Events are implicitly const — `emit` calls `createFrozenSnapshot()` on the event payload before dispatch. Handlers receive a const view; attempting to mutate event data (including transitively nested properties) raises a runtime error.

### Signal Restriction

`const Signal` is prohibited at the compiler level — signals exist to change over time, so making them immutable is semantically contradictory. All declaration forms (`const s: Signal`, `var s: const Signal`, etc.) produce a compile error.

### Tests

The const/MVCC implementation is covered by an extensive test suite (all in `tests/`):

- **Core snapshot isolation**: `const-interior-mutation`, `const_mvcc`, `const_snapshots`, `const_multi_snapshot`
- **Graph topology**: `const_alias` (alias preservation), `const_cycle` (cyclic graphs), `const_diamond` (diamond sharing), `const_deep_chain` (deep nesting)
- **Identity**: `const_identity` (`is` and `==` behavior)
- **Containers**: `const_list`, `const_dict`
- **Methods**: `const_method_dispatch`, `const_builtin_method_err`
- **Type qualifiers**: `const_type_qualifier`, `const_mutable_type`, `const_func`
- **Error cases**: `const_assign_err`, `const_escape_err`, `const_property_method_err`, `const_property_runtime_err`, `const_signal_err`, `const_signal_type_err`, `const_missing_initializer_err`, `const_nonliteral_err`
- **Stress**: `const_mvcc_stress` (exercises version chains under high mutation load)
- **Dataflow safety**: `df_capture_mutable_err` (closure capture check)
- **Interior alias isolation**: `const_interior_alias` (const actor param with aliased interior objects falls back to safe path), `move_interior_alias_err` (mutable actor param with aliased interior objects errors)

## Remote Compute Server

Roxal's remote actor support reuses the existing actor model rather than adding a
separate distributed object system. A remote actor still looks like an ordinary
actor to user code: construction returns an actor instance, actor method calls
still return futures, and `wait(for=...)` remains the synchronization point.

### Overview

- `roxal --server` starts a compute server that accepts one or more TCP client
  connections.
- `MyActor(...) at "host[:port]"` causes the actor type plus constructor args
  to be shipped to the remote process, where a real actor instance and thread
  are created.
- The local side receives a proxy `ActorInstance` marked `isRemote=true`.
- Calls on that proxy are queued as normal via `ActorInstance::queueCall()`,
  but the proxy thread dispatches them over a `ComputeConnection` instead of
  executing locally.

This keeps the language-facing semantics aligned with ordinary actors while
moving the actual execution to another process.

### Protocol and Connection Model

The wire protocol is defined in `compiler/ComputeProtocol.h`. It uses framed
messages:

- `HELLO` / `HELLO_OK` / `HELLO_ERR`
- `SPAWN_ACTOR` / `SPAWN_RESULT`
- `CALL_METHOD` / `CALL_RESULT`
- `PRINT_OUTPUT`
- `ACTOR_DROPPED`
- `BYE`

`ComputeConnection` owns one bidirectional TCP connection and a reader thread.
Outgoing RPC-like requests are tracked by `call_id` in a pending-call table.
Each pending entry stores:

- a `std::promise<Value>` used to complete the local wait
- print-routing metadata for Phase 8 output forwarding

This means the transport is synchronous per network hop internally (the helper
thread blocks on the promise/future pair), while still exposing the normal
asynchronous Roxal future interface to Roxal code.

### Remote Actor Proxies

Remote actor proxies are ordinary `ActorInstance`s with additional transport
state:

- `isRemote`
- `remoteActorId`
- `remoteConn`

The proxy still has its own local worker thread. When that thread pulls a
queued call in `Thread::act()`, it notices `isRemote` and sends `CALL_METHOD`
to the remote process rather than invoking the bound method locally. The reply
is received as `CALL_RESULT`, which fulfills the local Roxal future.

This design means existing actor call machinery (`queueCall`, futures, wakeups,
`wait(for=...)`) does not need a separate remote-specific user-visible path.

### Back-channel Actor References

Actor references passed across the network are serialized specially using
`NetworkSerializationContext`:

- a local actor sent over a connection is registered in a per-connection actor
  table and serialized as a foreign actor id
- when the far side reads that actor reference, it creates a remote actor proxy
  pointing back across the same connection

This enables the "back-channel" case where a remotely running actor calls a
method on an actor reference that originated on the caller side.

### Type Shipping

Remote actor creation has to ship more than just constructor args. The remote
side must have the actor type and any user-defined object/actor types that its
methods refer to.

The `SPAWN_ACTOR` payload therefore contains:

- the remote call id
- a dependency preamble of shipped type definitions
- the main actor type definition
- constructor `CallSpec`
- constructor args

Dependencies are collected by walking:

- actor methods
- nested function constants
- default-parameter functions
- object-type references in constant pools
- `superType`
- property type references
- function signature metadata (`funcType`)

Each shipped dependency is keyed by canonical module export identity:

- module full name
- module short name
- exported symbol name

On the server, dependency types are deserialized first and registered into the
appropriate module exports before the main actor type is deserialized and
canonicalized.

### Type Freshness and Stale Server State

A long-lived compute server can already have an exported type for a given
`(module, symbol)` from an earlier spawn.

To address this, each shipped dependency type and the main actor type carry a
64-bit content fingerprint:

- the client serializes the type to bytes
- computes an FNV-1a 64-bit hash of those bytes
- includes that hash in `SPAWN_ACTOR`

On the server:

- if an existing canonical export for `(module, symbol)` has the same
  fingerprint, it is reused
- if the fingerprint differs, the stale export is cleared before deserializing
  the incoming type, then replaced with the new canonical definition

This is intentionally the simple freshness model: one canonical "current"
definition per module export symbol. It fixes the dev-time stale-type problem
without yet implementing multi-version coexistence on one server.

### Print Redirection

Remote print routing is call-scoped rather than process-scoped.

Each in-flight remote call carries a print target:

- local stdout, or
- an upstream `(ComputeConnection, call_id)` pair

`sys.print(value='', end='\n', flush=false, here=false)` uses that target:

- with `here=false`, output is routed back to the originating caller if the
  current call came from a remote peer; otherwise it prints locally
- with `here=true`, output always goes to the local process's stdout

`PRINT_OUTPUT` frames are forwarded transitively, so if `A -> B -> C` and code
running on `C` calls `print()`, the output is forwarded from `C` to `B` to `A`.
Local actor-to-actor calls made while servicing a remote call inherit the same
print target, so nested local calls on the server also print back to the
originating client by default.

### Lifetime Model

Remote actor lifetime is connection-scoped and deliberately simpler than
distributed GC:

- when the last local proxy for a remote actor is dropped, the proxy destructor
  sends `ACTOR_DROPPED`
- on disconnect, the server tears down actors associated with that client

This is closer to remote reference counting than distributed tracing. Cross-
process cycles are not collected automatically and are currently considered the
programmer's responsibility to break explicitly.

### Limitations

- Type versioning: multiple live versions of the same type are not supported
- Type-shipping unoptimized: on actor invocation, types may be shipped unnecessarily
- Cross-remote actor reference method calls are routed hop-by-hop, not directly.
- No fully-disctributed GC
- Seperate server TCP connection per-actor-instance rather than shared

## Controversial Design Decisions

- The language allows the `/` character in user-defined literal suffixes and the builtin sys module defines literal suffix `m/s`
  - This means that writing `1m/s` is interpreted as a quantity 1 with unit meters per second.  However, if the user declared a variable `s` and wrote `1m/s` expecting to have 1 meter divided by the scalar value of the `s` var, that is not the language interpretation.
  - On one hand, if the user is utilizing units, they should know that `m/s` is a unit and that they should include a space after units, like `1m / s`.  Alternatively, `1{m}/s` also works.
  - In addition, if the value `1m/s` is used somewhere expecting a distance, an error would indicate it is a velocity instead.

- The vector() constructor accepts quantities in vector literals for elements and for list elements for the from-list constructor.  However, it it not dimensioned.  It converts to SI units and discards the dimension.
  - e.g. `[1m 2m 3m]` converts to `[1 2 3]`, but `[1in 2m 3m]` converts to `[0.0254 2 3]`. Similarly for vector([1in,2m,3m]). All elements must have the same dimension type (e.g. can't mix distance and time), though 0 can be 'bare' (no units)
  - This is convenient for specifying vector forms of orientations, like the orient() constructor args such as rpy.  So that `orient(rpy=[10deg 20deg 30deg])` is valid and as expected. (the vector values are converted to radians and passed and orient stores a quaternion).
  - It may be convenient for specifying robot joint configuration vectors also (but only if all the joints are of the same type), as in `[10deg 20deg -30deg 0 3.1rad 0]`, but won't help if the joints mix revolute and prismatic, forcing use of a list with comma separator syntax in that case, which can cause confusion
  - matrix and tensor don't have this behaviour
