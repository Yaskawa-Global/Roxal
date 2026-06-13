# Roxal Compute Server — Implementation Plan

## Context

Roxal actors are isolated units of state that communicate only via queued method calls returning futures. This isolation makes them natural candidates for transparent remote execution: an actor can run on a remote machine with no changes to the calling code, because callers already interact through the async future interface.

The goal is `roxal --server` mode plus an `at "host:port"` syntax extension so that `a = MyActor() at "host:port"` silently creates the actor on the remote process, while all method calls (`a.someMethod(...)`) remain identical — they just serialize over TCP instead of queuing locally.

Key properties of the existing system that make this tractable:
- `ObjObjectType::write/read` fully serializes an actor type including all method closures and bytecodes
- `ObjFunction::read` has a re-linking mechanism for native (builtin) methods — they don't need to be shipped
- `ActorInstance::write/read` serializes instance properties and restores a running actor with a new Thread
- `.roc` cache files use magic `ROXC` + version `27` — the same check can gate the compute protocol
- Imports are NOT transitively loaded at runtime (only when explicitly encountered), but the actor type's method closures carry their bytecodes inline, so most code is self-contained in the serialized type

---

## Phase 1: Grammar — `at` Soft Keyword

**File**: `compiler/Roxal.g4`

Use `IDENTIFIER` directly in the grammar — no new lexer token needed. After the main expression, optionally match `IDENTIFIER expression`. The compiler validates that the identifier is exactly `"at"` and raises a compile error otherwise. Since no existing expression grammar accepts a bare `IDENTIFIER` after a complete expression (method calls use `.identifier`, not a standalone identifier), this is unambiguous.

```antlr
// Parser — modify expr_stmt only; no lexer changes
expr_stmt
  : expression (IDENTIFIER expression)?
  ;
```

`IDENTIFIER` after the main expression can only be `at` (enforced at compile time in `visit(ExprStmt)`). The second `expression` is any arbitrary expression evaluated at runtime — it must yield a string (e.g., `"192.168.1.1:7777"`, or a variable/function call that produces a string).

Examples:
```
a = MyActor() at "192.168.1.1:7777"    # string literal
a = MyActor() at serverAddr             # string variable
a = MyActor() at getServerAddr()        # any expression
```

**AST change** (`core/AST.h`): Add `std::optional<ptr<ast::Expr>> atHost` to `ExprStmt` (or to a new `RemoteActorExpr` wrapper — TBD based on where cleanest to thread through the compiler visitor).

---

## Phase 2: Protocol Definition

**New file**: `compiler/ComputeProtocol.h`

```
Magic:   'R','X','C','S'  (4 bytes)
Version: uint32_t = 27   (same as .roc cache version — enforces matching serialization)

Frame:   [uint32_t payload_len][uint8_t msg_type][payload bytes...]

Message types:
  HELLO         = 0x01   client→server: magic(4) + version(4)
  HELLO_OK      = 0x02   server→client: (no payload)
  HELLO_ERR     = 0x03   server→client: reason string
  SPAWN_ACTOR   = 0x10   client→server: call_id(8) + serialized ObjObjectType + serialized init-args list
  SPAWN_RESULT  = 0x11   server→client: call_id(8) + actor_id(8) on success, or error string
  CALL_METHOD   = 0x20   either dir:   call_id(8) + actor_id(8) + method_name_len(4) + method_name + serialized args list
  CALL_RESULT   = 0x21   either dir:   call_id(8) + ok(1) + serialized result Value or error string
  ACTOR_DROPPED = 0x30   either dir:   actor_id(8)
  BYE           = 0xFF   either dir:   clean shutdown
```

Serialized values use the standard `writeValue/readValue` with a shared `SerializationContext`. Actor Values are encoded as a special sentinel so they can be identified and replaced with stubs (see Phase 4).

---

## Phase 3: ComputeConnection Class

**New files**: `compiler/ComputeConnection.h`, `compiler/ComputeConnection.cpp`

Manages a single bidirectional TCP connection (used on both client and server side).

```cpp
class ComputeConnection {
public:
    // Client side: connect to server
    static ptr<ComputeConnection> connect(const std::string& hostPort);

    // Spawn a remote actor: ships the type + init args, returns a remote actor proxy Value
    Value spawnActor(const Value& actorType, const std::vector<Value>& initArgs);

    // Called by RemoteActorThread to send a method call and await result
    Value callRemoteMethod(uint64_t remoteActorId,
                           const icu::UnicodeString& methodName,
                           const std::vector<Value>& args,
                           const CallSpec& callSpec);

    // Register a local actor so back-channel calls can be routed to it.
    // Stored as Value (not raw Obj*) for GC integration.
    // Use Value::weakRef() so the actor can be GCed when no other refs exist.
    uint64_t registerLocalActor(const Value& actorVal);  // actorVal.weakRef() stored
    void unregisterLocalActor(uint64_t id);

private:
    int fd_;                                              // TCP socket fd
    std::thread readerThread_;                            // background message reader
    std::atomic<uint64_t> nextCallId_{1};
    std::atomic<uint64_t> nextActorId_{1};

    // Pending outgoing calls: call_id → promise
    std::unordered_map<uint64_t, ptr<std::promise<Value>>> pendingCalls_;
    std::mutex pendingMu_;

    // Local actor registry (for back-channel calls).
    // Values stored as weakRef so actors can be GCed independently.
    std::unordered_map<uint64_t, Value> localActors_;    // Value::weakRef() entries
    std::mutex localActorsMu_;

    void readerLoop();
    void sendFrame(uint8_t msgType, const std::vector<uint8_t>& payload);
    void handleCallMethod(uint64_t callId, uint64_t actorId,
                          const icu::UnicodeString& method,
                          const std::vector<Value>& args);
};
```

**Network serialization context** (`NetworkSerializationContext`): extends `SerializationContext` with a `ComputeConnection*`. During `writeValue`, when an `ActorInstance` is encountered:
- If it is a **remote actor** (see Phase 4 `isRemote` flag), write it as a remote ref `(connection_id=0, actor_id=remoteId)` — the server already owns it
- If it is a **local actor**, register it with `registerLocalActor()` and write a foreign-ref marker with the assigned local ID

On `readValue` with this context, a foreign-ref marker creates a `RemoteActorProxy` pointing back to the originating side.

---

## Phase 4: Remote Actor Proxy (ActorInstance extension)

**File**: `compiler/Object.h` (ActorInstance) + `compiler/Thread.cpp` (Thread::act)

Add remote mode fields to `ActorInstance`:

```cpp
// ActorInstance additions
bool isRemote = false;
uint64_t remoteActorId = 0;
// weak_ptr so the connection can be torn down independently of the actor proxy.
// ComputeConnection is C++ infrastructure, not a Roxal Value, so shared_ptr/weak_ptr is appropriate here.
weak_ptr<ComputeConnection> remoteConn;
```

Modify `Thread::act()`: after acquiring the next `MethodCallInfo` from the call queue, check `actorInst->isRemote`. If true, instead of local dispatch:

```cpp
if (actorInst->isRemote) {
    auto conn = actorInst->remoteConn.lock();
    // conn is guaranteed alive while actor is alive
    Value result = conn->callRemoteMethod(
        actorInst->remoteActorId, methodName, callInfo.args, callInfo.callSpec);
    callInfo.returnPromise->set_value(result);
    asFuture(callInfo.returnFuture)->wakeWaiters();
    continue; // back to event loop
}
```

This keeps the actor's external interface identical — callers still call `queueCall()`, get a future back, and block/await as normal.

Important requirement:
- Preserve enough method metadata end-to-end on the proxy side to tell whether a remote method is a `func` or `proc`, so the proxy can decide correctly whether `queueCall()` must produce a future. This should be treated as the real fix; any fallback that forces a completion future when metadata is missing is only a defensive stopgap.

**Factory function** `Value makeRemoteActor(const Value& actorType, uint64_t remoteId, ptr<ComputeConnection> conn)`:
- Create `ActorInstance` with `isRemote=true`, `remoteActorId`, `remoteConn` set
- Start Thread (calls `Thread::act()` which will use remote dispatch path)
- Register with VM thread list

---

## Phase 5: Server Mode

**File**: `compiler/roxal.cpp` (new execution mode), new `compiler/ComputeServer.h/cpp`

### CLI
```
roxal --server [--port N]   (default port: 7777)
```

Detected in `main()` before script execution logic (alongside existing `--precompile` etc.).

### ComputeServer

```cpp
class ComputeServer {
public:
    void listen(uint16_t port);  // blocks; accepts connections in a loop

private:
    void handleClient(int clientFd);  // runs in its own thread per connection
};
```

`handleClient()`:
1. Read/verify HELLO (magic + version mismatch → HELLO_ERR + close)
2. Send HELLO_OK
3. Create a `ServerConnectionContext` to track actors spawned for this client
4. Enter message loop:
   - **SPAWN_ACTOR**: deserialize `ObjObjectType` into the shared VM (call `VM::instance().registerType(type)`), create `ActorInstance` via the normal `Value::actorInstanceVal()` path + start Thread, assign server-side actor ID, register in context, send SPAWN_RESULT
   - **CALL_METHOD**: look up actor by ID in context, call `actorInst->queueCall(...)`, wait for future, serialize result, send CALL_RESULT
   - **ACTOR_DROPPED**: tear down the named actor (quit its thread)
   - **BYE** / connection close: tear down all actors in context

### Shared VM considerations
Multiple clients share a single `VM::instance()`. Each client's actors get their own Thread objects (as normal). Actor types received from clients are registered in the shared VM's type registry. Name collision risk is low since the serialized type includes the full qualified name; the server can check if an identical type is already registered (by name + version) and reuse it.

---

## Phase 6: Compiler/Runtime for `at` Clause

**File**: `compiler/RoxalCompiler.cpp` — modify `visit(ast::ExprStmt)` (or wherever `ExprStmt` is visited).

When an `ExprStmt` has a non-null `atHost`:

1. Compile the call expression normally up to the point where the actor instance would be created
2. After the call result is on the stack, emit a call to a new native builtin `__spawn_remote(actorValue, hostString)` which:
   - Extracts the just-created local `ActorInstance` (which was instantiated via normal path)
   - Calls `ComputeConnection::connect(hostPort)` (or gets a cached connection for that host)
   - Calls `conn->spawnActor(actorType, initArgs)` which serializes the type, sends SPAWN_ACTOR, waits for SPAWN_RESULT
   - Returns a `RemoteActorProxy` Value (via `makeRemoteActor(...)`)
   - The original local actor is discarded (its init args were already captured)

**Preferred approach**: VM opcode `OpCode::SpawnRemote` emitted after the call expression's args are resolved. In `VM::execute()` in the actor-construction path (around VM.cpp:2217), check for a pending `SpawnRemote` flag on the current frame and redirect to remote construction. This avoids touching the compiler visitor significantly.

---

## Phase 7: Module Dependency Shipping

Most actor method bytecodes are self-contained after the type is serialized (closures carry their chunks). The main gap is **user-defined types referenced in method bodies** (e.g., argument types or return types from user modules).

During the `SPAWN_ACTOR` serialization on the client:
- Walk the `ObjObjectType`'s method closures' constant pools
- Any `ObjObjectType` constant that belongs to a non-builtin module (check `chunk->moduleName` against `VM::getBuiltinModuleType()`) must also be included in the payload
- Add these as additional type definitions prepended to the SPAWN_ACTOR payload
- Server deserializes and registers them first, then deserializes the main actor type

For the initial implementation, this can be a recursive walk with a visited set (to avoid duplicates/cycles).

---

## Phase 8: print() redirection

Optional - consider first.

Currently the builtin print() function just outputs to std::cout.  This implies that the behaviour of a script will change if one actor is instantiated remotely, since if it uses print() the output will appear on the server instead of the client.  So, implement routable print() so that
when code on a remove --server uses print() the output is routed back to the client side.
This would need to work transitively, so that a client invoking a method on a server B, which in turn invokes a method on server C would need to be piped back from C to B to A for display.

Add a builtin parameter to `sys.print` to allow opting out of routing for debugging on the machine where the code is actually executing:
- proposed signature change: `print(value='', here:bool=false)`
- default behaviour remains routed output to the originating client/session
- when `here=true`, print locally on the current process instead of routing back

`here` is preferred over `local` because `local` is ambiguous in a distributed setting, whereas `here=true` reads as "print here where this code is running".

Implementation approach:
- Make print routing call-scoped, not process-scoped.  The current executing actor call carries a print target.
- Add a small runtime `PrintTarget` concept with two cases:
  - local stdout
  - remote upstream target `(ComputeConnection, call_id)`
- Extend `ActorInstance::MethodCallInfo` to carry an optional/inherited print target.
- When `queueCall()` is used during an already-executing actor call, the callee inherits that current print target unless an explicit target is provided.
- This ensures that:
  - local top-level code and ordinary local actors with no remote caller still print locally even with `here=false`
  - local actor-to-actor calls within a remotely-originated call route their output back to the origin

Protocol approach:
- Add a new compute frame:
  - `PRINT_OUTPUT = 0x22`
- Payload:
  - `call_id(8)`
  - printed text string
- Reuse the existing per-connection outbound `call_id` tracking for routing.  When a connection sends a remote `CALL_METHOD`, the pending outbound call entry should also remember where any `PRINT_OUTPUT` frames for that `call_id` should go.
- When `PRINT_OUTPUT` arrives:
  - if the pending call's print target is local stdout, print locally
  - if the target is another upstream `(connection, call_id)`, forward the text as `PRINT_OUTPUT` on that upstream connection
- This gives transitive routing naturally for `A -> B -> C`: prints from `C` are forwarded by `B` to `A`.

Builtin semantics:
- `print(value='', here=false)`:
  - if there is an active routed print target for the current call, route the output to it
  - otherwise print locally
- `print(value='', here=true)`:
  - always print locally on the process where the code is running, bypassing routing
- Keep this line-oriented: route the final formatted line that `print()` would emit, including newline behaviour, rather than introducing a general stdout stream abstraction.

Likely implementation files:
- `compiler/ComputeProtocol.h`
- `compiler/ComputeConnection.h/cpp`
- `compiler/Object.h/cpp`
- `compiler/Thread.cpp`
- `compiler/ModuleSys.cpp`
- `compiler/VM.h/cpp`

Tests to add:
- basic remote print routes back to the client
- `here=true` prints only on the server/local executing process
- transitive forwarding `A -> B -> C`
- local actor invoked during a remote call inherits routed printing
- interleaved remote calls printing concurrently do not cross-wire output

## Files to Create / Modify

| File | Change |
|------|--------|
| `compiler/ComputeProtocol.h` | **New** — constants, framing helpers |
| `compiler/ComputeConnection.h/cpp` | **New** — bidirectional TCP RPC |
| `compiler/ComputeServer.h/cpp` | **New** — server listen/accept loop |
| `compiler/Roxal.g4` | Add `(IDENTIFIER expression)?` to `expr_stmt`; no new lexer token |
| `core/AST.h` | Add `atHost` field to `ExprStmt` (or new `RemoteActorExpr`) |
| `compiler/RoxalCompiler.cpp` | Handle `atHost` in `visit(ExprStmt)`; emit `SpawnRemote` opcode |
| `compiler/VM.cpp` | Handle `OpCode::SpawnRemote`; connect + ship type + create proxy |
| `compiler/Object.h` | `ActorInstance`: add `isRemote`, `remoteActorId`, `remoteConn` |
| `compiler/Thread.cpp` | `Thread::act()`: remote dispatch path for `isRemote` actors |
| `compiler/roxal.cpp` | `--server [--port N]` mode; call `ComputeServer::listen()` |
| `CMakeLists.txt` | Link new `.cpp` files |

---

## Verification

1. **Server startup**: `./build/roxal --server --port 7777` in one terminal — should print a "listening" message and block.

2. **Basic remote spawn + call**: Script with `a = MyActor() at "localhost:7777"` + `result = a.someMethod(42)` — verify the call executes on the server process (e.g., server-side print or log) and the future is fulfilled on the client.

3. **Type shipping**: Actor whose constructor arg or method references a user-defined `object` type from another `.rox` file — verify it works without that module file existing on the server.

4. **Back-channel**: Client passes a local actor ref as argument to remote actor method; remote calls a method on it; verify the call comes back and executes locally.

5. **Disconnect cleanup**: Kill client mid-run; verify server tears down the remote actors gracefully (no zombie threads).

6. **Version mismatch**: Build two mismatched versions, attempt connection — should get a clean error, not a crash.
