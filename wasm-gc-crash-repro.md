# The wasm GC crash: how to reproduce it, and what it is not

**Status: BOTH SOLVED.  The ~24s freeze was a collector-thread spin (section
2b).  The 7s browser crash was a SPLIT RECLAIM BATCH (section 6r): one sweep's
unreachable set was published to the retire queue ONE OBJECT AT A TIME, so the
reclaimer could destroy a child before its parent's teardown read the Value
pointing at it -- a use-after-free with no missing root involved.  Publishing
the set with a single atomic push fixes it: the reproducer that crashed at 7s
in every run now soaks 4 minutes clean, and 3 minutes at a 128KB threshold with
every detector armed.**

**Status addendum: a THIRD fault, and it was never a GC bug at all.  With the
crash fixed, the demo still stopped emitting values after ~270s (always at
`value = 11`).  That is a VALUE-STACK LEAK in `VM::invokeClosure`, the path the
dataflow engine evaluates every script node through -- see section 9.  It
reproduces natively with no browser and no collector (`collections=0`), which
is why every browser-shaped hypothesis missed it.**

* The ~24 s freeze was a self-inflicted regression (a collector-thread spin
  introduced by commit `8ce4283f`'s reclaim deferral) -- root cause proven by
  a CDP stack dump and by intervention; the one-line predicate fix is
  UNCOMMITTED, awaiting review.  See section 2b.
* The crash is the original bug and is still open, but forensic tripwires
  have now observed it directly: during the reclaim drain, dropping a retired
  object's references decRef's a child whose control block is ALREADY FREED
  (`magic=0`, `strong=-1` -- refcount underflow on freed memory).  For that
  to happen the sweep must have freed an object still strongly referenced
  from live state: **a live edge the mark phase missed**, after which
  `delObj`'s weak-release destroys the control block regardless of the
  surviving strong count.  What remains is naming WHICH type's trace() (or
  which untraced native retention) misses the edge -- the `DROP-PARENT` tag
  in the violation report answers this; the race is stochastic, so it takes
  repeated runs (observed outcomes across three identical runs: violation
  then crash at 26 s; crash without readable violation at 65 s; 200 s clean).
* The mark/reclaim exclusion (`8ce4283f`) did NOT fix the crash: the crash
  reproduced with it in place once collections actually ran.  Its earlier
  "validation" was vacuous -- every GC-enabled run of it froze at the FIRST
  collection request (the spin above), so zero collections ever completed
  under it.  Its necessity is an open question for review.
* A structural simplification was designed but DELIBERATELY DEFERRED (see
  section 7): reclamation inside the stop-the-world window.  It removes the
  entire coordination-hazard class but does not fix the mark miss, and
  changing the architecture mid-hunt would add another variable.

---

## 1. The symptom

Under SAFE_HEAP the fault is an `alignfault` inside `free()`, always reached
the same way: the collector destroys an object, and releasing one of its
copy-on-write members frees a heap chunk whose header is garbage.

```
alignfault                                   <- SAFE_HEAP check in free()
emscripten_builtin_free
std::__shared_ptr_pointer<unordered_map<int, VariablesMap::MonitoredValue>>::__on_zero_shared()
roxal::ObjectInstance::dropReferences()
roxal::VM::freeObjects()
<collector thread>
```

Without SAFE_HEAP the same corruption surfaces later and less legibly, as
`RuntimeError: unreachable`, `index out of bounds`, or
`operation does not support unaligned accesses`.

The corrupted member differs by object type but the shape never does: it is
always a COW `ptr<>` member released from `dropReferences()` on the reclaimer.
Both members seen so far:

* `ObjList::elts_` — the earlier signature
* `ObjectInstance::properties_` — the current one

That pattern (always a container member's `shared_ptr` control block, never the
`Obj` itself) is the strongest structural clue available.

---

## 2. Reproducing it

### Build and serve

```bash
# wasm build (release-ish, what the IDE ships)
./wasm/build.sh
cd web && npm run sync-wasm && npm run build
npx vite preview --port 5173 --strictPort
```

For a symbolised fault site, relink with SAFE_HEAP instead (slower, but the
stack above is only obtainable this way):

```bash
cmake -B build-wasm-dbg -DCMAKE_EXE_LINKER_FLAGS="-g2 -sSAFE_HEAP=1 -sASSERTIONS=2"
cmake --build build-wasm-dbg -j4 --target roxal_wasm
cd web && ROXAL_WASM_DIST=$PWD/../build-wasm-dbg/dist npm run sync-wasm && npm run build
```

Note: the dev server (`npm run dev`) may fail with `ENOSPC` when the machine's
inotify watch budget is exhausted; `vite preview` needs no watchers.

### The reproducers

Open the IDE, select the file, press **Run**, and watch the output pane.

| scenario | URL | typical time to crash |
|---|---|---|
| `counter4.rox` | `/?gcthreshold=4194304` | 2–8 s |
| `counter4.rox` | `/` (default 64 MiB) | 32–34 s, ~60 output lines |
| `flipflop.rox` + editor sampling churn | `/?gcthreshold=4194304` | 6–21 s |

The sampling churn means calling the Df store repeatedly while the app runs,
which is what the diagram editor does for live values:

```js
const df = window.__rox.roxalStore('df');
setInterval(() => df.call('sample', '/data/flipflop.rox').catch(() => {}), 300);
```

`counter4.rox` at the default threshold is the most deterministic case: it has
reproduced at 32–34 s across many builds and code changes, which makes it the
best A/B subject.

### Diagnostic switches (URL query, wired in `web/src/roxal.js`)

| flag | effect |
|---|---|
| `?nogc=1` | disables collections entirely — **the app then runs indefinitely** |
| `?gcthreshold=BYTES` | auto-trigger threshold; 4194304 makes collections frequent |
| `?gcprecise=1` | force precise mode (already the wasm default) |
| `?gcshadow=0\|1` | conservative shadow scan on/off |

`?nogc=1` is also the current **workaround** for demos. It is not a fix: memory
only grows, and a long session eventually dies with `std::bad_alloc`.

**Provenance rule (learned the hard way):** every browser probe must record
`ccall('roxal_build_info')` next to its verdict. `npm run dev`/`build` re-run
sync-wasm themselves without your ROXAL_WASM_DIST, and sync-wasm skips
older-than-destination sources -- both can silently leave a different binary
deployed than the one you built. An entire validation series was invalidated
this way; a second failure mode (a cmake dir configured native by accident,
producing no wasm at all, its sync failing behind an output filter) produced
the same effect.

---

## 2b. The ~24 s freeze: SOLVED (a self-inflicted spin)

**Root cause** (proven by a CDP stack dump of all workers at the frozen
moment, then confirmed by intervention): commit `8ce4283f` taught the
collector thread to defer reclaim drains while a collection is pending.  But
the collector's idle wait is `safepointCv_.wait_for(lock, 10ms, pred)` with
`hasRetiredObjects()` in the predicate -- and the deferred drain never clears
that term.  A `wait_for` whose predicate is already true returns immediately
WITHOUT RELEASING THE MUTEX, so from the first collection request onward the
collector hot-spins holding the GC `mutex_`.  Every mutator then blocks at
`pollContext`'s mutex acquisition, the barrier can never observe them parked,
the collection never starts (`req=1`, `inProg=0`, `collections=0` forever),
and the whole VM stops.  The stack dump showed exactly this population: the
collector executing (caught in its clock call), four threads blocked in
`mutex::lock` inside `pollContext`, the rest parked or idle.  The file's own
comment block had warned about this precise spin mode for the predicate's
OTHER term.

**The fix** (uncommitted): make the predicate deferral-aware -- retired
objects only count as a wake reason when the drain is actually allowed to
run.  With it, collections request AND COMPLETE (first time all session),
and no freeze occurs in any scenario.

**Timing forensics that misled for half a day**: the freeze fired at the
first collection request (64 MiB allocated), which is activity-proportional
-- ~32 s with the canvas's 400 ms sampling, ~267 s without.  While the URL
GC switches were silently broken (section 3), "?nogc=1" runs still froze,
which manufactured a false "GC-independent freeze" narrative; with verified
treatments, GC-off runs never freeze.

**Interaction with the crash**: a wedged VM stops allocating and therefore
cannot crash.  Every crash observation window on a freezing build was
truncated at the freeze point, and any "clean" verdict from such a run is
evidence only up to that point.
## 3. Measurement discipline (read this before trusting any result)

**A frozen VM cannot crash.** An earlier round of this investigation concluded
the bug was fixed on the strength of soak runs that only watched for crash text.
Those runs were measuring a deadlocked app: collections had stopped happening,
so nothing crashed, and "clean" was indistinguishable from "hung".

Every probe must therefore assert **liveness as well as absence of crash**:
count output lines over time and treat "no new output for N seconds" as a
failure distinct from a crash. The scenarios above all print continuously
(`value = …` for counter4, `q = …` for flipflop), so line growth is a direct
liveness signal.

Second rule: **verify the switch you are testing is actually live.** One
experiment here read its environment variable through a function-local static
evaluated on the collector thread's first loop iteration — which happens at VM
construction, before the host can set the variable. It latched "off" and the
experiment measured nothing, while producing three plausible identical timings.
Identical-to-the-second results across a control and its treatment are a symptom
of an inert switch, not of a falsified hypothesis.

Third: crash times are noisy (2 s vs 8 s at the 4 MB threshold means nothing).
Only large moves, or repeated clean runs well past the baseline, are evidence.

Fourth: **DOM output is not VM liveness.** Counting lines in the output pane
tells you the browser is still receiving prints; it cannot distinguish a
healthy VM from one that wedged with its last prints already rendered. The
probe that actually answers "is the VM alive" is a store call —
`df.call('sample', '/data/<file>.rox')` — which round-trips into the VM and
returns live wire values. Use it alongside the output count; several
conclusions in this investigation had to be retracted because a probe watched
only the DOM (or only for crash text).

---

## 4. What has been ruled out — and how

**Validity warning (added after the provenance findings):** several of these
tests depended on URL switches routed through `roxal_config` and on deploys
made before the sync/provenance traps in section 3 were understood.  Any row
whose treatment cannot be shown to have been live at the time should be
treated as UNCONFIRMED rather than falsified, and re-run on the current
instrumented build (which verifies treatments by readback) before being
relied on.  The two rows known solid are the `?nogc=1` exoneration
(re-confirmed with verified `gcOn=0` readback on 2026-08-15) and the
native-clean row (native binaries, no deploy layer).

Each of these was measured, not argued. All still crash at baseline timings.

| hypothesis | test | result |
|---|---|---|
| Premature sweeping of live objects | mark, but never retire unmarked objects | still crashes (2 s) |
| MVCC version-chain trimming | skip `trimVersionChain` during mark | still crashes (4 s) |
| Plain refcount-driven frees | `?nogc=1` (no collections at all) | **clean for minutes** |
| Untraced in-flight dataflow evaluation state | trace `FuncNode` call args across `invokeClosure` | identical (32 s) |
| Non-atomic sweep death-claim | make the claim an `exchange` (kept — see §5) | identical (2 s / 21 s) |
| Reclaimer/marker overlap | mutual exclusion, both directions (kept — see §5) | identical (4 s / 34 s) |
| Marking performed by a mutator thread | build with `ROXAL_GC_DEDICATED_THREAD=ON` | identical (4 s / 21 s) |
| Conservative vs precise root scanning | both modes | both crash |

The one fact that survives all of it: **collections are necessary and
sufficient to trigger the corruption.** Disable them and the app runs; enable
them in any configuration and it dies.

---

## 5. Configuration facts worth knowing

* **`ROXAL_GC_DEDICATED_THREAD` is OFF for wasm.** The default is ON only when
  `CMAKE_SYSTEM_NAME == Linux`, and Emscripten is not. So on wasm, tracing
  collections run **inline on whichever mutator self-elects at a safepoint**
  (in practice the dataflow engine's actor thread), while the collector thread
  exists solely to reclaim. On native Linux both run on the collector thread.
  This is the clearest structural difference between the crashing platform and
  the stable one — but note the table above: forcing the native arrangement on
  wasm does *not* fix the crash.
* **The collector thread is started unconditionally** (`VM::VM` →
  `startCollectorThread`), in every build profile, because refcount
  zero-crossings only *enqueue* — something must drain them between
  collections, and RT slices must stay destructor-free. A genuinely
  single-threaded target therefore still gets a thread today; moving the drain
  inline for such targets is an open design question, independent of this bug.
* **Wasm cannot support conservative stack scanning.** Locals live in wasm
  SSA registers outside linear memory and there is no register-spill
  primitive, so a `Value` held only in a C++ local is invisible to the
  collector. Precise mode is therefore the wasm default, with `GCNoParkScope`
  covers over native regions that hold Values across a possible safepoint.
* **Covers must never span a long-running native.** A covered thread stays
  `Running` through safepoints so the barrier waits for it. Wrapping the whole
  actor-dispatch native call in a cover froze the app permanently at the first
  collection, because the dataflow engine's run loop is dispatched that way and
  polls safepoints internally. Covers now wrap only the post-call region.

---

## 6. The crash: what the tripwires actually caught

The earlier "convergent explanation" (reclaimer drain racing the mark phase
on COW members, fixed by mark/reclaim mutual exclusion) did NOT survive
contact with a working garbage collector: once the freeze was fixed and
collections completed, the crash reproduced with the exclusion in place,
during the SECOND collection's reclaim drain (`collections=2, drain=1`,
engine locks held by resumed mutators).

The forensic tripwire then observed the corruption directly.  During a
drain, `dropReferences()` on a retired object decRef'd a child whose control
block was already freed:

```
GC FORENSIC VIOLATION at retireObject | magic=0x0 strong=-1
weak=<garbage> dropCount=0 retireCount=1 deathPath=1 objType=<garbage>
(during drain=1, collections=1)
```

`strong=-1` is a decRef underflow on freed memory.  A control block is freed
only when its weak count reaches zero -- and `delObj` drops the implicit
weak that "represents strong refs" when the reclaimer destroys a swept
object, WITHOUT regard to the actual strong count.  So the observed state
requires: the sweep freed an object that live state still strongly
referenced, i.e. **the mark phase missed a live edge**; the stale holder's
later decRef (here: a subsequent drain dropping the holder) is the fatal
touch.  This is consistent with every historical signature: corruption
always surfacing in `free()` on member control blocks, wasm-only (precise
mode has the narrowest root coverage), timing dependent on allocation and
death patterns, native (conservative scan) always clean.

**ATTRIBUTION LANDED (2026-08-15, attempt 3 with containment)**:

```
DROP-PARENT obj=0xa15ad18 type=5 (ObjectInstance)
child ctrl=0x88  magic=0  strong=-1  objType=240(garbage)
```

The retired parent is an ObjectInstance, and the child Value in its property
storage was GARBAGE BITS (control pointer 0x88), not a dangling once-valid
pointer.  The instance's property storage itself contained corrupted Values
at drop time -- which re-converges with the original fault signature: an
instance's `properties_` (and a list's `elts_`) is a COW shared_ptr, shared
with MVCC frozen snapshots.  This workload snapshots instances constantly
(the web store publishes instance state every turn; actor-boundary returns
freeze results).  If the SHARED CONTROL BLOCK's count goes wrong under that
churn, the buffer frees while the live instance still points at it, the
allocator recycles it (small ints like 0x88 are what a recycled byte buffer
looks like), and the eventual drop reads garbage Values from reused memory.

**Next square, precisely**: the `properties_`/`elts_` shared-count lifecycle
under snapshot churn -- `shallowClone`, `createFrozenSnapshot`, snapshot
death, and `cowLock_`'s `activeSnapshotCount > 0` gating.  Fastest probable
kill: a NATIVE TSan run of store-style snapshot churn (shallowClone + clone
death racing property mutation), now that the exact structure to churn is
known.  (An earlier unconditional-CowGuard experiment "falsified" this area,
but that run predates the provenance discipline and is unconfirmed -- see
the section 4 warning.)

The earlier open question -- superseded by the above: which type's `trace()` (or which
native retention outside any traced root) misses the edge.  The violation
report now carries a `DROP-PARENT obj/type` tag naming the retired object
whose drop touched the corpse; that is the holder of the stale edge, and its
type collapses the search to one class's outgoing edges.  The race is
stochastic (three identical runs: violation+crash at 26 s / crash without
readable report at 65 s / 200 s clean), so landing a tagged report takes
repeated runs.  All instrumentation for this is built and deployed.

## 6b. The native TSan attempt (2026-08-15): one real hole, three closed channels

The plan from section 6 was a native TSan run of snapshot churn.  It ran.  It
did NOT reproduce the corruption -- but it closed candidate channels with
evidence and exposed one genuine unguarded window.  Both matter more than a
null result suggests, so record them precisely.

**Build**: `cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"`.  TSan aborts on this kernel
with "unexpected memory mapping" unless ASLR is off for the process: run it
as `setarch $(uname -m) -R build-tsan/roxal script.rox`.  (ASan and TSan are
mutually exclusive; `build-san` stays ASan+UBSan.  The ASan binary needs
`LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.8` here.)

### The hole: `createFrozenSnapshot()` clones the root with NO cow lock

`lockCow()` is acquired in exactly ONE place in the entire tree:
`resolveConstChild` (`compiler/Object.cpp:770`).  Mutation methods take
`CowGuard` around `saveVersion + ensureUnique + mutate + epoch bump` -- which
defends the COW pointer against *that* one reader.  But
`createFrozenSnapshot()` shallow-clones the **root** object
(`compiler/Object.cpp`, `auto snap = obj->shallowClone();`) holding no lock at
all.  So a thread freezing a root races a mutator reassigning that root's
`properties_` / `elts_` inside `ensureUnique()` -- read and write of the same
non-atomic pointer word, no happens-before edge between them.  This is exactly
the shape the wasm attribution points at.  What is NOT yet established is a
reachable pairing (see closed channels); the fix, when it comes, is either
taking `cowLock_` in `createFrozenSnapshot` like `resolveConstChild` does, or
proving no caller can be concurrent.

### Verified, unrelated, real: a data race on `ActorInstance::thread_id`

TSan flags this on every run containing an actor:

    Write  Thread.cpp:357   actorInst->thread_id = std::this_thread::get_id();   (actor thread)
    Read   VM.cpp:3315      if (std::this_thread::get_id() == inst->thread_id)   (caller thread)

A plain `std::thread::id` member, written by the actor as it starts and read
by callers to decide self-call vs queued-call, with no atomic and no fence.
Not the GC bug; a genuine race on the actor dispatch decision.

### Channels closed by experiment (scripts in the session scratchpad)

- **Actor return of a list holding an interior instance the actor keeps**:
  the caller gets a DEEP COPY, so nothing is shared.  `identity_probe.rox`:
  main's `shared.a` stayed `0` across 3,000,000 actor writes.  This is the
  documented isolation rule (implementation-notes.md:2245, "if the root is
  sole-owner but interior objects are aliased within the actor's state, the
  return value is deep-cloned") working as designed.
- **`wait(for=fut)` anywhere before the racing loop**: creates a
  happens-before edge that erases the race by construction, and it waits for
  *completion* even when given `ms=` (see `wait_builtin`: a future target sets
  `pendingWaitFor` regardless of the delay).  This silently invalidated three
  earlier runs that looked clean.
- **A DF `FuncNode` reading a module-level instance**: the instance is
  snapshotted once at wiring, not per tick -- the node's output stayed
  constant while the instance advanced through 24,000,000 mutations.  No
  per-tick clone, so no race on this path.

### Method discipline (this cost most of the session)

An overlap harness proves nothing unless it demonstrates BOTH:
(i) **temporal overlap** -- interleaved progress prints from both threads, and
(ii) **actual sharing** -- one thread's writes are visible to the other.
Three runs satisfied neither and reported zero races, which reads exactly like
"the code is fine".  Also: native actors *do* run concurrently with a
compute-bound main thread (`concurrency_probe.rox` interleaves cleanly) -- the
serialization seen earlier was the `wait()` edge plus a race-amplification
spin probe so large it crippled the mutator, not a scheduling limitation.

### The signal-payload channel: also closed (same snapshot-once effect)

`dataflow/Signal.cpp:28` (`snapshotForSignal`) freezes list/dict payloads but
**deliberately passes Object/actor payloads through unfrozen** ("e.g. DDS
message flows"), so it looked like the one place isolation is waived.  Tried
it (`signal_object_probe.rox`: a node returns a module-level instance into a
200 Hz signal, another node reads its property, main hot-mutates the
instance).  The engine-side read stayed `0` through 4,000,000 mutations --
the module variable is snapshotted once at wiring, so the engine never holds
the live object.  Closed, for the same reason as the FuncNode channel.

### What this reframes (the honest consequence)

Every *script-level* channel tried is closed by Roxal's isolation design:
aliased-interior returns deep-clone, module variables reach dataflow nodes as
one-time snapshots.  If ordinary scripts cannot share a mutable
`ObjectInstance` across threads, then the "COW shared-control-block
corruption under snapshot churn" branch of section 6 needs a **runtime-side**
sharing path to stay viable -- the web store / inspect mirrors / the `Df`
actor's document trees crossing the worker boundary -- rather than anything a
.rox program does.

Correspondingly, the ORIGINAL explanation regains standing: a mark miss lets
the sweep free an `ObjectInstance` that is still referenced; its property
buffer is recycled by the allocator; the later drop reads garbage Values out
of reused memory.  That produces `ctrl=0x88`-style garbage identically, and
it does not require any script-level race at all.  Section 6's evidence does
not distinguish the two; today's results tilt toward this one.  Note also
that the unguarded root clone above is a real hole regardless of which branch
explains the crash -- it just may not be *this* crash.

## 6c. The freeze reproduces NATIVELY — and the first fix was over-broad

Instrumenting the mark phase (next section) required a forensics build, and
building it in the **wasm GC configuration** turned the whole investigation
around.  `ROXAL_GC_DEDICATED_THREAD` is a compile-time option that defaults ON
only on Linux, which is why wasm behaves differently from every native run all
week.  Configure it OFF natively and you get the wasm collector semantics with
gdb, ASan and TSan available:

    cmake -B build-wasmlike -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DROXAL_GC_DEDICATED_THREAD=OFF -DCMAKE_CXX_FLAGS="-DROXAL_GC_FORENSICS=1"

(`runtests.py` now honours `ROXAL_BUILD_DIR` so the suite can run against it.)

**The freeze has a 30-line deterministic reproducer.**  With the ORIGINAL bare
`hasRetiredObjects()` wake predicate, `tests/gc_liveness.rox` HANGS every time
under this build.  No browser, no timing window, no 24-second wait: the
permanently-true predicate spins `wait_for` while holding `mutex_`, exactly as
section 2b describes.  That is now a regression test waiting to be written.

**The first fix was over-broad and the suite caught it.**  Deferring the drain
whenever a collection was *requested or running* suppressed reclamation for
the entire request-to-election window -- which in an inline build lasts until
some mutator reaches a safepoint.  `gc_liveness` failed on its 9th line: a
dead parent's transitive drop had not run, so its child outlived its last
reference and `_weak_alive` still reported `true`.  Deterministic refcount
reclamation is a language guarantee, and that test exists to protect it.

**Corrected fix**: defer the drain only while `collectionInProgress_`, on BOTH
the drain side and the collector's wake predicate (they must mirror each other
or the spin returns).  Nothing is traced before a collection starts, and the
start handshake already waits out an in-flight drain, so excluding only the
running window is both sufficient and necessary.

Result: `760/760` in the inline-GC (wasm) configuration with verification
enabled, and `872/872` in the dedicated-thread configuration.

## 6d. The mark-verification instrument (`ROXAL_FC_VERIFY`, bit 3)

Between "mark is final" and "the sweep claims its first victim" there is one
instant at which the question *"is this sweep about to free something still
referenced?"* can be answered BEFORE the damage.  The pass walks every live
(marked) object's strong edges and reports two impossible-in-a-correct-heap
conditions, naming the PARENT that owns the bad edge:

- **MARK-MISS** -- a live parent points at an unmarked child, which the sweep
  is about to free out from under it.
- **DYING-CHILD** -- a live parent points at a child already claimed dying,
  i.e. a refcount reached zero while a live parent still held it.  That is
  precisely the `strong=-1` signature in section 6's report.

This fires at the CAUSE rather than at the eventual use-after-free, so the
report names a type instead of a corpse.  It costs O(live objects x edges) per
collection, hence its own flag bit; enable with `ROXAL_FC_FLAGS=15` natively
or `?fcflags=15` in the browser.  Validated across the entire suite in the
wasm GC configuration: **zero false positives**.

Native reproduction of the browser workload (the counter4 network plus
continuous `inspect` mirror churn, with an actor holding parsed trees, on a
2-4 MB threshold) has so far stayed clean -- the instrument is ready but the
native analogue has not yet provoked the fault.  The next move is the browser:
build wasm with forensics, run the section 2 reproducer with `?fcflags=15`,
and read `roxal_forensic_report()`.

## 6e. ROOT CAUSE, reproduced natively: edges created DURING a collection

Running the browser workload's shape against the native inline-GC build
(counter4-style network + `inspect` mirror churn + an actor holding parsed
trees, `--gc-threshold 2048`) crashes in **seconds, on nearly every run**:

    scratchpad/wasmlike_repro.rox
    ROXAL_FC_FLAGS=15 build-wasmlike/roxal --gc-threshold 2048 wasmlike_repro.rox

Three independent tripwires fire on the SAME object in the SAME epoch, which
is what makes this an attribution rather than a correlation:

    GC ENQUEUE-DURING-COLLECTION: actor=0x607403024740 thread=... nopark=0 yieldsec=0 extparticipant=1
    GC MARK-MISS at epoch 9 | LIVE parent obj=0x607403024740 type=6(Actor)
        -> DOOMED child obj=... type=1(BoundMethod) strong=1 weak=1 markEpoch=0
        | parentTracedEpoch=9 (TRACED: edge appeared after tracing)
        | 22 such edge(s) this collection

and the crash stack (gdb, `build-wasmlike`):

    Obj::incRef  <- control->strong.fetch_add on FREED memory
    Value::Value(const Value&)
    ActorInstance::MethodCallInfo::MethodCallInfo(const MethodCallInfo&)
    std::deque<MethodCallInfo>::operator=            <- forEach COPIES the queue
    atomic_queue<MethodCallInfo>::forEach
    ActorInstance::trace                              (Object.cpp:6848)
    MarkWorklist::drain
    SimpleMarkSweepGC::performCollection

**The mechanism, start to finish:**

1. A thread registered as a GC **ExternalParticipant** (every actor thread
   holds one for its whole lifetime -- Thread.cpp:342 -- as do host covers)
   calls `ActorInstance::queueCall` **while a collection is in progress**.
   The tripwire proves the barrier did not hold it: `nopark=0 yieldsec=0`.
2. `queueCall` stores new strong Values into the actor's `callQueue`: the
   `BoundMethod` callee, the argument Values, the return future.
3. The mark had ALREADY traced that actor (`parentTracedEpoch == epoch`), so
   those Values are never marked -- `markEpoch=0`, and they are reachable
   only from the queue (`strong=1`).
4. The sweep frees them: unmarked, not claimed, therefore garbage by
   definition. Tens to hundreds per collection (22-234 observed).
5. The actor's queue now holds dangling Values. The next `trace()` walks it
   -- and `atomic_queue::forEach` (core/atomic.h:901) **copies the whole
   queue**, so it `incRef`s every queued Value -- `control->strong.fetch_add`
   on freed memory. Native: SIGSEGV. wasm: silent heap corruption surfacing
   later in `free()`, which is the fault this document opens with.

This subsumes the section 6 attribution: the corrupted parent was an instance
whose property storage held garbage Values, and the same "edges created after
the parent was traced" mechanism produces exactly that.

**A second, independent defect** shows up in the same runs (its own tripwire,
sometimes first):

    GC FORENSIC VIOLATION at DOUBLE dropReferences (inline+queued)
      dropCount=2 retireCount=1 deathPath=1(refcount) objType=15(Type)

An object dropped BOTH inline (refcount zero-crossing) and again from the
retire queue -- the double shared_ptr release the sweep's own comment
predicts. Then `newObj unique_ptr destroyed without releasing` as the heap
unravels.

**Design flaw worth fixing regardless**: `atomic_queue::forEach` copying the
queue means `trace()` -- the mark's hot path -- performs refcount traffic on
every queued Value, and a `decRef` from the temporary copy dying can retire an
object *during the mark*. A visitor that walks under the lock without copying
removes both the cost and that hazard.

**Open design decision (David's call)**: how to close the barrier hole. The
options are (a) make `queueCall` (and other participant-side heap mutation)
park when a collection is in progress, (b) gate `enterRunning()` on
`collectionInProgress_` so a participant cannot re-enter Running mid-collection
-- it currently has no such check -- or (c) the section 7 restructure, which
removes the class by construction. No code change has been made here.

## 6f. The fixes: native repro GONE, wasm crash REMAINS (and is now narrowed)

Changes made (all uncommitted, see section 8):

1. **Drain deferral corrected** -- defer only while `collectionInProgress_`,
   mirrored on the drain side and the collector's wake predicate.
2. **Barrier hole closed** -- `worldStopped_` is published under `mutex_` at
   the instant the barrier admits a collection (BEFORE the reclaim-drain wait
   releases the lock), and `enterRunning`/`blockExit` wait on it.  Gating on
   `collectionInProgress_` alone was NOT enough: it is set later, inside
   `performCollection`.
3. **Collector double-election fixed** -- the role (`externalCollectorActive_`)
   is claimed under the same lock hold as the barrier decision, and the
   electing branch re-checks it after every wake.  Two threads could otherwise
   both pass the barrier and mark the same heap concurrently.
4. **`atomic_queue::forEach` walks in place** (no queue copy, so no
   incRef/decRef of every queued Value on the mark's hot path).
5. **`queueCall` builds the call outside `queueMutex`** (removes the
   queueMutex -> GC-mutex inversion against `trace()`'s GC-mutex -> queue-lock).
6. **`dropReferences` is one-shot** via `ObjControl::dropClaimed`.

**Native result: fixed.**  `wasmlike_repro.rox`, which crashed on nearly every
run, is now 8/8 clean with every tripwire enabled.  Suites: 760/760
inline-GC, 872/872 dedicated.

**Browser result: the crash REMAINS.**  On a forensics build with
`?gcthreshold=4194304&fcflags=15`, counter4 still faults at ~7 s with the
original signature -- `free()` inside
`__shared_ptr_pointer<unordered_map<int, MonitoredValue>>::__on_zero_shared()`,
under `ObjectInstance::dropReferences()`, under `VM::freeObjects()`, on the
collector thread.

That negative is **interpretable**, which earlier ones were not: the report
reads `(no violation; fcflags=0xf)`, so the checks were provably live.  With
mark verification enabled and silent, the browser crash is therefore:

* NOT a mark miss (no live parent pointed at anything the sweep freed, in any
  collection of that run), and
* NOT the inline+queued double drop (that tripwire was armed too).

So the wasm fault has a cause the current instruments do not cover.  The
strongest remaining hypothesis is the one the header of `SimpleMarkSweepGC.h`
already states: **wasm cannot expose wasm locals to any scanner**, so a Value
live only in a native/VM C++ frame is invisible to the mark -- it has no
traced parent, which is exactly why the verification pass cannot see it
either.  Second candidate: `createFrozenSnapshot` shallow-clones the ROOT with
no `cowLock_` (section 6b) -- unproven but never excluded.

**Next instruments** (in order of expected value):
1. A tripwire on the RECLAIM side: before `ObjectInstance::dropReferences()`
   releases `properties_`, validate the shared control block (use_count sanity
   / poison magic).  That faults at the corrupting release rather than after.
2. Re-run under SAFE_HEAP for the precise fault site with the fixes in place.
3. Root-coverage audit of the wasm path specifically: which Values live in
   C++ frames across a safepoint without a cover.

## 6g. Second wasm pass: five more hypotheses excluded, with the switches proven live

The instruments were extended and the browser reproducer re-run.  Everything
below is provenance-checked at the ARTIFACT level, because "is the binary I am
running the binary I built?" has invalidated more results in this
investigation than any technical mistake:

* `roxal_build_info()` now carries **`gcrev=N`** (`ROXAL_GC_COORD_REV`, defined
  in `SimpleMarkSweepGC.h`).  The git hash is configure-time-stale and
  uncommitted fixes never move it, so without this a stale wasm and a fixed
  one read identically.  The run below reported `gcrev=2`.
* The served `roxal.wasm` was fetched from the preview server and searched for
  strings unique to each new detector -- all present.  (Symbol search alone is
  not enough: `dropReferencesOnce` is absent from the artifact because it is
  inlined, while `dropReferences` appears 23 times.  Validate the method
  against a symbol you know is there before trusting a zero.)
* `roxal_forensic_report()` returns `(no violation; fcflags=0x..)` when empty,
  so a silent run states the switch value that produced the silence.

**Instrument gap found and fixed**: the ENQUEUE-DURING-COLLECTION and
BARRIER-VIOLATION tripwires were `fprintf` guarded by `#ifndef __EMSCRIPTEN__`
-- silent in the ONE environment whose crash is open.  They now go through the
buffered reporter, so a browser run reports them.

**Result: the crash still reproduces at ~7 s, `(no violation; fcflags=0xf)`,
`gcrev=2`.**  With every check live, the wasm fault is NOT:

1. a mark miss (no live parent pointed at anything swept),
2. the inline+queued double drop (`dropClaimed` guards it, tripwire armed),
3. a barrier violation (no context Running at any collection start, no actor
   call enqueued during a collection),
4. a COW race between `createFrozenSnapshot`'s unguarded root clone and
   `ensureUnique()` (detector via `ObjControl::cloneInProgress`, silent),
5. an implausible `properties_` share count at the moment of release (checked
   immediately before the release that traps).

So the sweep is behaving CORRECTLY by its own rules and the memory it frees is
still referenced by something the collector cannot see.  That is precisely the
limitation `SimpleMarkSweepGC.h` documents: **wasm cannot expose wasm locals
to any scanner**, so a Value live only in a native/VM C++ frame has no traced
parent -- invisible to the mark, and equally invisible to a verification pass
that walks marked parents' edges.  The header already names the two sound
answers: defer collections to host-loop turn boundaries (no native frames live
=> precise roots suffice), or adopt precise shadow-stack rooting for the wasm
profile.  The `GCNoParkScope`/`ScopedGCMutatorCover` covers are an
approximation of the first, patched hole by hole; this crash is the residue.

**Inconclusive experiment, recorded so it is not repeated**: an
`ROXAL_FC_LEAK` bit (16) makes the sweep identify garbage but never reclaim
it.  The intent was a clean bisection -- if the fault vanishes, the freed
memory was still referenced.  It does not work: with nothing ever reclaimed
the live set grows monotonically, every collection costs more than the last,
and the VM wedges in a collection storm before reaching the crash window (at
both 4 MB and default thresholds).  The flag is left in place but the leg is
inconclusive by construction, NOT evidence of a freeze.

**SAFE_HEAP leg (run, with the fixes in place -- `gcrev=2`, 7.3 MB artifact
verified served)**: same site, no earlier trap.  SAFE_HEAP aborts INSIDE
`free()` validating the chunk header, under
`__on_zero_shared()` -> `ObjectInstance::dropReferences()` ->
`VM::freeObjects()` on the collector thread, ~7 s.  No instrumented load or
store in Roxal code faulted before it.

That is a meaningful limit of the tool rather than a surprise: SAFE_HEAP
checks alignment and bounds, it has no redzones and no quarantine, so a write
through a stale pointer that lands INSIDE a live (recycled) allocation passes
silently and only surfaces when the allocator later validates that chunk.  A
use-after-free of this shape is exactly what it cannot name.

**Next**: **ASan for wasm** (`-fsanitize=address`, `build-wasm-asan/` already
exists) is the one instrument that can name the corrupter -- it quarantines
freed chunks and reports allocation + free stacks at the offending access, so
a stale write lands on the culprit rather than on the victim.  Independently
of the diagnosis, the root-coverage question above is a design decision:
either collections may only be performed when every parked thread parked at a
PRECISE point (interpreter safepoint, not inside a native frame) -- failure
mode becomes "collection deferred" instead of "live object swept" -- or the
wasm profile adopts precise shadow-stack rooting.

## 6h. HEADLESS reproducer, and the fault named on the ACTOR side

The browser is no longer required.  The wasm build is `MODULARIZE=1` +
`PROXY_TO_PTHREAD=1`, so `node roxal.js script.rox` prints nothing (the file
only defines `createRoxal`), which is why this was never tried.  Driving it
properly gives a headless reproducer that faults in well under a minute:

    cd build-wasm-dbg/dist     # or build-wasm-mt/dist
    node wasm-gc-probes/run-wasm.mjs "$PWD" wasm-gc-probes/wasm_repro.rox 180

The driver (`wasm-gc-probes/run-wasm.mjs`) must: let `main()` run (it calls
`serveInbox()`; `noInitialRun` leaves submissions unread), submit through
`roxal_submit_source` (direct `roxal_run_source` is refused off the VM
thread), and run with the dist directory as cwd (`roxal.data` is loaded
relative to it).

**Plain build**: `operation does not support unaligned accesses` -- the
original signature from day one -- at ~round 500-1000, with the stack in
`inspect.unparse` -> `InspectBuild::buildFunction` ->
`shared_ptr<ast::Function>::reset`.

**SAFE_HEAP build**: a segfault with a sharper stack, on the ACTOR thread:

    segfault  <- SAFE_HEAP
    std::__shared_weak_count::lock()
    roxal::ObjFuture::wakeWaiters()          (Object.cpp:3436, w.thread.lock())
    Thread::act(...)

`wakeWaiters` iterates `waiters` and locks each weak reference.  A segfault
inside `lock()` means the control block it reads is freed -- i.e. the
`waiters` vector is freed memory, i.e. **the `ObjFuture` itself is dangling
while the actor thread is inside its method**.

That is the same class at a second, independent site, and it names the
mechanism concretely.  In `Thread::act` the dispatch loop holds the in-flight
call's Values -- callee, args, and the result future -- in **C++ locals**.
`Thread.cpp:342`'s own comment says these are "outside any vm.execute() frame
-- invisible to the collector, they can be swept while live on this C++
stack", and makes the thread a persistent ExternalParticipant to control WHEN
it parks.  But participation only decides when a collection may start; it does
not make those Values traceable.  Natively the conservative stack scan finds
them.  On wasm nothing can: locals are unscannable.  So a collection that runs
while the actor is parked at a safepoint inside `execute()` sweeps the
in-flight call's future, and `wakeWaiters()` then walks freed memory.

**Targeted fix available** (much smaller than the global redesign): keep the
in-flight `MethodCallInfo` in a TRACED slot on the actor/thread for the
duration of the call -- visited by `ActorInstance::trace` -- instead of only
in C++ locals.  The queue already traces entries; the hole is precisely the
window between pop and completion.  The headless reproducer verifies it in a
minute.  The same reasoning applies to any other native frame that holds the
only reference to a Value across a safepoint, which is the general form of the
design decision in 6g.

**ASan on wasm: not usable here.**  The artifact is ~110 MB and never boots in
the browser (assets 200, module never instantiates, 8 pthread workers x 1 GB
initial memory).  Headless it boots, but it is far too slow to reach the crash
window (no workload rounds in 240 s), and its only report is a parse-time
ANTLR `memcpy` over-read in `Parser::consume` -> `vector<ParseTree*>` growth --
noise for this hunt, though possibly worth a look on its own.

## 6i. First root-coverage hole FIXED (actor dispatch); the browser path has another

`Thread::act` pops a `MethodCallInfo` off the queue and holds it in a C++
local until the call completes.  The queue entry is traced, and the callee was
rooted (`Thread::currentActorCall`, visited in `visitSingleThreadRoots`) --
but the **arguments and the return future had no root at all** for the
duration of the call.  Natively the conservative scan of parked C++ stacks
covers them, which is why this never bit; wasm locals are unscannable, so
nothing did.  Fixed by rooting all three together
(`currentActorArgs`/`currentActorFuture`, cleared via
`clearCurrentActorCall()` on every exit path) -- commit `a13346e3`.

Verified with the headless reproducer, which used to fault at round
~500-1000: **5500 rounds x2 clean on the SAFE_HEAP build AND x2 on the
release build**; 760/760 inline-GC, 872/872 dedicated.

**The browser reproducer still crashes at ~7 s** (`memory access out of
bounds`, `gcrev=2`, `fcflags=0xf`, no forensic violation).  That is expected
rather than contradictory: the fix closes ONE native-frame root hole, and the
IDE exercises paths the headless script does not -- `web.serve()`'s host-loop
pump, the store bridge and its call marshalling, the `Df` actor's document
trees, OPFS.  Any of those holding the only reference to a Value across a
safepoint has the same exposure.

**Next, and cheap now**: extend the headless driver to exercise the store /
`web.serve()` path (the browser's actual shape) so the next site can be named
by the same SAFE_HEAP-under-node trick in a minute rather than by browser
archaeology.  Then audit that path for Values held in C++ locals across a
safepoint.  The general remedy remains the 6g design decision; each site fixed
this way is a bounded step toward it.

## 6j. Second hole fixed (actor RESULT); browser still fails -- stale artifact ruled out

The same class appeared again a few lines further on in `Thread::act`: the
method's RESULT is a C++ local while `resolveFuture()`, `createFrozenSnapshot()`
and `clone()` run on it -- all of which allocate, so a collection can land
inside that window.  Rooted as `Thread::currentActorResult`, re-assigned after
each transform, cleared with the rest.  (Only reference types matter here; a
primitive result costs nothing to assign.)

**The browser reproducer still crashes at ~7 s**, and this verdict is now
protected against the failure mode that has burned this investigation
repeatedly: the soak asserts the page's reported build stamp
(`GC_SOAK_EXPECT_BUILD`, compared against the `__TIME__` literal read out of
the artifact with `strings`) and fails with STALE ARTIFACT otherwise.  The
run above reported `01:35:22`, the binary containing BOTH rooting fixes, and
`deploy-and-verify.sh` confirmed served bytes == built bytes.  So the
remaining fault is real and lives in a path the headless script does not
exercise.

**Where to look next** -- the paths that handle Values OUTSIDE both an
`execute()` frame and `callNativeFn`'s wasm cover, i.e. the same shape as the
two already fixed:

* `compiler/web/WebHostLoop.cpp` -- the host-loop pump and anything it holds
  across a pump turn;
* the store bridge in `compiler/web/ModuleWeb.cpp` / `JsBridge.cpp` --
  marshalling Values to and from JS;
* event delivery (`VM::processPendingEvents`) and the deferred native
  param-conversion resumption paths;
* `wasm/main.cpp`'s inbox (`serveInbox`) and `g_lastResult`.

The cheapest way to get there is to extend `wasm-gc-probes/run-wasm.mjs` to
drive a `web.serve()`-shaped workload with store calls, so the next site is
named by SAFE_HEAP-under-node in a minute rather than by browser archaeology.

**Attempted, and blocked on ONE harness gap** (`serve_repro.rox` +
`ROXAL_STORE=probe`): the script serves, `Module.roxalStore('probe')` exists
and accepts calls, but every call times out with *"was never serviced
(20000ms) -- is a script still running?"*.  The script IS running; what is
missing is the HOST-LOOP PUMP.  In the browser `web/src/roxal.js` drives it;
under node nothing does, so `web.serve()` parks and store ops are never
delivered.  The run therefore proves NOTHING about the store path -- it was a
clean 150 s with 0 store calls landed.  Finish this first: give the node
driver the same pump the browser host provides (or expose a
`roxal_pump_once()`-style export for headless hosts).  Until then the
browser remains the only way to exercise the store path, and this leg must
not be read as a negative.
The general rule now lives in implementation-notes.md, "Root coverage for
native frames (why wasm differs)".

## 6k. The fault is HEAP-ALLOCATOR METADATA CORRUPTION, not GC object lifetime

A run of eliminations, each with its own instrument.  Two of them retract
earlier entries in this document, so read this section before trusting 6f-6j.

**RETRACTED: the "PropertyMap double free".**  The detector fires 3/3 runs
WITHOUT container quarantine and 0/3 WITH it on the same workload -- so those
reports were the allocator handing a fresh map the address of a freed one.
The check is now gated on `ROXAL_FC_QUARANTINE`, where it is meaningful, and
the "same epoch => real double free" rubric in the old message was simply
wrong.

**RETRACTED: "the headless store leg is blocked on a missing host-loop pump".**
There is no pump gap.  The probe script was broken: `when X changes as ev`
binds the EVENT, and it assigned `ev` (not `ev.value`) to an `int` property,
which raised, unwound through the parked `web.serve()`, and ended the script --
after which nothing pumps.  With `ev.value` the store round trip completes in
6-10 ms under plain node.  A dead script disguised as a broken bridge.

**Excluded, each by quarantine (addresses never recycled, poison verified):**
GC object blocks; the `ObjectInstance` PropertyMap itself; and the map's
INTERNAL node/bucket allocations (via a forensic allocator on the map type).
The crash survives all three.

**Where it actually lands.**  On a SAFE_HEAP build with every fix and all
quarantines active, the abort is inside **`emscripten_builtin_malloc`**, called
from `ASTGenerator::visitStr` -> `visitPrimary` -> `visitCall` -- the PARSER.
Earlier it was inside `free()` under `ObjectInstance::dropReferences`.  Both
are the allocator tripping over its own metadata: **malloc and free are the
victims, not the culprit**.  That is the signature of an out-of-bounds write
(or a write through a stale pointer) somewhere else entirely, and it explains
why every GC-lifetime instrument came back clean.

**The IDE is required; the program is not.**  `web/public/bare.html` runs the
EXACT generated harness on the same build with no React, no editor and no IDE
stores: it survives 120 s+ **with collections actually occurring** (an early
version of this leg was invalid -- `roxal_diag` showed `collections=0`, so the
test now fails itself if no collection has happened).  The IDE crashes at ~7 s
on the same harness, and still crashes with the diagram sampling switched off.
So the writer is on a path the IDE exercises and a bare page does not.

**Instruments added** (all `ROXAL_GC_FORENSICS`, flag bits in Object.h):
`ROXAL_FC_QUARANTINE` (32) now covers object blocks, the property map, and map
nodes; the soak's stale-artifact guard hashes the wasm THE PAGE FETCHED
(`GC_SOAK_EXPECT_SHA`) because `__TIME__` comes from one translation unit and
does not move when other files change.

**In flight**: the IDE under `-sMALLOC=emmalloc-memvalidate`, which validates
the whole heap on every malloc/free and therefore aborts at the FIRST
operation that sees corruption rather than at the eventual victim.

## 6l. Elimination log for the browser fault (what it is NOT)

Every entry below is an experiment, not an opinion.  Read with 6k.

**Confirmed properties of the fault**
* It is heap-allocator METADATA corruption: SAFE_HEAP aborts inside
  `emscripten_builtin_malloc` (from the parser) and inside `free()`.  malloc
  and free are victims.
* It needs a COLLECTION.  Every "clean" leg that lacked one is invalid, and
  the harnesses now fail themselves if `roxal_diag` reports `collections=0`
  (two early legs were silently invalid this way).
* It needs the IDE.  `bare.html` runs the identical generated harness, on the
  same build, WITH collections, and survives 120 s+.
* It does not need Run: the IDE opens AND RUNS the last-used file at startup,
  and crashes there with `collections=1 drain=1` -- i.e. at the FIRST
  collection's reclaim drain.

**Excluded by experiment** (each re-run against a hash-verified artifact)
| hypothesis | how it was excluded |
|---|---|
| GC object lifetime | quarantine of object blocks, the PropertyMap, and the map's internal nodes -- addresses never recycled, poison verified -- crash unchanged |
| PropertyMap double free | fires 3/3 WITHOUT quarantine, 0/3 WITH: allocator address reuse |
| actor in-flight call / result unrooted | fixed (a13346e3, 8bd0967a); headless reproducer went from faulting at round ~500 to 5500 rounds clean, browser unchanged |
| untraced JS callback registry | fixed (2dbbdf5d); browser unchanged |
| `Tag::Method` receiver in a lambda capture | fixed (ddb5f4de); browser unchanged |
| `~ObjJsValue` deferring a bridge op from the collector thread | A/B bit `ROXAL_FC_NO_JS_RELEASE`: suppressing it changes nothing |
| conservative shadow scan | `?gcshadow=0`: crash unchanged |
| the diagram editor's live-value sampling | crashes with the source view too |
| `fileio` / the async I/O worker | transplanted into `bare.html` (with collections): survives. Its missing GC registration was a REAL bug and is fixed anyway |
| JS<->wasm marshalling (sizes, offsets, stale HEAP views) | audited: the shipped bundle rewrites every `HEAPU8` as `(growMemViews(), HEAPU8)`, so no cached view exists; all writes are exactly sized |

**Tools that turned out NOT to work here**
* wasm ASan: ~110 MB artifact, never instantiates in the browser (8 workers x
  large initial memory); headless it boots but is far too slow to reach the
  crash window.
* `-sMALLOC=emmalloc-memvalidate`: validates the whole heap per allocation, so
  the IDE never finishes booting (15 min, zero tabs).  Viable only for a much
  smaller workload.

**Open, and the next thing to try**: the remaining untested IDE-only
ingredient is JS-driven STORE TRAFFIC (calls and snapshot publishing) while a
collection runs.  `bare.html` cannot test it: its store calls are queued but
never serviced even though `pump` advances and the script is parked in
`web.serve()` -- an unexplained servicing gap that headless node does NOT have
(there, a store round trip completes in 6-10 ms).  Understanding that gap is
the gate on the last hypothesis.

## 6m. Drain histogram: what the crashing environment destroys that the surviving one never does

`ROXAL_FC_DRAINLOG` (bit 128) counts the ObjTypes destroyed by the reclaim
drain, readable from JS via `roxal_drain_histogram()`.  Running the SAME
generated harness in both environments, with collections in both:

    IDE  (crashes) : t1=860 t2=388 t3=41 t4=10 t5=2074 t6=3 t9=22 t13=1681
                     t15=1223 t16=8869 t17=1427 t19=1 t25=6 t28=2 t32=3
    bare (survives): t1=5 t3=1834 t4=6 t5=718 t9=356 t13=2354 t15=2066
                     t16=900 t17=450 t19=1 t22=4408 t25=356 t27=177

Types the crashing environment destroys and the surviving one NEVER does:
**t2 BoundNative (388)**, **t6 Actor**, **t28 Exception**, **t32
ChangeNotifier**.  (t33 JsValue is absent from BOTH, which independently
clears the DOM-handle path.)

`ChangeNotifier` is the store's property observer, so that was tested first --
`ROXAL_FC_NO_STORE_OBS` (bit 256) installs none of them.  **The crash is
unchanged**, so the observer teardown is not the writer either.

**Also excluded since 6l**: hardened libc++
(`_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE`, which makes every
vector/string index, front/back and iterator op a checked operation that traps
at the write) produces NO assertion before the crash.  So the out-of-bounds
write is NOT through a libc++ container operation -- it is raw pointer
arithmetic, a memcpy, or a write through a stale pointer.

**Tooling note for anyone continuing.**  Ranked by what they can actually see
here: ASan gives true per-allocation redzones but its artifact never
instantiates in the browser; `emmalloc-memvalidate` validates the whole heap
per allocation but is too slow for the IDE to boot; SAFE_HEAP checks alignment
and heap bounds but has no per-allocation boundaries (it is what we have);
hardened libc++ covers container operations only (now excluded); UBSan
`-fsanitize=bounds` covers fixed-size arrays the compiler can see.  A
Fil-C-style capability check -- every pointer carrying its bounds -- has no
wasm/emscripten implementation.  The gap in that list is exactly the class we
are left with: raw pointer/memcpy writes past a heap block.

## 6n. RETRACTION: the quarantine never covered swept objects (and what a second review found)

A second static review (Codex) caught a mistake that invalidates one of 6l's
entries, plus two real defects and one regression of mine.  Verified, not
assumed -- each was read in the source before acting.

**RETRACTED, then re-established.**  `VM::freeObjects` takes a batch WEAK HOLD
on every control block before dropping references, so `delObj`'s
"last weak ref" branch -- where the quarantine hook lived -- can never fire for
a swept object.  The batch release then called `delete[]` directly.  So the
"use-after-sweep of object blocks is excluded" result in 6l was measured with
an INERT treatment.  The hook now lives at the batch release, the sweep's real
free site, carrying the type and size captured before the destructors run.
Re-run with it live: **`quarantined=19311`** blocks retained and poison-checked,
crash unchanged, still no violation.  The exclusion is now real.  (Lesson,
again: verifying that the FLAG is live is not the same as verifying the CODE
PATH is reached.)

**FIXED -- drain/mark admission race.**  The collector tested
`collectionInProgress_` under `mutex_`, released it, and only published
`reclaimDraining_` later, inside `freeObjects`.  In that gap an inline
collector could see "no drain in progress", pass the barrier and start
marking, so `trace()` read the very COW members `dropReferences()` was
resetting -- exactly the shared_ptr control-block race the exclusion exists to
prevent.  The drain window is now published while the lock is still held, and
`reclaimDraining_` became a COUNTER so it nests with `freeObjects`' own
bracket.  Does not fix the browser crash.

**REVERTED -- my own regression.**  Making `blockEnter()` refuse while a
no-park section is held (6l) deadlocks: every wasm native call holds a no-park
cover, so `sys.join()`'s safe-block became a no-op, the joiner stayed Running,
the barrier waited for the joiner, and the joiner waited for a thread parked
for that very collection.  The underlying hazard is real but needs a
rooting-aware transition, not blanket precedence; the code now says so.

**NOT a defect -- covering a direct actor-native call.**  The review suggested
the two direct-native paths in `Thread::act` should cover the CALL, not just
the post-call region.  The existing comment explains why they must not: a
long-running native (the dataflow engine's run loop) polls safepoints
internally, and a cover turns those polls into no-ops -- the thread never
parks and the barrier waits forever.  Attempting it produced exactly that
design conflict; reverted, and the tradeoff stands as documented.  A proper
fix needs the cooperative-native classification the review proposes.

**Also fixed**: `AsyncIOManager`'s participant is now scoped to the window in
which it actually executes ops.  Holding it for the thread's lifetime left a
Running context at shutdown, where `collectNowForShutdown()` requires none --
13 fileio tests caught it.

## 6o. The fault, localised: the property map's LIVE memory is corrupt

Making assertions survive on wasm was the unlock.  `assert_msg_impl` wrote to
`std::cerr`, which on a pthread is proxied to the main thread -- an `abort()`
immediately after loses the text, so every failure presented as an anonymous
"native code called abort()" and read like memory corruption.  It now uses
`fprintf`+`fflush`.  Mirroring VM stdout/stderr into the devtools console
(`web/src/roxal.js`) made the rest visible.

With that, the browser fault has a precise stack:

    abort  <- the allocator's own corruption check
    emscripten_builtin_free
    __hash_table<..., ForensicNodeAlloc<...>>::__deallocate_node(...)
    __shared_ptr_default_delete<unordered_map<int, MonitoredValue, ...>>::__on_zero_shared()
    roxal::ObjectInstance::dropReferences()
    roxal::VM::freeObjects()        [collector thread]

So: destroying an `ObjectInstance`'s **PropertyMap** walks its internal
node/bucket links and hands `free()` a pointer that is not a valid block.  The
other presentation, `operation does not support unaligned accesses`, is the
same class seen through a different lens: wasm TRAPS on a misaligned ATOMIC,
i.e. an atomic on an `ObjControl*` whose bits are not really a pointer.

**What that rules out, given the quarantines already in place.**  The map
object, its nodes/buckets (via a forensic allocator) and the MVCC version
nodes are all retained -- their addresses are never recycled -- and a periodic
poison sweep of every retained block reports nothing.  So this is NOT a stale
write into freed memory and NOT address reuse: **the map's LIVE memory is
being corrupted while it is still in use.**

Also excluded this round: `"df sample: unwind"` is a SYMPTOM, not a cause --
timestamps put the crash at 2.6s and the first unwind at 2.7s, with the rest
arriving every ~400ms as the sampling poll hits a dead worker; MVCC snapshot
counts are identical in both environments (`snap=5`); and a misaligned-control
check at `incRef`/`decRef` never fires before the crash.

**Hardening kept, necessity UNPROVEN**: `createFrozenSnapshot` now takes the
object's `cowLock_` across the ROOT `shallowClone()`, which `resolveConstChild`
has always done for child clones.  It closes the window described in 6b, but
the crash is unchanged, so it is not the cause.

**Where to look next**: something writes into a live PropertyMap. The
remaining candidates are a concurrent mutation of the same instance from two
threads, or an out-of-bounds write from an adjacent allocation. The
`in=<drained>/<queued>` counter added to `roxal_diag` shows the JS->VM
direction is healthy (74/74), so the store INBOUND path is not stalled -- what
never arrives is the response.

## 6p. Concurrent mutation EXCLUDED (with the treatment proven live)

If the map's live memory is corrupt (6o), the obvious suspect is two threads
mutating one instance.  `ForensicWriterGuard` (under `ROXAL_FC_VERIFY`) claims
`ObjControl::mutatingThread` on entry to every `ObjectInstance` mutator --
`propertySlot`, `assignProperty`, `emplaceProperty`, `setProperty` -- AND on
`dropReferences`, so a writer overlapping the destructor counts too.  An
overlap reports both thread ids and the site.

**Result: `wguards=14052`, zero reports.**  The counter matters as much as the
silence: this instrument is proven to have executed 14k times, unlike the
quarantine leg in 6l that was inert and produced a false exclusion.  So no two
threads mutate one instance, and nothing mutates one while it is being
destroyed.

That leaves, for a live map whose internal links go bad: an out-of-bounds
write from an ADJACENT allocation (nothing in the map's own lifecycle is
touching it), or a write through a pointer derived from something other than
the map -- with the caveat that this build's node/bucket allocations come from
`ForensicNodeAlloc`, so their addresses are never recycled while quarantine is
on.

Instrument inventory now (all `ROXAL_GC_FORENSICS`, bits in Object.h):
1 checks, 2 poison, 4 provenance, 8 verify (mark verification + writer guard +
clone-race detector), 16 leak, 32 quarantine (object blocks at the sweep's real
free site, PropertyMap, map nodes, MVCC version nodes; periodic poison sweep),
64 no-JS-release A/B, 128 drain histogram, 256 no-store-observers A/B.
Liveness counters -- `quarantined=`, `wguards=`, `in=drained/queued` -- exist
so no future result rests on a treatment that never ran.

## 6q. IT IS A USE-AFTER-FREE OF A SWEPT OBJECT (and why 6l/6n missed it)

Redzoning the property map's own allocations and making the first violation
fatal produced the chain:

    GC BAD CONTROL POINTER at decRef: 0xdddddddd ... DROP-PARENT obj=... type=5
    roxal::roxalForensicBadPointer
    roxal::Obj::decRef()
    roxal::Value::decRefObj()
    roxal::Value::~Value()          <- destroying a Value whose BYTES are poison
    std::__shared_...               <- inside the map's shared_ptr teardown
    roxal::ObjectInstance::dropReferences()

`0xdd` is the quarantine's own poison for DEAD OBJECT BLOCKS.  So a Value is
being destroyed out of memory that the collector had already swept: something
still holds a pointer into a dead object.

**A/B that proves it is not the instrument talking.**  With quarantine ON the
detector fires at ~2.9s with `0xdddddddd`.  With quarantine OFF (same build,
`fcflags=527`) there is NO bad-pointer report at all -- just the original
`free()` abort at ~2.7s.  That is exactly what a use-after-free looks like from
both sides: retained-and-poisoned, the stale reader sees `0xdd` and is caught;
recycled, the same memory becomes something else (a property-map node) and the
stale access corrupts it until the allocator trips over its own metadata.

**Why the earlier exclusions were wrong.**  6l/6n concluded "not a
use-after-sweep" from a poison sweep that only asks whether a dead block was
WRITTEN.  It cannot see a dead block being READ -- which is what happens here:
the stale Values are read (and their control pointers decRef'd) out of the
corpse.  Both entries are hereby superseded.  (The 6n retraction stands on its
own: the hook really was in the wrong place.)

**Where this leaves the hunt.**  The parent whose teardown runs when the stale
Value dies is an `ObjectInstance` (`DROP-PARENT type=5`).  The open question is
no longer WHAT class of bug -- it is WHO retains a pointer into a swept object.
The drain histogram (6m) says the crashing environment destroys `BoundNative`
(388), `Actor`, `Exception` and `ChangeNotifier` where the surviving one
destroys none, which is the natural place to look next.

**Instrument correctness note.**  The first redzone report was a FALSE
POSITIVE of my own making: the live sweep verified a snapshot after releasing
its lock, so a concurrent `deallocate` poisoned a block mid-check and the
sweep read the poison as damage.  The byte dump (solid `0xcd`, the node-poison
pattern) exposed it immediately; the sweep now verifies while holding the lock.
Dumping the offending bytes is what made both of these legible -- a detector
that reports "smashed" without showing WHAT was written cannot be audited.

## 6r. SOLVED: a split reclaim batch (thanks to a second review's theory)

The theory came from a static review and it fits every observation:

1. One sweep finds child `C` and parent `P` (an `ObjectInstance` holding
   `Value(C)`) unreachable.  Both are legitimately garbage -- no missing root.
2. The inline collector published the unreachable set **one object at a time**
   (`retireObject` per object).  The first push takes the retire queue from
   empty to non-empty and WAKES the reclaimer thread.
3. The reclaimer drains what is there -- just `C` -- destroys it, and (in a
   forensic build) poisons its block.
4. The collector then publishes `P`.
5. Reclaiming `P` destroys its property map, whose `Value(C)` destructor calls
   `C->decRef()`.  `C->control` is already dead: `0xdddddddd`.

That is precisely the stack in 6q, and it explains what nothing else did:

* **No mark miss** -- both objects were garbage, so mark verification is
  correctly silent (it deliberately ignores edges from UNMARKED parents).
* **No concurrent mutation** -- `wguards=14052` with zero reports.
* **Native stays clean** -- the dedicated collector publishes and then drains on
  ONE thread, so the set is never split.  Only wasm's inline mode has a
  separate publisher and reclaimer.
* **`freeObjects`' batch discipline** ("drop every object's references before
  any destructor runs") is only meaningful if the batch is INDIVISIBLE.  It
  wasn't.

**The fix**: `SimpleMarkSweepGC::retireObjects()` links the whole set into a
chain locally and attaches it with a SINGLE compare-exchange, so a concurrent
drain sees either none of the set or all of it.  All three publish sites (the
inline elector, the dedicated collector, shutdown) now use it.

**Verification**: the counter4 IDE reproducer -- which crashed at ~7s in every
run for this entire investigation -- now soaks **4 minutes clean**, and **3
minutes at a 128KB threshold with quarantine and mark verification on**.
Browser suites (app, workspace, liveness, dfdoc) pass.  Native: 872/872
dedicated, 760/760 inline-GC.

**A liveness-proxy correction worth keeping.**  The first post-fix run reported
"frozen": the soak used OUTPUT TEXT as its liveness signal, and a diagram file
renders values on the canvas rather than printing.  `roxal_diag` showed the VM
perfectly healthy (pump 426->12594, eventTurns 26->278, engineTicks 92->2088,
collections 1->2, store calls 273/273).  The soak now keys liveness on the VM's
own counters.  A test that can report a healthy VM as frozen is as dangerous as
one that reports a corrupt heap as clean.

## 7. Deferred by decision: reclamation inside the stop-the-world window

Proposed structure (David): stop the world -> mark & sweep -> reclaim ->
start the world, in BOTH modes; the dedicated thread (when compiled in)
executes the whole sequence, the self-elected mutator does the same
otherwise, and `runFor()` on RT simply yields while the world is stopped.

Assessment: this removes, by construction, the entire coordination-hazard
class that plagued this investigation -- drain vs resumed mutators, the
inline-drop overlap, the mark/reclaim handshake and its check-then-publish
window, and the exclusion machinery (including the bug it introduced).  It
does NOT fix the crash's root cause: the mark miss decides WHAT gets swept,
and the dangling Value corrupts whenever it is next touched, stopped world
or not (though it would make the fatal touch single-threaded, deterministic,
and much easier to catch).  Costs to weigh: the pause grows by batch
destructor time (bound it with an incremental per-window cap for RT), and
"destructors may allocate during the stopped window" becomes a stated
invariant rather than an accident.  DEFERRED so the hunt keeps a stable
substrate: find the mark miss first, then simplify from understanding.

## 8. Uncommitted state at session end (per the no-speculative-commits rule)

In the working tree, awaiting review, in dependency order:

1. The freeze fix: deferral-aware collector wake predicate
   (`SimpleMarkSweepGC.cpp`).  Cause proven by stacks + intervention.
2. `roxal_config` export repair (`EMSCRIPTEN_KEEPALIVE` was severed by an
   insertion between attribute and function -- see section 3) and the
   unsilenced error path in `web/src/roxal.js`.
3. Diagnostics: GC-state readback + mutator-context snapshot + engine
   lock probes in `roxal_diag()`; forensic tripwires with both drop sites
   counted and provenance recorded BEFORE the inline drop; `DROP-PARENT`
   tagging (all `#ifdef ROXAL_GC_FORENSICS` except the diag readback).
4. Committed earlier but flagged for review: `8ce4283f` (mark/reclaim
   exclusion) -- did not prevent the crash; superseded in intent by the
   deferred restructure in section 7.

The GC coordination FIXES (section 6f, items 1-6) are uncommitted and are NOT
forensics-gated -- they are the load-bearing changes:
`SimpleMarkSweepGC.{h,cpp}` (deferral, `worldStopped_`, collector role),
`core/atomic.h` (forEach), `compiler/Object.{h,cpp}` (queueCall lock scope,
`dropReferencesOnce`), `compiler/ObjControl.h` (`dropClaimed`),
`compiler/VM.cpp` (reclaimer uses the one-shot drop).
New test surface: `scripts/test-inline-gc.sh`, `runtests.py` ROXAL_BUILD_DIR,
`web/tests/gc-soak.spec.js` (currently FAILS -- see 6f), and the GC
coordination invariants section in `implementation-notes.md`.

The instrumentation is also uncommitted, all
`#ifdef ROXAL_GC_FORENSICS` except where noted:
- the corrected drain deferral (`collectionInProgress_` only, both sides) --
  NOT forensics-gated; this is the freeze fix, now with `gc_liveness` as its
  native regression test in an inline-GC build;
- `ROXAL_FC_VERIFY` mark verification + `ObjControl::tracedEpoch` + the
  `roxalForensicMarkMiss` reporter;
- the ENQUEUE-DURING-COLLECTION and MUTATION-DURING-COLLECTION tripwires;
- native stderr echo of forensic reports (wasm still buffer-only);
- `ROXAL_FC_FLAGS` env override of the forensic switches;
- `runtests.py`: `ROXAL_BUILD_DIR` env override (2-line, not forensics-gated).
Reproducer: `wasm-gc-probes/wasmlike_repro.rox` (git-excluded).

The earlier TSan session added NOTHING to the working tree: its
race-amplification probe in `ObjectInstance::ensureUnique()` was reverted
once it had served its purpose, and `build-tsan/` is a build directory, not
tracked content.  The harness scripts live in the session scratchpad
(`tsan_snapshot_churn.rox`, `overlap_churn.rox`, `identity_probe.rox`,
`concurrency_probe.rox`, `df_snapshot_churn.rox`); `identity_probe.rox` and
`concurrency_probe.rox` are the two worth keeping -- they are the evidence
behind the closed channels in section 6b.


## 9. The value stall: NOT a GC bug -- a value-stack leak in `invokeClosure`

David reported that the demo "eventually stops outputting the value= and also
updating the diagram labels", stopping at `value = 11` in two Firefox runs, in
Chrome, in `bare.html`, and with the 4MB gc argument.  Identical stopping value
across browsers and pages is not what a race looks like, and console/tab
throttling was never a candidate for the same reason.

**What the counters said.**  A 10-minute IDE soak reproduced it exactly:
stalled at t=270s, `lastValue="value = 11"`, 532 updates.  At the stall:

    t=267s n=532 tick=5325/-1990ms hd=0 in=695/695 pump=33543 eventTurns=695
           engineTicks=5325 locksHeld=0 collections=0 ctx[I/R/P/B]=7/5/0/0
    t=270s n=532 tick=5325/-5015ms ...  (delta recedes at exactly wall-clock rate)

`collections=0` -- no collection had run in the entire session, so the GC was
never involved.  `engineTicks` frozen at 5325 while `pump`/`eventTurns` kept
climbing: the VM and host loop were alive and only the dataflow engine had
stopped.  The added `tick=<n>/<ms to next tick>` field made it decisive: a live
run loop either ticks (incrementing) or hits its resync branch (which zeroes
the tick number and makes the delta positive).  A frozen tick number whose
scheduled time recedes at 1s/s means nobody is executing the loop at all.

**Native reproduction killed every browser-shaped theory.**  The same diagram
run under `build/roxal` stopped after exactly 532 updates at `value = 11` --
same count, same value, no browser, no worker threads, no collector.  Raising
the clock 10x stalled at the same 532 updates ten times sooner, so the trigger
is a COUNT of evaluations, not elapsed time.  gdb (launched under the debugger;
`ptrace_scope` blocks attaching) showed three threads at the stall -- main
parked in `wait`, the GC collector idle, and the engine's actor thread back in
`Thread::act` waiting for its next message.  `run()` had RETURNED.

**The exception nobody saw.**  Wrapping `dataflow_run_native` printed it:

    dataflow engine stopped: Value stack overflow (limit 16384; deep handler/event recursion?)

Thrown on the engine thread, swallowed at the actor boundary, never surfaced.
`sys._stackdepth()` called from inside a node function measured the leak
directly -- depth 9 at the first node call, 1193 at call 100, 5993 at call 500,
9593 at call 800: dead linear, ~12 slots per evaluation round for this network.

**Root cause** -- `VM::opReturn()`:

    thread->popFrame();
    if (!thread->frames.empty()) {          // only when a caller frame remains
        auto popCount = &(*thread->stackTop) - returningFrame.slots;
        for (auto i=0; i<popCount; i++) pop();
    }

A returning frame's slots (callee slot 0, arguments, locals) are reclaimed only
if another frame is still beneath it.  For a top-level script that is harmless
-- the program is ending.  But `invokeClosure()` runs a closure AS the outermost
frame, and that is how the engine evaluates every script node: each evaluation
abandoned its whole frame.  ~1400 evaluations later the 16384-slot stack was
full, the engine thread threw, and the network stopped for good while the VM
carried on -- which is exactly what "the values just stop" looks like.

**Fix.**  Dropping the `frames.empty()` guard fixes the leak but breaks 42
actor tests -- actor handlers also return as outermost frames and depend on
that behaviour.  The fix is scoped to the re-entrancy boundary that owns the
pushes instead: `invokeClosure` records the stack depth before pushing the
closure and restores it once the call completes (a Yielded call is still live
and is left alone).  This matches what the native-closure branch of the same
function already did explicitly.

**Verification.**  Node-call stack depth flat at 7 across 800 calls (was 9593);
the counter harness ran 700 updates over the full 350s at a sustained 2/s (was
532 then dead); 872/872 native tests; browser soak clean past the old stall
point.  Regression test `invokeClosure_stack_balance` in the `rt_execution`
suite asserts the stack delta over 8 calls is 0 -- it reports `stack delta 24`
(3 slots per call: closure + argument + local) with the fix disabled.

**Kept as diagnostics**: `dataflow_run_native` now reports an engine-stopping
exception to stderr, and `roxal_diag` carries `tick=<n>/<ms>` and `hd=`.  A
silently-dying dataflow engine is exactly the failure that took longest to see;
the actor boundary swallowing that exception is a separate issue worth its own
look.
