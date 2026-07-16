# GC/RT Coordination v2 — Redesign Specification (Draft, rev 2.1)

Status: **exploratory design** — not scheduled.  The shipped v1 coordination
(see `implementation-notes.md` § "Garbage Collection & Thread Coordination")
is verified and remains the production design until a phase of this plan is
explicitly adopted.  Rev 2.1 (2026-07-15) incorporates an external design
review: the address oracle is now specified (2.4), the scanner accepts raw
object pointers (2.2a), `SafeBlocked` is an explicit transition with a
capture protocol (2.1), thread lifetime moves to a `ThreadManager` (2.2d),
reclamation is a collector phase with two-stage actor finalization (2.6),
destruction semantics are stated honestly as promptly-queued (2.7), the
RT-plan/evaluator handoff is specified (2.8), the migration is re-cut with
a shadow-verification gate (§3), and acceptance gates are added (§6).
Rev 2.2 (2026-07-15) incorporates review round 2 + the RT-tick decision:
scanner scope restricted to Parked/SafeBlocked (2.1/2.2a), normative
state/epoch memory-order table (2.1), capture-into-context-record (2.1),
oracle range corrected to `[ctrl, ctrl+allocationSize)` (2.4), borrowed-
pointer contract for auxiliary storage (2.2a), pinning stats promoted to
operational (2.2a/§4), single-mutex root-list protocol (2.2b), traced
`Finalizing` actor state + generalized lifecycle queue + destructor
contract (2.6), honest Vyukov + batch rule (2.7), RT context/segment
preparation (2.3), threadless-profile runtime assert (2.5), and §2.8
replaced: budgeted RT tick with structured overrun errors + advisory lint
instead of a certified-plan gate (certified plan recorded as future
option).

Keeps: the simple non-moving mark & sweep collector, NaN-boxed Values,
`ObjControl` blocks, the cooperative stop-the-world model, and the hard-RT
contract (an RT slice never parks, never blocks on GC, never performs a
collection).

Replaces: the *coordination* architecture around the collector — how threads
become quiescent, how roots are discovered, who reclaims, and how the RT
boundary is expressed.

---

## 1. Lessons from v1 (why redesign)

Fifteen confirmed bugs during the v1 effort cluster into four root causes:

| Class | Count | Examples |
|---|---|---|
| Hand-enumerated roots (a Value the mark phase couldn't see) | ~8 | event-dispatch state, `ModuleDDS::defaultParticipant`, gRPC `ActiveStreamState`, `staticDefaults` |
| Opt-in quiescence (a blocking call nobody taught to park/poll) | 3 | `std::thread::join`, compute-future wait, DDS/gRPC reader design |
| Dual-owner destruction (refcount path × sweep path × N drainers) | 2 | concurrent `freeObjects` batch split, lost cleanup notification |
| Unbounded work coupled to GC pauses | 1 | event-backlog recursion overflowing the fixed value stack |

v1 fixed each instance, but the *mechanism count* grew to compensate: generic
registration, `ExternalParticipant` (scoped + persistent + nested +
owner-marked parking), `GCYieldScope`, `GCNoParkScope`, `ScopedGCMutatorCover`,
`GCSafeBlockScope`, `ExternalRootProvider`, per-structure `trace()` methods,
a 100-line hand-maintained `visitSingleThreadRoots`, and a single-drainer
protocol with a re-armed notification flag.  Every one is individually
justified; collectively they form a vocabulary that future contributors must
know completely, because **the failure mode of forgetting any of it is silent
heap corruption**.

The v2 goal is not a smarter collector.  It is a smaller vocabulary in which
the safe thing is the default thing:

| Situation | v1 answer (one of nine) | v2 answer |
|---|---|---|
| Native/C++ stack local `Value` | participant root visitor / covered window / nothing (bug) | automatic (conservative parked-stack scan) |
| Retained `Value` in a C++ container | `ExternalRootProvider` + hand-written `visitRoots` | `PersistentRoot<T>` / `TracedMember<T>` (self-registering type) |
| Ordinary thread running interpreter/native code | `onThreadEnter` / participant / cover (pick correctly) | its one `MutatorContext`, state `Running` |
| Blocking operation | poll in loop / scoped participant / `GCSafeBlockScope` (pick correctly) | ONE explicit `BlockingRegion` wrapper → `SafeBlocked` (embedded in all runtime-owned blocking calls) |
| RT work | `GCYieldScope` + scattered in-section checks | `RTSlice` token; GC-aware APIs take/assert the token |
| Suspended interpreter state | registry + `replThread` + `dataflowEngineThread` + FuncNode special cases | every `Thread` centrally registered ctor→dtor |
| Compile in flight | `GCNoParkScope` (barrier waits out whole compile) | compiler root arena (compile can park) |
| Who collects | inline election *or* dedicated thread (two modes everywhere) | dedicated collector, always (threaded profile) |
| Who destroys | any `freeObjects` caller (try-lock protocol) | one reclaimer, fed by the collector |

---

## 2. Core components

### 2.1 `MutatorContext` — one state machine per physical thread

Every thread that ever touches GC-managed state owns exactly one TLS context,
created lazily on first contact and centrally listed:

```cpp
enum class MutatorState : uint8_t {
    Inactive,     // exists, not currently touching GC state
    Running,      // mutating: barrier must wait for it
    Parked,       // at a safepoint, waiting out a collection
    SafeBlocked,  // inside an opaque call (join/recv/cq->Next/waitset);
                  // collector proceeds WITHOUT it; transition-check on return
    RTSection,    // holding an RT slice token (see 2.3)
};
```

Nesting (nested `execute`, native→closure→native, participants-within-
participants) adjusts a depth counter on the *same* context; the collector
counts physical contexts, never scopes.  This subsumes and deletes:
`onThreadEnter`/`onThreadExit`, `tlExecuteRegistered_`, `ExternalParticipant`
registration/parking/owner-marking, and the double-registration rules
(v1's `GCSafeBlockScope` survives as the `BlockingRegion` transition below).

**Barrier:** a collection may start when no context is `Running` (and no
RT slice from before the request is still open — 2.3).  `SafeBlocked` and
`Inactive` contexts are quiescent by definition — but with different scan
treatment: **Parked and SafeBlocked stacks are captured and scannable;
Inactive stacks are NEVER scanned.**  An Inactive thread may be executing
unrelated C++, so its stack is changing; its contract is that no unrooted
GC pointer exists while Inactive — which holds by construction, since a
thread only becomes Inactive by returning out of all GC-touching frames.

**`SafeBlocked` is explicit, not automatic.**  The runtime cannot detect
that arbitrary native code is about to call `recv()`, `join()`, or a
blocking third-party API.  A context enters `SafeBlocked` only through a
scoped `BlockingRegion` transition (the v2 successor of `GCSafeBlockScope`),
and every runtime-owned blocking operation — `Thread::join`, condvar waits,
the DDS waitset, the gRPC completion queue, compute-future waits — embeds
it in one central wrapper.  Policy: native integrations must not block
outside these wrappers (debug assert + clang-tidy in v2-adopted
directories).  *Entry protocol:* capture registers into a per-context
**capture area that the collector scans explicitly as a root region** —
GCC/Clang: `__builtin_unwind_init()` forces callee-saved registers into
the capturing frame, which lies inside the recorded `[SP, base]` range;
MSVC: `RtlCaptureContext` into the context record, scanned as an
additional root region (so no ABI needs register values to land in
ordinary stack range; per-target capture stubs remain the documented
fallback; NOT `setjmp` — glibc pointer-mangles SP/FP slots in `jmp_buf`).
Record SP **before** the opaque call; the
scanner walks only `[capturedSP, stack base]`, and the region's contract is
that no GC-touching work happens inside it (allocation and Value operations
assert `state != SafeBlocked` in debug builds).  *Return protocol:* the
`SafeBlocked → Running` transition CASes against the collection epoch and
parks first if a collection is active — a blocked thread can never leave
its captured state while a scan is in progress (the JVM in-native pattern).
This keeps v1's join/compute deadlock fixes with ONE mechanism instead of
nine, though at third-party boundaries it remains an enforced convention,
not something the type system proves.

**State/epoch protocol (normative).**  Cross-thread observation of context
state means the orders below are load-bearing — this is the v1 Dekker
argument made explicit per transition:

| Transition / operation | Actor | Protocol |
|---|---|---|
| Request collection | collector | seq-cst store `requestedEpoch`; then acquire-load each context's state |
| Safepoint poll (Running) | mutator | seq-cst load `requestedEpoch`; if raised → park path |
| Running → Parked / SafeBlocked | mutator | write capture area (registers + SP) FIRST, then **release-store** state; notify barrier cv |
| Read a captured stack | collector | only after an **acquire-load** observes Parked/SafeBlocked — pairs with the release-store, so capture *happens-before* scan |
| SafeBlocked → Running (return) | mutator | seq-cst CAS on state, then seq-cst load of request/in-progress; if a collection is active, revert to Parked and wait it out — a thread can never leave its captured state mid-scan |
| Parked → Running (resume) | mutator | wait on barrier cv until epoch advances (acquire-load) |
| RTSlice enter / exit | RT | unchanged v1 Dekker: seq-cst `fetch_add` on the slice count, THEN seq-cst load of the request; on conflict decrement and fail.  Enter/exit never notify; the collector polls while slices exist |

The Running-vs-request race resolves as in v1: poll and request are both
seq-cst, so either the mutator sees the request at its next poll, or the
collector sees `Running` and waits for the park notification — no third
interleaving exists.  Native boundary cost is unchanged in spirit (a store
+ a load per boundary), but the store is release, not relaxed, whenever
captured data must be visible.

### 2.2 Roots — discoverable, not remembered

Three complementary mechanisms replace all hand-written enumeration:

**(a) Conservative scan of parked C++ stacks (the default for locals).**
The collector walks `[captured SP, stack base]` of every **Parked or
SafeBlocked** context (never Inactive — its stack may be live, see 2.1)
applying **two acceptance rules** — native code frequently holds a raw
`Obj*`/`ObjString*` as the *only* live reference, so tag-checking
alone would have false negatives:

- *(i) Tagged `Value`s*: the word carries a NaN-box object tag (const/weak
  bits normalized before payload extraction) AND the payload resolves in
  the address oracle (2.4).  Fast path with a very tight false-positive
  filter.
- *(ii) Raw pointers*: the word resolves **into** any registered
  allocation via the oracle's range lookup — interior pointers into the
  GC block accepted.  This rule is the safety net; (i) is an optimization
  of it.

**Borrowed-pointer contract (auxiliary storage).**  Rule (ii) protects
pointers into the `[ObjControl | object]` block only.  A `char*` into an
ICU string buffer or an iterator into `std::vector` storage points into a
*separate native allocation* the oracle cannot map back to its owner.
Contract: **a borrowed pointer into an object's auxiliary storage must be
accompanied by a rooted owner** (the owning `Value`/`Obj*` alive on the
same stack or in a persistent root) — usually true naturally, but the
optimizer can drop the owner while keeping a cached `data()` pointer, so
the rule is explicit and §6 carries a documenting expected-fail test.

False-positive retention is **not bounded to one cycle**: a stale word in
a long-lived frame (an actor loop, `main`) can pin an object across many
collections, or indefinitely.  Must land inside a live allocation, so
false positives stay rare — but the objects that hurt when pinned are the
big-buffer ones (tensors, images), which is why pinning statistics
(per-object-type counts and bytes retained *only* by stack-scan hits) are
an **operational** requirement, not a diagnostic nicety, and big-buffer
types are already on the explicit-release audit (2.7).  The collector is
non-moving, so conservatism has no compaction cost.  This retires the
*entire* "in-flight local" bug class with zero API change — natives keep
using plain `Value` and plain `Obj*`.

Requirements: the collector-owned address oracle of 2.4 (range lookup, not
just exact match); `-fno-omit-frame-pointer` not required (linear scan, not
unwinding); registered stack bounds per context (captured at context
creation); register/SP capture at every park/`SafeBlocked` transition
(2.1).

**Portability (decided: conservative is the default on all native
targets).**  Because parking is *cooperative* (threads park themselves at
safepoints), the collector never suspends a thread externally or fetches
its register context from the OS — the two genuinely platform-specific
operations in Boehm-style collectors.  Each parking thread spills its
callee-saved registers itself into its own stack frame
(`__builtin_unwind_init()` on GCC/Clang, `RtlCaptureContext` on MSVC — see
2.1; glibc `setjmp` is unusable here because it pointer-mangles `jmp_buf`
slots) and records its own SP; only the stack *base* needs a
per-platform lookup: `pthread_getattr_np` (Linux, any CPU),
`GetCurrentThreadStackLimits`/TIB (Windows), `taskInfoGet` TCB fields
(VxWorks), statically known stacks (RTOS/bare-metal).  Works unchanged on
x86, AMD64, ARM64, ARM32 v7a, and Cortex-M class MCUs.  On 32-bit targets
a 64-bit `Value` spans two stack words: rule (ii) already catches the
payload word (on LE it *is* a plain pointer), and for rule (i) the scanner
reconstructs 64-bit candidates at every 4-byte offset via `memcpy` in
native byte order — a single 32-bit word can never satisfy both the tag
and pointer checks by itself.

**The exception is WebAssembly**: wasm compiles C++ locals into
non-addressable wasm locals — no scanner can see them, by sandbox design.
A wasm embedding (e.g. VM in a web IDE) therefore uses the threadless
profile (2.5) with a **collect-only-at-host-boundary policy**: allocation
mid-turn only raises the request flag; the host loop collects between
`execute()` turns, when no native frames are live and the precise roots
(thread registry, persistent roots, arenas) are the complete root set.
Heap high-water grows within one script turn — acceptable for an IDE.  If
in-turn collection on wasm ever becomes necessary, the options are
Binaryen `--spill-pointers` + `emscripten_scan_stack()` (toolchain cost)
or precise shadow stacks (`Root<Value>` scopes) for that profile only.

**(b) `PersistentRoot<T>` / `TracedMember<C>` (for heap-resident C++ state).**
Conservative stacks cannot see `Value`s stored in malloc'd containers.  Those
become typed:

```cpp
class ModuleDDS : public BuiltinModule {
    TracedMember<std::unordered_map<std::string, Value>> idlModules{this...};
    PersistentRoot<Value> defaultParticipant;
    ...
};
```

The wrapper self-registers on a global root list and knows how to visit
its contents.  Registration protocol (deliberately the simplest one —
"lock-free push + locked unlink" do not compose safely): **one mutex for
register/unregister; persistent roots may not be created or destroyed
inside an RTSlice (debug assert); the collector scans the list locklessly
only after all mutators are quiescent.**  Honestly stated: forgetting to wrap a member is **not** a C++
type error — raw `Value` members still compile.  The rule "no raw `Value`
member in module/engine code" is a greppable convention backed by a
clang-tidy check that is **mandatory in v2-adopted directories** (CI-
enforced), which makes omissions findable rather than impossible.  Deletes
`ExternalRootProvider`, all module `visitRoots()` implementations, and
participant root visitors.

**(c) Compiler root arena.**  The compiler allocates all in-progress GC
objects' references into a `RootArena` owned by the compilation, discarded on
completion.  The arena is a persistent root while alive, so **the compiling
thread can park normally** — deleting `GCNoParkScope` and the "barrier waits
out the whole compile" latency (v1's worst-case pause contributor).

**(d) Central `Thread` ownership — `ThreadManager`.**  "Register at
construction, leave at destruction" with a *strong* registry reference is
circular (the registry would prevent the destruction that unregisters), so
ownership and tracing are split: a **`ThreadManager` owns every
`roxal::Thread`** — explicit `createThread()` / `destroyThread()` / join —
while the GC-side registry is a **non-owning intrusive list** whose unlink
strictly precedes destruction.  Suspended dataflow execution threads, REPL
thread, engine thread, actor workers, and builtin-module throwaway threads
all follow the one rule; the collector iterates the list only while the
world is stopped.  ThreadManager is also the sole join authority (see 2.6's
actor finalization).  One authoritative lifecycle for ownership, execution
registration, joining, and tracing — and it deletes the `replThread` /
`dataflowEngineThread` / yielded-FuncNode / `VM::thread` special handling
(and the thread-local-root blind spot it carried).

### 2.3 `RTSlice` — a first-class read-side token (RCU-style)

The v1 yield-section counter is directionally right; v2 narrows it into an
explicit capability token so the invariant is established once at the RT
boundary instead of being re-checked throughout VM/dataflow internals:

```cpp
if (auto slice = gc.tryEnterRTSlice()) {        // lock-free; fails while a
    drainTelemetry(slice);                       // collection is requested
    engine.tickFor(slice, budget);               // or running
    vm.runFor(slice, remaining);
}                                                // lock-free exit
// else: BusyGC — skip this cycle (bounded staleness, as v1)
```

Properties (all present in v1, now structural):

- Enter/exit are wait-free: one seq-cst counter op, **no condvar notify**
  (the collector polls at `kRTSectionPollInterval` while slices exist).
- No mutator registration happens inside a slice — GC-aware entry points
  take the token (by ref) and *assert* it instead of consulting thread-locals,
  which removes the scattered `inGCYieldSectionOnThisThread()` checks and
  makes "called from RT without a token" a debug-time error.
- Allocation inside a slice only raises an atomic collection request
  (deferred wake, as v1) and bumps a per-thread segment (2.4) — the GC
  *registration* is lock-free.  (That does NOT make allocation itself
  RT-safe: `operator new`, ICU, vector growth and constructors may still
  lock — exactly the soft-RT tail risk the 2.8 lint flags.)
- **RT preparation:** the RT thread's context and a reserve of registry
  segments are created *before* entering RT scheduling — lazy context
  creation and segment allocation both allocate and may lock.  In-slice
  segment rollover draws from the preallocated reserve, then a
  fixed-capacity emergency segment; exhausting both raises a Roxal runtime
  error (a sizing bug — silent unbounded behavior would be worse).
- APIs uniformly report `BusyGC` / `Yielded` so hosts can account skipped
  work (FC's overrun logging keys off this).

### 2.4 Allocation — per-thread registry segments + the address oracle

`registerAllocation`'s global mutex is the last lock on the RT allocation
path (and a global contention point besides).  v2: each context owns a
segment list; allocation bump-appends its `ObjControl*` into the current
segment (thread-local, no lock); the collector walks all segments at mark
time (contexts are quiescent, so unlocked iteration is sound).  Frees
(collector-phase reclamation only, 2.6) tombstone entries; segments compact
lazily on the collector thread.

**Address oracle (required by the scanner, 2.2a).**  Segments do NOT
provide it by themselves: they hold bump-appended `ObjControl*`, so their
address ranges describe the pointer arrays, not the objects — the
`[ObjControl | payload]` blocks are individually allocated.  The oracle is
therefore a **collector-owned page map**: 4 KiB page → sorted block-start
entries answering range (interior-pointer) lookups.  Each entry's valid
range is **`[ctrl, ctrl + allocationSize)`** — `allocationSize` measures
the whole contiguous block *from the control header* (see `newObj`,
Object.h), so a range anchored at `obj` would over-extend past the block
tail and could claim adjacent allocations.  Blocks spanning multiple pages
are indexed on **every** covered page, not only the start page.  It is touched by exactly one
thread: the collector ingests registry additions at cycle start (world
stopped; segments are append-only between cycles, so ingestion is
incremental, not a rebuild) and removes entries as reclamation tombstones
them.  No mutator locks, no hot-path index updates.  The oracle's v0
implementation sources from the *existing global allocation registry* —
segments merely swap the ingestion source later, which **decouples
migration phase A from phase E**.  A slab/page object allocator (which
would make page→object lookup implicit and improve locality) is recorded
as a future optimization, not a dependency.

### 2.5 Collection — dedicated collector, one mode per build profile

Two explicit profiles, **no runtime fallback or election**:

- **Threaded profile** (Linux desktop/RT, default): a dedicated collector
  thread performs every collection (self-demoted `SCHED_OTHER`, RT-core
  exclusion honored).  Early-boot collections are simply deferred until it
  starts; shutdown stops mutators and runs one explicit final collection.
- **Threadless profile** (VxWorks kernel module, bare targets, WebAssembly
  embeddings): the host calls `gc.collect()` synchronously from its own
  loop.  Same code path as the collector-thread body; just a different
  driver.  The profile **asserts its own validity at runtime**: creating a
  second `MutatorContext` under the threadless profile is a fatal error —
  profile selection depends on actual concurrency, not the OS label (a
  VxWorks embedding that spawns actor/dataflow/delivery tasks must build
  the threaded profile).  On wasm the host additionally defers collection
  to turn boundaries (see 2.2a portability note) because parked-stack
  scanning is impossible there.  Wasm module surface: no DDS; signals/dataflow are
  required and work host-driven (the host loop pumps `tickFor`/`runFor`;
  with no foreign-thread producers, queue submission is trivially
  single-threaded); client gRPC, if wanted, must go via gRPC-Web (browsers
  cannot speak native HTTP/2 gRPC — needs a gRPC-Web-aware server or an
  Envoy-style proxy) with streams delivered as host-loop events rather
  than reader threads.

"Which thread collects?" becomes an invariant, deleting the inline election
branch from every barrier path and the mutator-collects interactions that
produced v1's `freeObjects` concurrency class.

Collection sequence:

```
1. collector raises requestedEpoch (atomic)
2. Running contexts reach polls → Parked;  SafeBlocked/Inactive already quiescent
3. new RT slices are refused; pre-existing slices drain (timed polls)
4. scan: thread registry (interpreter state)
        + conservative parked C++ stacks
        + persistent-root list / traced members
        + compiler arenas, globals, modules, interned strings
5. mark; unreachable objects: Live → Retired (atomic state flip), appended
   to the collector-owned retire batch
6. epoch++; mutators resume
7. SAME collector thread destroys the retired batch and drains the RC
   retire queue (2.6/2.7) — concurrent with resumed mutators
8. only then may the next collection begin (reclamation and the next scan
   never overlap — see 2.6)
```

### 2.6 Reclamation — a collector phase, and GC never joins threads

Destruction is separated from tracing and from mutators entirely.
**Decided: reclamation is a phase of the collector thread itself**, not a
separate thread — mark → retire → resume mutators → destroy the retired
batch + drain the RC retire queue (2.7) → only then accept the next cycle.
Destruction still overlaps *mutator* execution (cycles stay flat), but
registry entries, segments, tombstones, and the address oracle are only
ever touched by one thread, so no retirement-epoch protocol is needed
between reclamation and the next scan.  (A separate low-priority reclaimer
thread is recorded as a future option; it would require epoch-based
retirement of registry entries/control blocks — deliberately not paid for
now.)  Between collections, the collector thread's idle wait also services
the RC retire queue, so refcount-driven destruction does not wait for a
collection to happen.

This single-owner model deletes: concurrent `freeObjects` callers,
`freeObjectsMutex_`, the cleanup-notification re-arm, batch-split hazards,
and the actor-vs-non-actor drain asymmetry.

**Actor finalization is two-stage with a traced `Finalizing` state — the
reclaimer never joins.**  An actor object cannot be destroyed before its
worker exits — and until then the worker may still touch the actor and its
outgoing `Value` fields, so a retired-but-not-joined actor must stay
visible to the tracer (if a collection ignored it, its children could be
swept out from under the running worker).  Stage 1: `Live → Finalizing`
(atomic); a `Finalizing` object **remains traced as a root** — it and its
reachable children survive any intervening collection; the reclaimer
signals the worker to stop and hands the join to `ThreadManager` (2.2d),
which joins on its own schedule inside a `BlockingRegion` (2.1).  Stage 2:
on confirmed worker exit, `Finalizing → Retired`; the object re-enters the
retire queue, has its references dropped, and is destroyed normally.  The
same lifecycle queue serves **any object whose finalization can block**
(DDS entity deletion, gRPC channel shutdown) — blocking finalization never
runs on the collector thread.

**Destructor contract** (destructors run on the collector thread between
cycles, so a blocked destructor blocks all future collections): no
blocking operations (I/O waits, joins), bounded lock acquisition only,
recursive retirement is normal and allowed, allocation inside destructors
is forbidden unless explicitly supported (decide in §7 item 8).  Objects
needing blocking close go through the lifecycle queue above.  A debug
watchdog on reclaim-batch duration surfaces violations; §6 gate 9 tests
the contract.

### 2.7 Refcount role — eager destruction stays, with a bounded RT drain

**Decision (2026-07-15): reference counting KEEPS destruction authority.**
The motivation for refcounting is determinism in RT workloads: when
destruction happens as an object goes out of scope, each robot-cycle
iteration pays for (approximately) its own garbage — near-constant work per
iteration in steady state.  Two further RT arguments reinforce this:

- With eager RC, the tracing collector only ever reclaims *reference
  cycles* — collections stay small and rare, so barrier events (and skipped
  RT slices) are minimized.  A defer-everything-to-tracing model funnels
  the whole allocation throughput through the collector, making collections
  large and frequent — trading away the very smoothness it protects.
- Destructor side effects stay *prompt* — but see the refinement below for
  the honest statement: with reclaimer offload they are promptly **queued**,
  not scope-tied.

Rejected alternatives (recorded for the future): the "candidate" model
(zero-crossing only nominates; tracing destroys everything) and full
tracing-only ownership.  Both would dissolve the dual-owner machinery, but
at the cost above.  Consequence accepted: the hardened v1 dual-path
invariants — the `collecting` CAS, `obj = nullptr` handshake, single-batch
drain discipline, and locked intern-table access — are **kept and carried
into v2** as the price of eager RC.  The reclaimer (2.6) remains the sole
drainer for non-RT contexts, preserving v1's single-consumer fixes in
simpler form.

**Refinement (decided): RT slices run ZERO destructor work.**  v1 semantics
are actually enqueue-at-zero + drain-at-poll, and the drain is *unbounded*:
an RT slice that hits a backlog (or drops one large object graph) pays the
whole cost in that cycle.  v2 removes destruction from the RT path entirely:
a zero-crossing inside an RT slice only enqueues; the reclaimer (2.6),
running concurrently on a non-RT core, performs all destruction.  Cycles are
maximally flat — per-iteration reclamation cost is exactly zero regardless
of garbage volume or graph shape.  Costs accepted: destruction latency
becomes reclaimer latency (typically well under a cycle period), destructors
of RT-allocated objects run on the reclaimer thread (already true for
GC-swept objects today), and brief floating garbage smooths memory usage
rather than spiking cycle time.  Fallback documented in case reclaimer
offload ever proves problematic on a target: a bounded in-slice drain budget
(destroy at most K objects / T µs per slice, excess carried forward) --
*bounded* determinism instead of flat.  Non-RT threads likewise stop
draining inline; the reclaimer is the sole destructor-runner process-wide
(near-eager destruction, one well-defined destructor thread).  Wake
policy: **non-RT producers notify the collector's condvar; RT producers
never syscall** — their enqueues are picked up by the collector's existing
idle poll (10 ms today).  The guarantee is flat cycles with *bounded*
destruction latency (poll interval), not sub-cycle latency.

**Honest semantics: destruction is promptly QUEUED, not scope-tied.**  RC
still identifies garbage eagerly and keeps acyclic garbage out of tracing
collections (the determinism motivation is *flat cycle durations*, which
offload serves) — but "release before the next statement", destructor
*ordering* across producers, destructor thread affinity, and immediate
file/socket/handle closure are **not** guaranteed.  Order is **global arrival order** (the Vyukov MPSC is
FIFO in push-linearization order) — the right choice, not just the free
one: per thread it reproduces exactly the drop order eager RC produces
today (including reverse-scope order during unwind, since zero-crossings
happen in that order); parent→child dependency order emerges naturally
(children hit zero while the parent destructs and are re-enqueued); and
cross-thread order was never defined under v1 either.  (A LIFO/Treiber
stack would be a simpler push but starves the oldest entries under
sustained production — unbounded latency for unlucky objects — rejected.)
Latency is bounded by the reclaimer wake (condvar notify from non-RT
producers; the idle poll for RT enqueues — wake policy below).
Anything that needs synchronous release
gets **explicit `close()`/dispose semantics** — audit list at adoption
time: files, sockets, DDS entities (participants/readers/writers), gRPC
streams/channels, compute connections, and any native handle whose re-open
races its close.

**Retire queue mechanism (RT-safe by construction):** an intrusive
**Vyukov MPSC queue** threaded through a new `ObjControl* retireNext`
field; the collector thread is the sole consumer.  Honestly stated: push
is exchange-then-link (two steps — wait-free and allocation-free for the
producer, but the consumer must tolerate the standard Vyukov transient
where the head has been exchanged and `next` is not yet linked).
`static_assert(std::atomic<ObjControl*>::is_always_lock_free)` on hard-RT
targets — C++ atomics may otherwise hide locks.  A mutex-backed or
dynamically growing queue would reintroduce an RT blocking path.  The
consumer keeps **v1's batch discipline**: detach a complete available
batch, `dropReferences()` on every object, then destroy every object,
repeating for children newly enqueued during destruction — one-at-a-time
processing would reintroduce the cross-object destruction hazards v1
fixed.  The existing `collecting` flag remains the double-enqueue guard
(RC zero-crossing vs sweep retirement).

### 2.8 Dataflow — budgeted RT tick, structured overrun errors

**Decision (2026-07-15): no certified-plan gate.**  An earlier draft split
a pre-validated native subgraph into a separately executed "RT execution
plan" with partition/mailbox handoff.  Adopted instead — simpler, and it
keeps machinery that v1 already hardened and verified: **any island may
run on the RT tick path, under the budget.**  `tickFor(budget)` evaluates
due periodic islands inside the RT slice, yielding between FuncNodes and
bytecode instructions; blowing the budget produces a **structured runtime
error** naming the island, the offending node/builtin, and the measured
cost (extending the existing overrun-attribution logging).  Users iterate
empirically; the RT watchdog remains the hardware backstop.  (The
certified-plan + partition design is *recorded as a future option* if a
provable hard-RT subgraph is ever required — the design work is in rev
2.1's history.)

- **Drivers (unchanged from v1, kept deliberately):** periodic islands are
  evaluated by whoever drives `tickFor()` — in FC, the RT cycle callback
  (`m_hostDriven`); event-driven (0-period) islands fed by foreign
  producers (DDS readers etc.) are evaluated by the engine's actor thread
  draining the pending-event queue; script `set()` evaluates its islands
  inline on the script thread.  `m_evalMutex` serializes all three (one
  island evaluation anywhere at a time); `TickResult::Busy` is the RT
  side's non-blocking answer when the actor thread holds the evaluator.
  This coordination is verified v1 machinery and stays.
- **Advisory lint, not a gate:** when an island lands on the RT tick path,
  the engine walks its nodes and *warns* about known tail-latency hazards
  — allocating ops (string concat, dynamic-shape tensor results), locking
  builtins, I/O.  Budget errors report the *typical* case in development;
  the lint covers the *tail* case (a rare malloc stall or priority
  inversion under load that never reproduces on the bench).  The
  certification rule from open question 3 survives as the lint's
  definition — and as the implementation discipline for engine-authored
  cycle nodes (telemetry drain, FK, PD terms), which are simply written
  allocation- and lock-free, as FC's C++ is today.
- **Approved (2026-07-16): explicit execution domains.**  Each
  signal/island carries a domain, `rt` or `background`.  Defaults by
  inference: signals fed by RT sources (robot telemetry, RT I/O) → `rt`;
  event-driven/foreign-fed signals (cameras, network) → `background`; an
  island inherits `background` if any input or node is background-ish
  (tensor ops, model inference).  Explicit override at construction —
  e.g. `signal(freq, init, domain=rt)` or a lift-time annotation (exact
  Roxal surface: §7 item 11).  `rt` islands tick in-slice (lint applies);
  `background` islands evaluate on the actor thread even when periodic.
  Cross-domain edges are ordinary latest-value signal reads: an `rt` move
  condition reads a vision result's `.value`; the vision pipeline reads
  telemetry at its own pace.
- **Everything is soft-RT** (unchanged from today): Roxal bytecode — the
  robot program's `runFor()` slice, event handlers, and now RT-tick
  islands — runs on the RT thread under the deadline machinery, yielding
  between instructions.  Best-effort because a single instruction can be
  an unbounded native call and builtins may take locks shared with non-RT
  threads; accepted, because overruns surface as structured errors during
  development and the watchdog is the backstop.  In practice (FC): RT-tick
  signals stand in for I/O ladder logic and cycle telemetry — small
  scalar/fixed-shape flows inside the leftover cycle budget; heavy flows
  (tensors, vision, model inference) belong on the actor thread, which the
  proposed domain defaults encode.
- **Event-driven islands from foreign producers dispatch on the actor
  thread** — not because handler bytecode is less safe (it isn't; same
  soft-RT properties), but because event traffic is bursty: in-cycle
  dispatch makes iteration duration vary with event arrival, defeating
  the flat-cycle goal (2.7).  An RT program that must react in-cycle
  polls the signal's `.value` (a leaf read).
- **Later phase (noted, not in A–F):** offload `print()` (and any other
  I/O-performing builtin reachable from script code) from the RT thread —
  enqueue to a non-RT writer — so soft-RT slices perform no I/O even when
  scripts log mid-cycle.
- Event pumping is **iterative with a per-cycle bound** and explicit
  backpressure (drop-oldest / coalesce per signal, as today): dispatch depth
  is O(1) regardless of backlog, decoupling GC pause length from stack
  growth (the v1 1024-slot overflow becomes structurally impossible, not
  just bounds-checked).

---

## 3. Migration plan (independently adoptable phases)

Ordered by value ÷ risk; each phase lands green on the existing suite +
`gc_selftest` + `gc_coordination_stress` + the FC loaded matrices, and each
*deletes* v1 mechanisms as it lands (the deletion is the point).

| Phase | Contents | Deletes | Risk notes |
|---|---|---|---|
| **A0. Oracle + scanner prototype (shadow mode)** | collector-owned page-map oracle (v0 sourced from the global registry, 2.4) + dual-rule scanner (2.2a); runs alongside ALL existing precise roots, **deletes nothing** | — | Pure addition; scanner results logged, not trusted |
| **A1. Verification gate, then adopt for stack locals** | forced-GC comparison harness: every precise root must ALSO be found conservatively — zero misses across the roxal suite + FC loaded matrix, in optimized **and LTO** builds, before any deletion | *stack-local* root visitors only — heap-container participant visitors CANNOT go yet (conservative scan sees the container pointer, not its separately allocated elements; they wait for B) | Retention-only failures after the gate (benign); validate with quarantine mode + leak-diff runs |
| **B. `PersistentRoot`/`TracedMember` + compiler arena** | typed heap roots; arena | `ExternalRootProvider` impls, `GCNoParkScope`, module `visitRoots`, remaining participant root visitors | Mechanical migration, greppable; land module-by-module; clang-tidy rule becomes mandatory per adopted directory |
| **C. `MutatorContext` unification** | state machine, explicit `BlockingRegion` boundaries with register/SP capture (2.1), token-asserting `RTSlice` API | participants, covers, block scope, registration TLs, owner-marked parking | The rearchitecture; biggest win, touches every native boundary — gate behind a build flag until soak-equal; §6 gates must pass first |
| **D. Sole reclamation (collector phase)** | Live→Retired, collector-thread reclamation phase (2.6), Vyukov MPSC retire queue via `ObjControl::retireNext` (2.7), two-stage actor finalization via ThreadManager | inline election, ALL mutator `freeObjects` calls, `freeObjectsMutex_`, cleanup re-arm, in-slice drain cost | Destruction becomes promptly-queued (2.7): audit explicit-close needs + destructor thread-affinity assumptions |
| **E. Per-thread allocation segments** | lock-free alloc registration; oracle ingestion switches from global registry to segments | global-mutex `registerAllocation` | Truly independent of A now (oracle v0 does not need segments) |
| **F. Dataflow RT-tick UX** | structured overrun errors (island/node/cost attribution), advisory RT lint, execution domains (approved; syntax = §7 item 11), bounded iterative pump | nothing — deliberately KEEPS `m_evalMutex`/`Busy`/drain ownership (verified v1 machinery; see 2.8 decision) | Far smaller than the earlier certified-plan F; certified plan remains a recorded future option |

Phases A0/A1, B, E are retrofit-compatible with v1 (worth doing even if
C/D/F never happen).  C is the commitment point and is gated on §6; D and F
assume C.

**Recommended sub-splits** (each lands green independently):

- **A0** → *A0a* capture infrastructure (stack-bounds registration for
  every thread kind + register/SP capture at the three v1 park sites:
  `safepoint()`, `externalParticipantSafepoint()`, `GCSafeBlockScope`);
  *A0b* the oracle (registry-snapshot page map + lookup unit tests);
  *A0c* the scanner + shadow-compare counters.
- **B** → *B0* wrapper types + clang-tidy rule proven on ONE pilot module
  (ModuleDDS), then module-by-module; the compiler root arena is its own
  sub-phase (different mechanism, deletes `GCNoParkScope`).
- **C** → *C0* `ThreadManager` (pure ownership refactor, no coordination
  change — can land before B); *C1* `MutatorContext` behind the flag;
  *C2* the ~5 central `BlockingRegion` wrappers; *C3* flip the default +
  delete v1 paths after soak.
- **D** → *D1* collector-phase reclamation for swept objects only; *D2*
  route RC zero-crossings through the retire queue (the semantic change —
  isolate it for bisection); *D3* two-stage actor finalization.
- **F** → *F1* overrun attribution as structured errors + advisory RT
  lint; *F2* execution domains (once the surface syntax is decided, §7
  item 11).  The bounded iterative pump is independent of both and can
  land any time (even against v1).

**Status (2026-07-16):**

- **A0 DONE** (commit ca99eb70): capture at the three v1 park sites,
  oracle v0, dual-rule shadow scanner, `ShadowScanStats`, recall
  self-test (`sys._runtests('gc_scanner')`).  Optimized-build lesson
  banked: GCC replaces `return &local;` with NULL at -O2 — capture uses
  `__builtin_frame_address(0)`.
- **A1 DONE**: conservative marking **default ON** (kill switch
  `ROXAL_GC_CONSERVATIVE=0`).  Gates: suite green in BOTH modes, stress +
  quarantine clean (0 scan-only under stress), FC loaded matrix clean,
  recall self-test passes in the Release FC binary (register-only case
  included).  Two real bugs found by the gate: (1) pre-existing null
  deref assigning over a weak ref to a collected object
  (`Value::type()` — fixed); (2) pre-existing FC startup segfault under
  `--gc-threshold 2000` — an unrooted startup stack local; **fixed by
  conservative marking itself** (precise mode still crashes, conservative
  completes: the A/B proof of A1's value).  Cost measured: oracle build
  ≤300µs, scan ≤1.2ms per collection, on the collector thread (off-RT).
  Documented semantic consequence: prompt cycle death is no longer
  guaranteed (stale dispatch-frame slots can pin a dropped cycle);
  the two prompt-cycle-death tests are pinned precise in runtests.py and
  restructured with a churn+gc loop.
- **A1c audit outcome**: every current `setRootVisitor` lambda roots at
  least one heap-backed container (`std::vector<Value>` args/actors), so
  NO visitors were deleted — exactly the row-A1 caveat.  The whole set
  converts in Phase B; meanwhile the scan redundantly covers their
  scalar locals.
- **B0 DONE** (commit b054d2ac): `PersistentRoot<T>`/`TracedMember<C>` in
  GCRoots.h (single-mutex registration, quiescent lock-free scan, RT
  yield-section assert; missing adapter without a custom tracer is a
  compile error); ModuleDDS pilot converted — its `ExternalRootProvider`
  and hand-written `visitRoots()` deleted; interim greppable audit
  `scripts/check-raw-value-members.sh` (`// allowed-raw <reason>`
  annotations for struct fields traced via a container's custom tracer).
- **B1 (first pass) DONE** (commit e6d646eb): new `TracedRef<T>` for
  reference-semantics rooting of function locals AND thread-lambda
  captures — captures live in closure heap storage, invisible to the
  conservative stack scan, so those refs are load-bearing, not
  belt-and-braces.  Converted: all 10 Compute `setRootVisitor` sites
  (visitors deleted; participants stay for quiescence),
  `ServerActorRegistry` (map member is the root), ModuleGrpc (own
  members typed; adapter/connector internals reached via a delegating
  root until their own conversion).  Verified: both suites green in both
  modes, 14/14 remote tests in the COMPUTE_SERVER=ON build, grpc tests,
  stress, FC camera + move smoke.
- **B COMPLETE for root conversion** (commit 805123a5): Qt hubs
  (QtBindHub/QtModelHub/QtSignalHub), ProtoAdapter decl maps, Connector
  ActiveStreamState, and FC's ModuleRobot all converted to typed roots —
  and the **`ExternalRootProvider` + participant `setRootVisitor`
  mechanisms are DELETED** from SimpleMarkSweepGC (two of v1's nine
  vocabulary items gone; net −221 lines).  Verified: internal suite green
  both modes (one gc_coordination_stress harness flake did not recur over
  2 full + 3 targeted reruns), github Debug suite 730 green incl. 14
  remote + 39 qt, dds/grpc test groups, stress + both self-tests, FC
  move/camera/low-threshold smoke.  The interim audit script now checks 4
  adopted headers.
- **B FINAL — compiler roots DONE** (commit 673842e8).  Decision (David,
  2026-07-16): proceed with compiler rooting only if non-invasive, else
  keep blocking GC during compiles.  Assessment: non-invasive — the AST
  holds no Values; in-progress state is exactly `lexicalScopes` (per-scope
  ObjFunction + const bindings + module type; a `traceValues` virtual per
  scope subclass) and `importedModules`.  Implemented as two member
  `TracedRef`s — zero use-site churn (the `Scope` iterator typedef stays
  untouched).  Compiles now PARK mid-collection under conservative
  marking; `GCNoParkScope` survives ONLY as the precise-mode
  (`ROXAL_GC_CONSERVATIVE=0`) fallback, since codegen stack temps are
  scan-covered but not precisely rooted.  This also closes the real
  operational hole in the "compiles finish before RT" premise: dynamic
  imports can compile at runtime, and v1's barrier-waits-out-the-compile
  made RT slices skip for the whole compile.  Gates: gc_construct_stress
  ×3 (1 KB threshold — collections during construction+compile),
  `--recompile` full suites green in BOTH modes (no bytecode cache: every
  test compiles fresh), stress, scanner self-test, github Debug suite
  730 green, FC move + low-threshold smoke.  Audit covers 5 headers.
  **Phase B is fully complete.**  Next: Phase C (MutatorContext), gated
  on the §6 acceptance tests.
- **§6 gate progress (2026-07-16, commit 07523563)**: gate 10
  (borrowed-pointer documentation test) implemented as scanner self-test
  case 6 — an aux-storage pointer (long ObjString's heap ICU buffer) must
  NOT retain its owner; construction isolated in a dead-by-return noinline
  helper + dead-frame scrub so the assertion is deterministic.  Gates 1–3
  were already cases 1–5.  **x86-64 optimized+LTO battery GREEN**: full
  suites in both GC modes plus all gc gates against a RelWithDebInfo+IPO
  build (`build-lto/`).  Lesson recorded: the initial LTO run "failed" 3
  tests — root cause was a MISSING `ROXAL_ENABLE_REGEX=ON` in the fresh
  configure (regex-gated string methods unregistered), not LTO; when
  creating verification build trees, diff the FULL feature-flag set
  against the reference cache first.  Still open for §6: ARM64/ARM32
  runners (§7 item 10 — deferred by David 2026-07-16: complete x86 Linux
  first), and gates 5–9 which belong to phases C/D/E mechanisms.
- **C0a DONE** (commit fa9793f5): `ThreadManager` — the complete
  NON-OWNING index of every live `roxal::Thread`, self-registered in the
  Thread ctor/dtor, is now the collector's **sole interpreter-root
  source**.  Deleted: the `vm.threads` + `replThread` +
  `dataflowEngineThread` + `VM::thread` special-case gathering in
  `visitRoots` (the blind-spot family behind several v1 root holes).
  Yielded FuncNode execution threads and compute workers are covered by
  construction now, not by remembering.  Ownership deliberately unchanged
  (index, not owner — avoids the strong-registry circularity); C0b/C1
  move ownership + join authority here when MutatorContext lands.
  Gates: suites green both modes, stress + both self-tests,
  construct-stress ×3, github Debug 730 + remote 14/14, FC
  move/camera/low-threshold.  Also landed alongside (4b817e5c): the
  misleading "only object and actor instances have methods" runtime error
  now names the member and receiver type.
- **C1 DONE** (commit 316fc9d1): the MutatorContext state machine, gated
  by **runtime** flag `ROXAL_GC_V2_CONTEXTS=1` (deliberate deviation from
  the build-flag plan: one binary A/B-soaks both models with zero ABI
  risk — the exit-139 lesson).  Under v2: one context per physical thread
  (Inactive/Running/Parked/SafeBlocked), nesting is a depth counter — the
  participant map, owner-marked parking, registration counting, and the
  double-registration rules all collapse when enabled; the public API
  (ExternalParticipant/GCSafeBlockScope/safepoint) is identical so call
  sites cannot tell.  Pragmatics vs the spec: state transitions run under
  mutex_ (satisfies the §2.1 ordering table by mutual exclusion; the
  lock-free protocol is C2/C3 polish), RT yield sections keep the proven
  v1 lock-free Dekker mechanism verbatim, and the scanner keeps consuming
  the same capture records from both models.  Gates: flag-OFF suite
  identical-green (v1 regression), flag-ON suites green in BOTH GC modes,
  coordination self-test 640 collections/0 violations under v2, stress,
  github 730 + remote 14/14, FC move/camera/low-threshold.  Next: v2
  default-ON soak (C3 prep), then delete the v1 paths + participant
  machinery (C3), with the lock-free transition polish (C2) folded in
  where profiling justifies it.
- **C3 DONE** (commit 48fb82d8; soak waived by David — v1 not in active
  use; 10× stress + full batteries are the soak).  MutatorContext is now
  THE coordination model: deleted the participant map + owner-marked
  parking, `activeThreads_`/`threadsAtSafepoint_` registration counting,
  `tlExecuteRegistered_`, the v1 inline election (`collector_`), the
  GCSafeBlockScope v1 bookkeeping, and the runtime flag itself — net
  −498/+27 in the coordination core.  Gates: suites green both GC modes,
  stress ×10, self-test 624 collections/0 violations, construct-stress
  ×3, github 730 + remote 14/14, FC move/camera/low-threshold.
  NOTE (David): before finalization, a comment-scrub pass removes
  v1/v2-process vocabulary, phase labels, and design-doc references from
  code comments (tracked as the finalization item).
- **Phase D DONE** (commit adede6ce): sole-reclaimer destruction.
  Intrusive retire queue (link-then-CAS chain, FIFO by take-all+reverse;
  RT pushes never notify), mutator draining deleted (incl. interpreter
  scope-exit drains — destruction is promptly QUEUED per §2.7), collector
  idle-drains between collections, **reclaim fence** (parked mutators
  wake only after the batch is destroyed) + lifecycle-quiesce in the gc()
  builtin → script `gc()` deterministically means collected + reclaimed +
  actor teardown complete, while RT slices resume at request-clear.
  D3: ThreadManager lifecycle thread owns ALL actor joins; Thread::join
  is join-once.  Four real bugs found and fixed by the gates: the
  double-join hang; ObjCombinator's native promise-state Value edges
  (same-batch UAF); the generalized two-pass batch frees (weak-count
  batch hold — destructors all run before any block frees — closing the
  hidden-native-edge class wholesale); and the cache-load UAF
  (SerializationContext is now a self-registering GC root with strong
  retention; compile()/loadFileCache hold participant Running covers —
  the compiling thread had been Inactive and thus not waited on).  Full
  batteries green: internal both modes, github Debug 730 + remote +
  dds ×3, stress ×10 + quarantine, self-tests, FC smoke.
- **Phase E DONE** (commit 3f907a27): per-thread allocation-registry
  segments.  `registerAllocation` is **lock-free** (thread-local slot
  store + count release-publish; sealed-segment rollover CAS-pushes onto
  the global chain) — the last GC lock on the allocation hot path is
  gone.  `unregisterAllocation` deliberately stays under mutex_
  (reclaimer-side only; that lock is what makes freeing a control
  impossible mid-mark) and tombstones via ObjControl backpointers.  The
  collector compacts sealed segments and unlinks empty ones while the
  world is stopped; the oracle/epoch-reset/MVCC/sweep iterations all walk
  segments; the global `controls_` set is deleted.  Gates: suites green
  both modes, stress ×10 + quarantine, construct-stress ×3, self-tests,
  github 730 + remote 14/14 + dds 16/16, FC move/camera/low-threshold.
  Remaining: Phase F (overrun attribution, advisory RT lint, execution
  domains, bounded iterative pump), then the finalization comment scrub.
- **Phase F DONE**: the four §2.8 dataflow items, all engine-side.
  **F-a overrun attribution**: budgeted evaluation times each FuncNode;
  a Completed result past the deadline (non-yieldable stretch) and a
  period-expired resume are recorded per node (`NodeOverrun`: cost,
  overshoot, occurrence count) — one stderr warning per node per engine
  lifetime, aggregated records drained via
  `DataflowEngine::consumeNodeOverruns()` (FC's RoxalLoop appends them to
  its cycle-overrun warn).  **F-b advisory lint**: on the host-driven
  latch and each network rebuild, islands on the periodic schedule that
  contain script-closure nodes get a once-per-composition stderr
  advisory (`ROXAL_RT_LINT=0` silences).  The scan/printing runs ONLY on
  the engine thread via an `m_rtLintPending` flag — an early version
  printed from `buildNetworkCacheData` on the tick path and the stderr
  I/O alone blew a 100ms tick in the Debug self-test.  **F-c execution
  domains**: `Signal::Domain` (rt|background; declared per signal via
  `signal(freq, init, name, domain)` or chainable `sig.domain("...")`),
  island inherits Background if ANY member signal declares it; background
  islands are excluded from the global tick period and all periodic tick
  paths (tickFor/resume/tick source loops skip them via a derived
  per-signal flag) and are serviced by `serviceBackgroundIslands()` on
  the engine thread with no budget (due-time schedule per island,
  missed-period skip, idle-wait sized to the soonest due).  **F-d bounded
  event pump**: nested `set()` during island evaluation no longer
  recurses (one C++ frame per chain link → stack overflow on long
  chains); the outermost call drains a per-thread queue iteratively
  (O(1) depth, 100k-link convergence cap → clear error).  Contended
  `m_evalMutex` acquisition is now covered by GCSafeBlockScope (waiter
  was Running-invisible to the barrier while the evaluator parked at a
  safepoint holding the mutex — circular wait); RT yield-section holders
  keep the uncovered path (their slice already holds the mutex
  recursively).  RESIDUAL (pre-existing, flagged): an RT-section thread
  blocking on m_evalMutex outside its tickFor slice can still stall a
  collection against the parked evaluator — the §2.8 partition/handoff
  design is the real fix.  Also removed wholesale: **RTCallbackManager**
  (the pre-tickFor RT integration — per-instruction callback polling in
  the dispatch loop, sleep-path servicing, its ModuleSys self-test suites
  and the chronically flaky rtcallback_test); the main-thread identity it
  carried for the Qt host-event-loop pump moved to
  `VM::markMainThread()/onMainThread()`.  df_closure_multi_resume got
  margin (2Hz period — its assertion is multi-resume completion, not
  fitting 10k Debug iterations in 100ms; it sat within 1% and F-a's
  clock reads tipped it).  New tests: signal_domain, event_chain_depth.
  Gates: internal both modes 667, github Debug 732 + remote 14/14 +
  dds 16/16, stress ×10 + quarantine ×10 + construct ×3, FC move +
  low-threshold smoke.
- **Post-review fixes DONE** (commit 376c54c2; external review round 3):
  (1) The dataflow engine now has exactly ONE periodic driver besides an RT
  host's tickFor: the actor-type tick/run/runFor script methods are GONE
  (they ran inline on the calling thread -- a second concurrent scheduler;
  every use was racy incl. single-threaded-looking test use), replaced by
  the internal `_dataflow_tick()` global that queues a tick for the ENGINE
  thread and waits yieldably; DataflowEngine::runFor deleted; tick() now
  engine-thread-only, serialized under m_evalMutex with its source loop
  under m_mutex; gc_coordination_stress pump reworked to event-driven
  set() bursts (same GC interleavings, no second driver).  (2) Interning
  publish adopts a racing winner (tryIncRef under the table lock) instead
  of overwriting it -- equal-content interned strings keep one identity;
  the losing candidate dies through the retire path.  (3) Signal domain
  changes on registered signals go through
  DataflowEngine::setSignalDomain (engine m_mutex, the lock every cache
  rebuild reads domains under); the signal(...) ctor sets domain before
  registration.  (4) visitRoots iterates persistent roots UNDER
  persistentRootsMutex_ -- a root can self-register from a thread with no
  GC coverage yet (compute-server connection startup), so stopped-world
  alone doesn't exclude registrars.  (5) The collector thread exists in
  EVERY build as the sole runtime reclaimer; ROXAL_GC_DEDICATED_THREAD now
  gates only whether collections run on it.  REAL BUG caught by the new
  OFF-build gate: the reclaimer's wait predicate included
  collectionRequested_, which nothing in an inline build clears -- a
  permanently-true predicate spins in wait_for WITHOUT RELEASING mutex_,
  starving parking mutators (barrier deadlock, 19 test hangs); the term is
  now compiled out of inline builds.  Also: df_closure_multi_resume was
  re-margined properly (1Hz + 1500-resume cap -- the binding constraint
  was the RESUME CAP, not the period; fixed per-slice overhead dominates
  500us debug slices, and the subtest sat exactly at the old caps,
  flipping with unrelated binary-layout changes).  Gates: internal both
  modes 667, github Debug ON 732, github Debug
  ROXAL_GC_DEDICATED_THREAD=OFF full suite 693 (new standing gate; Qt
  feature-skipped there), stress 69/70+20/20 + quarantine 10/10 +
  construct 3/3, FC move + low-threshold smoke.
- **Sampling/diagnostic follow-up DONE** (commit after 4f869f40): three
  dataflow fixes surfaced by the review caveat + the stress-pump rework.
  (1) DataflowEngine::evaluate() stamps initialization at TIME ZERO (the
  Signal-ctor convention, and what its comment always claimed): it had
  used m_tickStart, which is the NEXT boundary while the engine sleeps --
  a FUTURE timestamp -- and sampling reads the latest-timestamped entry,
  so every func lift opened a window (one global tick period; 50ms in
  plain roxal via math._vecSignal, re-armed per lift) during which
  event-driven set() results were invisible to int(sig)/.value sampling
  even though evaluation + handlers ran.  (2) FuncNode ctors no longer
  clamp all-event-driven input frequency to 1Hz: an event island's output
  is now itself event-driven (freq 0), matching operator()(Signals);
  BEHAVIOR CHANGE: delayed indices (out[-1]) on such outputs now error
  (correctly -- event signals never supported them).  (3) The Debug
  setValueAt grid diagnostic prints to stderr (stdout is byte-compared by
  the harness) with its known benign misfire class documented (check is
  phase-relative to a concurrently rebasing tickStart; absolute checking
  would misfire on set()'s tickStart+period scheduling instead).  Gates:
  probes sample correctly with no settling wait, inline-Debug stress 0/20
  stdout diagnostics (was 15/20), suites green (693 OFF / 732 ON /
  667 both internal modes), stress 10/10 + quarantine 5/5, FC smoke.
- **Finalization DONE** (commits 5af92c83, b37bf596): comment scrub — all
  v1/v2 vocabulary, migration-phase labels and plan-doc references removed
  from code comments (incl. runtests.py and test-file comments; the stale
  runtime-gated MutatorContext block in SimpleMarkSweepGC.h deleted, state
  docs rewritten at the member); the v2* context-transition methods renamed
  (enterRunning/exitRunning/pollContext/blockEnter/blockExit/
  anyRunningLocked); implementation-notes.md GC & Thread Coordination
  section rewritten for the as-built design (retire queue, sole reclaimer,
  MutatorContext barrier, three-source root set, typed roots replacing
  ExternalRootProvider, ThreadManager join authority, gc() semantics) and
  the Signals section gained the scheduling/domain/overrun/iterative-pump
  paragraph.  Post-scrub gates: full internal both modes + github Debug
  suite green.  x86-64 Linux COMPLETE; ARM verification remains deferred
  by decision.

## 4. Compatibility & invariants

- ABI: all new state behind unconditional members / pimpls (the v1 rule);
  profiles exported via `roxal_features.cmake`; `roxal_abi` interface target
  carries the defines.
- The hard-RT contract is unchanged and becomes *checkable*: token-asserting
  APIs + the existing `sectionCollectorViolations`-style counters extend to
  "mutex acquired while holding RTSlice" (debug builds).
- Observability carries over: `ROXAL_GC_STATS`, `ROXAL_GC_QUARANTINE`,
  `sys._runtests('gc_coordination')`; add per-phase root-source counts
  (stack-scan hits vs persistent roots) and the 2.2a pinning statistics
  (operational, not just diagnostic) to spot conservatism pathologies.

## 5. Open questions

1. (Resolved: conservative scanning for locals on all native targets —
   zero API churn, no forget-hazard; portability mechanism in 2.2a.
   Shadow stacks (`Root<Value>` scopes) remain the documented fallback for
   a WebAssembly port needing in-turn collection, or a future moving
   collector; noted in SimpleMarkSweepGC.h.)
2. (Resolved: full reclaimer offload — flattest cycles.  Bounded in-slice
   draining is retained in 2.7 only as a documented fallback.)
3. (Resolved 2026-07-15, superseding the earlier certified-whitelist
   resolution: the certified plan is dropped as a *gate* — see the 2.8
   decision.  The certification rule survives in two roles: as the
   **advisory RT lint's definition** — warn when an RT-tick island
   contains nodes that are not (a) native, (b) preallocated/fixed-shape,
   (c) allocation-, lock-, and syscall-free, (d) bounded-time — and as
   the **implementation discipline for engine-authored cycle nodes**
   (telemetry drain, FK/frame ops, control terms: gain, error, clamp,
   deadband, first-order filter), which are written to that standard
   rather than validated against it.  All script bytecode runs soft-RT —
   budgeted, deadline-yielded, structured overrun errors — exactly as
   2.8 describes.)
4. (Resolved by the 2.7 decision: eager RC destruction stays, with the
   honest promptly-queued semantics stated there.)

## 6. Acceptance gates (must pass before Phase C)

Adopted from external review.  All of these run in **optimized and LTO
builds on x86-64, ARM64, and ARM32** — conservative-root behavior differs
most exactly where the optimizer is most aggressive:

1. A root existing *only* in a callee-saved register (no stack copy).
2. A root represented *only* as a raw `Obj*` (no tagged Value anywhere).
3. Tagged `const` and `weak` Value candidates (bit-normalization paths).
4. Candidate words at every legal stack alignment (incl. split 64-bit
   Values on 32-bit targets).
5. `SafeBlocked` return racing collection completion.
6. Thread creation and destruction racing a collection request
   (ThreadManager unlink-before-destroy).
7. Reclamation activity racing a subsequent collection request (the
   phase-ordering invariant of 2.6).
8. Segment rollover while inside an `RTSlice`.
9. Destructors that enqueue more objects, allocate, or assume thread
   affinity (reclaimer re-entrancy + audit coverage; tests the 2.6
   destructor contract).
10. **Expected-fail documentation test** for the borrowed-pointer contract
    (2.2a): a frame holding ONLY a pointer into an object's auxiliary
    native storage (e.g. a string buffer `data()`), with no rooted owner
    anywhere — the object is legitimately collected; the test documents
    that this is BY DESIGN and the owner must stay rooted.

Harness: where stack/register control is required these are C++ self-tests
dispatched via `sys._runtests` (the existing `gc_coordination` pattern);
the rest are `.rox` tests in the standard suite.  The A1 shadow-comparison
harness (§3) is the final gate: zero precise-roots-missed across the full
suite + FC loaded matrix.

## 7. Pre-implementation detail backlog

Known gaps to specify (or decide) before the phase that needs them:

1. *(A0a)* Capture-surface inventory: every thread kind needs stack-bounds
   registration (VM/script threads, actor workers, DDS/gRPC readers,
   compute threads, main/REPL thread, collector) and every park path needs
   register/SP capture — the three v1 park sites are `safepoint()`,
   `externalParticipantSafepoint()`, and the `GCSafeBlockScope` ctor;
   enumerate all callers.
2. *(A0b)* Oracle v0 is a per-cycle snapshot rebuild: until D, mutator RC
   frees still remove registry entries concurrently, so incremental
   ingestion is unsound.  Measure rebuild cost at FC-realistic heap sizes
   (it extends the stopped-world window); incremental ingestion arrives
   with D/E ownership.
3. *(A1)* Gate definition: v1's precise root set is NOT a stack oracle, so
   "compare against precise roots" concretely means (a) the §6 recall
   tests, where the test itself knows the object must be retained, plus
   (b) quarantine-mode + leak-diff sweeps with the candidate visitors
   disabled — not a direct set comparison.
4. *(A0/A1)* Confirm `GCNoParkScope` interaction: the barrier waits out
   no-park sections, so the scanner should never see a live no-park
   stack — assert it, don't assume it.
5. *(C0)* ThreadManager mapping table: replacement owners for
   `vm.threads`, `replThread`, `dataflowEngineThread`, and
   `FuncYieldState::executionThread` (yielded FuncNode threads become
   manager-owned; `FuncYieldState` keeps a non-owning handle).
6. *(C2)* BlockingRegion wrapper inventory: `Thread::join`, compute-future
   wait, DDS waitset, gRPC completion queue, module condvar waits — start
   from v1's `GCSafeBlockScope` and participant-poll call sites.
7. *(C)* Flag-gating seam: which entry points branch between v1 and v2
   coordination while both are in-tree.
8. *(D)* Reclaimer re-entrancy rules: define what destructors may legally
   do once the reclaimer is the sole runner (allocate? enqueue more
   zero-crossings? take module locks?) — §6 gate 9 tests whatever we
   define.
9. *(F — resolved by the 2.8 decision)* `sig.set()` keeps its inline
   synchronous semantics: the queue-submission model was dropped along
   with the certified-plan gate, and the retained v1 dual-driver
   coordination preserves today's behavior unchanged.
10. *(§6)* CI coverage: the ARM64/ARM32 optimized+LTO gates need a runner
    — decide qemu-user vs real hardware.
11. *(F2 — DECIDE, language surface)* execution-domain syntax: how user
    code marks a signal/island `rt` vs `background` — e.g.
    `signal(freq, init, domain=rt)`, a lift-time annotation, or a scoped
    construct.  Semantics are specified in 2.8 (proposed); only the Roxal
    surface needs deciding.
12. *(A0/A1)* Pinning-statistics design (2.2a): per-type counts + bytes
    retained only by stack-scan hits, exposed via `ROXAL_GC_STATS` — the
    operational alarm for conservative-retention pathologies.
