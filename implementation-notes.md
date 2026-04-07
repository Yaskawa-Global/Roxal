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
   saves state in `NativeParamConversionState`, sets up `NativeContinuation`
   with `onComplete = processNativeParamConversion`, and pushes the first
   conversion frame via `pushParamConversionFrame()`
3. Each conversion frame returns to `processNativeParamConversion()` which
   stores the converted value in the args buffer and pushes the next
   conversion frame (or calls the native when all conversions are done)
4. After the native returns, the original callee+args are cleaned up

This is the same pattern as `NativeDefaultParamState` (for closure-evaluated
default parameters) — both defer the native call until async pre-call work
completes.

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

For async conversions, `ClosureParamConversionState` (sibling to
`NativeParamConversionState`) stores the target frame depth and param indices
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
`strict` flag. `findConversionMethod()` also takes an explicit `strictContext`
parameter for checking `@implicit(nonstrict_only=true)` annotations.

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
for auto-conversion unless marked `@implicit`. This matches modern language
conventions (C++ recommends `explicit` on single-arg constructors; Rust and
Swift have no implicit constructors). The `@explicit` annotation on
constructors is a no-op (the default is already explicit).

Auto-conversion eligibility is checked in `tryConvertValue()` via
`hasImplicitAnnotation()`.

### Native Functions with Continuations

When a native function needs to execute Roxal code iteratively (e.g.,
`list.map()` calling a predicate for each element), it cannot re-enter
the dispatch loop. Instead it uses the `NativeContinuation` mechanism:

1. The native sets up `thread->nativeContinuation` (state + `onComplete`
   callback) and optionally sets `resultSlot` / `stackBase` for stack cleanup
2. It calls `pushContinuationCall()`, which pushes closure + args and calls
   `call()`, creating a new frame marked `isContinuationCallback = true`
3. The native returns a dummy value — `callNativeFn` detects that new frames
   were pushed (`thisCallPushedFrames`) and skips the normal callee+args
   cleanup, since the continuation frames sit on top of that area
4. If the continuation did not set a `resultSlot`, `callNativeFn` fills one in
   automatically to cover the original callee+args area
5. The dispatch loop executes the Roxal callback naturally
6. When the callback returns, `opReturn` sets `continuationCallbackReturned`
7. `processContinuationDispatch()` pops the result, calls `onComplete`, and
   either pushes the next iteration's frame or finalizes: pops the original
   call's footprint (via `resultSlot` / `stackBase`) and pushes the final result

On exception unwind, `unwindFrame()` detects continuation callback frames and
extends the pop range to include the original call's footprint using
`resultSlot`, then clears the continuation.


## Types & Values

Runtime values in the language are represented by the Value class, which wraps a 64bit value.  This holds builtin primitives (`bool`, `int`, `real`, `decimal`, `enum`) and references to reference types.  The implementation uses NaN-boxing, whereby the full 64bit are used as a C double for the type `real`, but if the Quiet NaN (Not-a-Number) flags are set, then, it is instead assumed to be one of the other types, as stored in the type tag.

These by-value types can be tested via the various is*() Value methods (`Value::isBool()`, `isInt()`, etc).

In the case of reference types (`list`, `dict`, `vector`, `matrix`, `signal`, user-defined objects & actors), the `Value` only indicates the builtin type, or that is is an Object or Actor for user-defined types.  For enums, the Value holds the enum numeric value and a typeid value corresponding to a global registry of enum type information.  The reference is to an instance of class `Obj`.  Value manages reference counting for `Obj` references (via `incRef()` and `decRef()`).

The reference types are implemented in `Object.h`|`cpp`.  `list` and `dict` types currently use STL `std::vector` and `std::map` of Values.

The `vector`, `matrix` and `tensor` types utilize the Eigen library.  Although these are reference types, the intention is that they behave like value types (- the current implementation is a mixture - operations create new values, but they're passed by reference and assigning elements mutates)

## Scopes

Each `.rox` file is a module by default, even if not declared as such (according to the filename).  Within a module, module-scoped variables can be used without forward declarations - references by name are resolved at runtime.

Within a function or method, parameters and variable or function declarations are local.  These are access via via offsets from the function's execution frame pointer.

Functions are first-class values and can capture variables from outer scopes, yielding a closure (`ObjClosure`), which encapsulates the function's static code (`Chunk`), and captures upvalues.  Upvalues initially refer to stack entries of enclosing function scopes, but are 'lifted up' into the heap as required before the original stack positions are unwound.


## Object & Actor types

A new object type (like a C++ class) can be declared and have its own methods (`func` or `proc`) and member variables.  Members can be declared private, in which case they're not accessible outside the scope of the type's methods.

An actor type is similar to an object, but additionally has its own thread of execution associated with it.  This thread is the only thread that can execute the actor's methods. Hence, when another thread (e.g. the main script thread or another actor's thread) calls an actor instance's methods, a future for the return value (if any) is immediately returned to the caller, which can continue to execute asynchronously.  Only if that future needs to be converted into the return value will the execution block, if necessary, until the called actor's method has completed and returned to provide the value.  Hence, execution of methods within an actor are serialized, since there is only one thread, so that developers need not worry about shared state between multiple threads.

Complex reference types passed to an actor's method (or returned from it) behave as if deep-copied (cloned).  In practice they use Multi-Version Concurrency Control (MVCC), as discussed below, to avoid actually copying.


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


## Signals and Data-Flow

The VM includes a data-flow engine (in `Dataflow/`) that can represent a set of signals (`Signal` & `ObjSignal`) of Values that interconnect as inputs and outputs to function nodes (`FuncNode`).
The dataflow engine will updates signal values as they are effected by changes to other signals via functions.  There exists a special builtin function `clock(freq)` that creates a native signal that counts up at the specified frequency.

Function nodes wrap standard functions (`func`) and execute their `Chunk` code (via a `Closure`).

The data flow engine is represented as a builtin actor instance.  Hence, the evaluation of all functions (`FuncNode`s) happens on the dataflow engine's actor thread.

Signals can be sampled to yield their current value at any time on any thread, either via the builtin `value` property, or by using them to construct their underlying value type (e.g. `vector(vecsignal)`, or `real(realsig)`)

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


## Continuations

The VM uses continuation-based execution to handle operations that require
calling Roxal closures from native code. Rather than recursively calling
`execute()`, native code sets up continuation state and returns control to
the main `execute()` loop. When the closure completes, a handler processes
the result.

Four continuation mechanisms exist in the `Thread` class:

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
iteratively, such as `list.filter()`, `list.map()`, and `list.reduce()`.

```cpp
struct NativeContinuation {
    std::function<bool(VM&, Value)> onComplete;  // Called when closure returns
    Value state;                                  // Iteration state (e.g., index)
    bool active;
    Value* resultSlot;                           // Where to write final result
    ValueStack::iterator stackBase;              // Stack position for cleanup
};
```

The native function sets up state, pushes a closure frame with
`isContinuationCallback = true`, and returns. When the closure returns,
`processContinuationDispatch()` invokes `onComplete`, which either pushes
another closure frame or completes with the final result.

### NativeDefaultParamState

Handles closure-based default parameter evaluation for native functions.
When a native function has parameters with closure defaults (computed at
call time), this mechanism evaluates each default before invoking the
native function.

```cpp
struct NativeDefaultParamState {
    bool active;
    NativeFn nativeFunc;                         // Native function to invoke
    ptr<type::Type> funcType;
    std::vector<Value> staticDefaults;
    CallSpec callSpec;
    bool includeReceiver;
    Value receiver;
    Value declFunction;
    uint32_t resolveArgMask;
    std::vector<Value> argsBuffer;               // Args being built
    std::vector<size_t> closureParamIndices;     // Params needing evaluation
    size_t nextClosureIndex;
    std::map<int32_t, Value> paramDefaultFuncs;  // Param hash -> closure
    size_t originalArgCount;
};
```

`callNativeFn()` detects closure defaults via `getClosureDefaultParamIndices()`,
partially marshals args with `marshalArgsPartial()`, and pushes default closure
frames one at a time. `processNativeDefaultParamDispatch()` stores each result
and either pushes the next closure or invokes the native function with
complete args.

### NativeParamConversionState

Handles async user-defined type conversion for native function parameters.
When a native function has typed parameters and an argument requires executing
Roxal code to convert (e.g., an object with `@implicit operator->string()`
passed to `print(value:string)`), this mechanism defers the native call until
all async conversions complete.

```cpp
struct NativeParamConversionState {
    bool active;
    NativeFn nativeFunc;
    ptr<type::Type> funcType;
    CallSpec callSpec;
    bool includeReceiver;
    Value receiver;
    Value declFunction;
    uint32_t resolveArgMask;
    std::vector<Value> argsBuffer;               // Args with conversions applied
    std::vector<size_t> conversionParamIndices;   // Params needing async conversion
    size_t nextConversionIndex;
    size_t originalArgCount;
};
```

`callNativeFn()` detects params needing async conversion via
`needsAsyncConversion()`, marshals args (storing originals for async params),
and pushes conversion frames one at a time via `pushParamConversionFrame()`.
`processNativeParamConversion()` stores each converted value and either
pushes the next conversion frame or invokes the native function with
complete args.

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

**ObjList**: internal storage is `ptr<std::vector<Value>> elts_`. `shallowClone()` copies the shared pointer (refcount bump, O(1)). Before any mutation, `ensureUnique()` checks `use_count() > 1` and copies the vector if shared. This pattern is already used by `ObjMatrix`, `ObjVector`, and `ObjTensor`.

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
