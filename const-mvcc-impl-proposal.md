# Const Implementation: MVCC with Lazy-Materialized Frozen Clones

## Motivation

The const spec (`const-spec.md`) requires **snapshot isolation**: `const c = o` must freeze the entire reachable object graph such that mutations through mutable aliases are invisible through `c`. An eager approach (e.g., walking the reachable graph to mark or copy objects) incurs O(n) cost on every `T → const T` conversion. MVCC can defer that cost, paying incrementally only for objects actually mutated and accessed.

## Why Lazy Propagation Alone Fails

One might try to avoid both the eager walk and versioning by lazily incrementing a `constRefCount` on child objects as they are accessed through const refs. The problem: if a mutation happens through a mutable alias *before* anyone reads through the const ref, the child's `constRefCount` is still 0 — no CoW triggers, and snapshot isolation is broken.

Example (`const-interior-mutation.rox`):
```roxal
var o = Outer(Mid(Leaf(1)))
var m = o.m                  // mutable alias to interior Mid
const c :Outer = o           // snapshot

m.l.i = 2                   // mutate Leaf via mutable alias
print(c.m.l.i)              // must print 1 (snapshot isolation)
```

Here `m.l.i = 2` happens before `c.m.l.i` is ever read. With lazy propagation, `Leaf` has no const-ref count and no CoW occurs — the mutation leaks through.

We fundamentally need **either** eager marking of the reachable graph **or** always-versioning mutations. This proposal takes the latter path.

---

## Design Overview

### 1. Global Write Epoch

A global `atomic<uint64_t> writeEpoch` is bumped on each mutation to any ref-type object *while snapshots are active*. A global `activeSnapshotCount` allows bypassing the version-save overhead entirely when no const snapshots exist (cost: one branch per mutation, well-predicted).

```cpp
// global
std::atomic<uint64_t> globalWriteEpoch{1};
std::atomic<uint64_t> activeSnapshotCount{0};
std::atomic<uint64_t> minActiveSnapshotEpoch{UINT64_MAX}; // for cleanup
```

### 2. Version Chains on ObjControl

Add to `ObjControl`:

```cpp
struct ObjVersion {
    uint64_t epoch;         // the object's writeEpoch at time of save (i.e., the epoch at which the
                            // object entered this state — its "birth epoch", NOT globalWriteEpoch)
    Value snapshot;          // shallow clone capturing state at this epoch (Value for RAII ref counting)
    ObjVersion* prev;       // older version (linked list)
};

// Ref-counted token shared by all frozen clones of a single snapshot.
// Lifetime = union of all frozen clones (root + lazily materialized children).
struct SnapshotToken {
    std::atomic<int32_t> refcount{1};
    uint64_t epoch;
    CloneContext cloneCtx;   // per-snapshot map for alias/cycle preservation
};

struct ObjControl {
    // ... existing fields (strong, weak, obj, allocationSize, collecting, markEpoch) ...
    std::atomic<uint64_t> writeEpoch{0};            // epoch of last mutation
    SnapshotToken* snapshotToken{nullptr};           // for frozen clones: the snapshot this clone belongs to (nullptr for live objects)
    std::atomic<ObjVersion*> versionChain{nullptr};  // linked list of older versions (atomic for lock-free CAS prepend/read)
};
```

On mutation of any object (SetProp, list append/set, dict store, etc.) while `activeSnapshotCount > 0`:

1. `saveVersion()`: shallow-clone the object (copy its property map / elements vector — NOT recursive into children), link into `versionChain` tagged with the **object's current `control->writeEpoch`** (its "birth epoch" — the epoch at which the object entered this state, NOT `globalWriteEpoch`). For a never-mutated object this is 0, which is ≤ any valid snapshot epoch.
2. Mutate in place.
3. Bump the object's `writeEpoch`: `control->writeEpoch = globalWriteEpoch.fetch_add(1)`. This is done **after** the mutation so that concurrent readers see the new epoch only after the new state is fully written.

Cost: O(#direct-properties) per mutation, only when snapshots are active.

### 3. `T → const T`: Shallow Clone + Epoch Stamp

On `const c = o`:

1. Allocate a `SnapshotToken` with `epoch = globalWriteEpoch.load()` and `refcount = 1`.
2. Shallow-clone the root object → `frozenRoot` (copies property slots; children remain shared refs to live objects).
3. Set `frozenRoot->control.snapshotToken = token` (the frozen clone holds a ref to the token).
4. Set **bit 48 (ConstMask)** on the Value.
5. Increment `activeSnapshotCount`, register epoch in the active snapshot set.

**Cost: O(#direct-properties-of-root)** — NOT O(reachable-graph).

### 4. Const Reads: Version Resolution + Caching

When `GetProp` reads through a const Value (bit 48 set):

1. The object pointed to is a **frozen clone** with `snapshotEpoch = E`.
2. Read the child property from the frozen clone's property slots.
3. If the child is a primitive (int, real, bool, etc.): return directly — no clone needed.
4. If the child is a reference type: **always create a frozen clone** of the child with `snapshotEpoch = E`, and **cache it back into the frozen parent's property slot**. This ensures every object in the const read chain carries the `snapshotEpoch` for resolving its own children (epoch propagation). To create the frozen clone:
   - If the child has not been mutated since the snapshot (`child->control.writeEpoch < E`): shallow-clone the child's current state. The strict `<` is important: `writeEpoch == E` means a mutation consumed the same global epoch value as the snapshot (via `fetch_add`), so it may have occurred after the snapshot and must be resolved via the version chain.
   - If the child has been mutated since the snapshot (`child->control.writeEpoch >= E`): walk `versionChain` on the child to find the newest version with epoch `< E`, then shallow-clone that version. **Invariant:** such a version must always exist — the oldest version's epoch is the object's writeEpoch at its first-ever mutation (0 for newly created objects), which is ≤ any valid snapshot epoch ≥ 1. Assert this in debug builds; a miss indicates a bug in version chain trimming.
5. The returned Value gets bit 48 set (transitive constness).

**Invariant:** every reference-type child accessed through a const ref is materialized as a frozen clone with the same `snapshotEpoch`. This is the single rule — there is no "return directly without cloning" path for reference types.

**Caching** is key: once a child is materialized, subsequent reads of the same property return the cached frozen clone in O(1). Total materialization cost across all reads through a const ref is proportional to **#objects actually accessed**, not the total reachable graph size.

When multiple properties of the same frozen parent refer to the same underlying object (aliases), the materialization must use a per-snapshot `CloneContext` map to ensure both properties resolve to the **same** frozen clone, preserving `is`-identity within the snapshot (i.e., if `o.a is o.b` then `c.a is c.b`).

### 5. Epoch Propagation via the Objects Themselves

The epoch lives in each frozen clone's `SnapshotToken` (accessed via `ObjControl.snapshotToken`). When `GetProp` resolves a child and creates a cached frozen clone, that clone holds a ref to the same `SnapshotToken` (incrementing its refcount). So for a chain `c.m.l.i`:

- `c` → frozen clone of `Outer` (snapshotEpoch = E)
- `c.m` → resolve `Mid` at E → cache frozen clone of `Mid` (snapshotEpoch = E)
- `c.m.l` → resolve `Leaf` at E → cache frozen clone of `Leaf` (snapshotEpoch = E)
- `c.m.l.i` → read `i` from frozen `Leaf` → primitive int value, returned directly

No wrapper/proxy objects, no side tables, no widened stack slots. The epoch propagates naturally through the frozen clone chain.

### 6. Snapshot Lifetime and Cleanup

**Snapshot ownership via shared token:** Child frozen clones can escape their parent and outlive the root (e.g., `var x = c.m` captures a child frozen clone, then `c` goes out of scope). Therefore, snapshot lifetime cannot be tied to the root clone's destruction.

Instead, each snapshot is represented by a ref-counted **`SnapshotToken`** (a small object holding the snapshot epoch and a pointer to the per-snapshot `CloneContext`). The root frozen clone creates the token at `T → const T` time and holds a strong ref to it. Every child frozen clone materialized within the same snapshot also holds a strong ref to the same token. The token's refcount thus tracks the total number of live frozen clones (root + children) for that snapshot.

When the token's refcount drops to zero (the last frozen clone for that snapshot is destroyed):
- `activeSnapshotCount` is decremented.
- The snapshot's epoch is removed from the active set.
- `minActiveSnapshotEpoch` is updated to the oldest surviving snapshot's epoch (maintained via a sorted set or min-heap of active snapshot epochs, or recomputed on decrement).

**Version chain trimming:**
- Version chain entries older than `minActiveSnapshotEpoch` can be trimmed — **except** the newest version with epoch ≤ `minActiveSnapshotEpoch` must be kept (it serves as the floor version for the oldest active snapshot's resolution lookups).
- **Invariant:** after trimming, the oldest remaining version must have `epoch ≤ minActiveSnapshotEpoch`. Assert this in debug builds — a violation means a snapshot could fail to resolve a version, which is a correctness bug.
- When `activeSnapshotCount` reaches 0, all version chains can be discarded entirely and `minActiveSnapshotEpoch` reset to `UINT64_MAX`.
- Trimming must not free version chain nodes while concurrent readers may be walking the chain. Safe reclamation options: defer trimming to GC sweep (when no readers are active), or use epoch-based reclamation (RCU-style) for the version chain nodes themselves.

---

## Costs Comparison

| Operation | Eager Freeze + CoW | MVCC (this proposal) |
|-----------|----------------------|----------------------|
| `T → const T` | **O(reachable graph)** | **O(#root properties)** |
| Mutation (no snapshots) | O(1) | O(1) (one branch) |
| Mutation (with snapshots) | O(1) CoW | O(#properties) version save |
| Const read (first access) | O(1) | O(version chain len + #properties) |
| Const read (cached) | O(1) | O(1) |
| Mutable read | O(1) | O(1) |

The total work is redistributed: instead of paying upfront on `T → const T`, we pay incrementally on mutations and first-time const reads, and **only for objects actually mutated or accessed**. The mutable read path has **zero overhead**, which is important since mutable reads are far more common than const reads.

---

## Walkthrough: `const-interior-mutation.rox`

```roxal
var o = Outer(Mid(Leaf(1)))    // Outer→Mid→Leaf(i=1)
var m = o.m                     // mutable alias to Mid

const c :Outer = o              // (A) shallow clone Outer, snapshotEpoch=E

m.l.i = 2                      // (B) mutate Leaf.i

print(c.m.l.i)                 // (C) should print 1
print(o.m.l.i)                 // (D) should print 2
```

**Step A** — `const c = o`:
- Shallow-clone Outer → `Outer'` with `snapshotEpoch = E` (say E=5).
- `Outer'.m` property points to the **same** `Mid` object as `o.m`.
- `activeSnapshotCount` = 1.

**Step B** — `m.l.i = 2`:
- Access `m.l` → returns mutable ref to `Leaf` (no const bit). Ordinary GetProp — O(1).
- SetProp on `Leaf` for property `i`:
  - `activeSnapshotCount > 0`, so `saveVersion()`: shallow-clone `Leaf` → version node `{epoch=0, snapshot=Leaf(i=1)}`, linked into `Leaf->control.versionChain`. The epoch is `Leaf->control.writeEpoch` (0 — never previously mutated), representing the "birth epoch" of this state.
  - Set `Leaf.i = 2` in place.
  - Bump `Leaf->control.writeEpoch` to 5 (via `globalWriteEpoch.fetch_add(1)`, which returns 5 and advances global to 6).

Note: `Mid` is not mutated, so no version is saved for it.

**Step C** — `c.m.l.i`:
- `c` is const (bit 48 set) → `Outer'` (frozen clone, snapshotEpoch=5).
- GetProp `m` on `Outer'`:
  - Child = `Mid` (reference type). Per the invariant (§4), always materialize a frozen clone.
  - `Mid->control.writeEpoch` (=0) < 5 (Mid was never mutated), so shallow-clone `Mid`'s current state.
  - Create frozen clone `Mid'` with `snapshotEpoch=5`, cache in `Outer'.m`.
  - Return const ref to `Mid'` (bit 48 set).
- GetProp `l` on `Mid'` (frozen clone, snapshotEpoch=5):
  - Child = `Leaf` (reference type). Always materialize a frozen clone.
  - `Leaf->control.writeEpoch` (=5) ≥ 5 — Leaf was mutated at or after the snapshot epoch.
  - Walk `Leaf->control.versionChain` → find `{epoch=0, snapshot=Leaf(i=1)}`. Epoch 0 < 5 — match.
  - Shallow-clone `Leaf(i=1)` → frozen clone `Leaf'` with `snapshotEpoch=5`, cache in `Mid'.l`.
  - Return const ref to `Leaf'` (bit 48 set).
- GetProp `i` on `Leaf'` (frozen clone): `i` is a primitive (int) → returns `1` directly. Correct.

**Step D** — `o.m.l.i`:
- `o` is mutable. GetProp `m` → `Mid` (mutable, no bit 48). GetProp `l` → `Leaf` (mutable, writeEpoch=5). GetProp `i` → `2`. Correct.
- Zero overhead on the mutable path.

---

## Implementation Layers

### Layer 0: Encapsulate Mutation in Obj Subtypes (Prerequisite)

Currently, the mutable payload of classes like `ObjList`, `ObjDict`, `ObjectInstance`, `ActorInstance`, `ObjVector`, `ObjMatrix`, `ObjTensor`, etc. is exposed directly — e.g., `ObjList::elements` is a public `std::vector<Value>` that VM opcodes and builtin functions mutate in-place. This is fine for purely internal code, but for MVCC we need to reliably intercept **every** mutation to insert `saveVersion()` guards. Missing even one mutation site silently breaks snapshot isolation.

**Before any MVCC work begins**, refactor each mutable Obj subtype so that:

1. **Payload members become private** (or protected). For example, `ObjList::elements`, `ObjDict::entries`, `ObjectInstance::properties`, etc.
2. **All mutations go through methods** — e.g., `ObjList::setElement(idx, val)`, `ObjList::append(val)`, `ObjDict::store(key, val)`, `ObjectInstance::setProperty(hash, val)`, etc. These can be `inline` where performance matters (the hot path in opcode dispatch).
3. **Read access also goes through methods** (or const accessors) — e.g., `ObjList::getElement(idx) const`, `ObjList::size() const`. This isn't strictly required for MVCC, but makes the API consistent and prevents accidental mutation through a non-const reference.

This is a mechanical but important refactor:
- The C++ compiler will flag every direct access to the now-private members, making it straightforward to find and update all mutation sites in the VM opcode handlers, builtin function implementations, and anywhere else in the codebase (e.g., the `<-` copy-into operator, serialization, GC traversal).
- Once complete, each mutation method becomes the natural place to insert the `saveVersion()` guard (Layer 4), with no risk of missing a mutation path.
- As a side benefit, it also makes it easier to add future invariant checks, instrumentation, or alternative storage strategies without touching call sites.

This layer has **no functional change** — it is a pure encapsulation refactor that can be done and tested incrementally (one Obj subtype at a time) before any const/MVCC logic is added.

### Layer 1: Value — ConstMask (bit 48)

In `Value.h`:
- Add `const uint64_t ConstMask = uint64_t(1) << 48;`
- Update `asObj()` and `asControl()` extraction masks to include ConstMask:
  `val & ~(SignBit | QNAN | WeakMask | ConstMask)`
- Add helpers: `isConst()`, `constRef()`, `mutableRef()` (analogous to `isWeak()`/`weakRef()`/`strongRef()`)
- In copy constructor / assignment / destructor: const Values participate in normal strong ref counting (unlike weak refs, const refs keep the object alive).

### Layer 2: ObjControl — Version Infrastructure

In `ObjControl.h`:
- Add `writeEpoch`, `snapshotEpoch`, `versionChain` fields (as above).
- Add `ObjVersion` struct.

### Layer 3: Obj Subtypes — Shallow Clone + Version Save

Add to each mutable Obj subtype (`ObjectInstance`, `ObjList`, `ObjDict`, `ObjVector`, `ObjMatrix`, `ObjTensor`, `ActorInstance`):
- `shallowClone()` method: copies the object's own state (property map, elements vector, etc.) without recursing into child Values. Child Values are copied by value (bumps refcounts), not deep-cloned.
- `saveVersion()` method: calls `shallowClone()`, wraps result in an `ObjVersion` node, links into `versionChain`.

The existing `clone()` (deep clone) is separate and unchanged.

### Layer 4: VM Mutation Sites

With Layer 0's encapsulation in place, each mutation method on the Obj subtypes is the natural insertion point. The sequence within each mutation method is:

1. `if (activeSnapshotCount.load(relaxed) > 0) saveVersion();` — capture pre-mutation state, tagged with the object's current `writeEpoch`.
2. Apply the mutation in place.
3. `control->writeEpoch = globalWriteEpoch.fetch_add(1);` — bump epoch **after** mutation, so concurrent readers see the new epoch only after the new state is fully written.

This also covers indirect mutation paths such as the `<-` copy-into operator, since they go through the same methods.

### Layer 5: VM GetProp — Const-Aware Resolution

In `GetProp` / `GetPropCheck` (and index-based access for lists, dicts, etc.):
- If the receiver has bit 48 (const) set:
  - Block mutations: any SetProp on a const Value is a runtime error ("Cannot mutate const").
  - For primitive-valued properties (int, real, bool, etc.): return directly (no clone needed).
  - For reference-type children: **always** materialize a frozen clone with the same `snapshotEpoch` (or return cached clone from the frozen parent's property slot). Use the snapshot root's `CloneContext` to preserve alias identity across properties (see §4). Set bit 48 on returned Value.

### Layer 6: Compiler — Grammar + AST + Bytecode

- Remove the current restriction that `const` only applies to primitive types.
- Parse `const` and `mutable` type qualifiers per the spec.
- Emit appropriate opcodes to set ConstMask on `T → const T` assignment.
- Compile-time enforcement: reject `SetProp` / reassignment on const-qualified identifiers (existing infrastructure in `constVars` / `VarDecl::isConst`).
- Emit `typeof()` information that distinguishes `const T` from `T`.

### Layer 7: Identity and Equality

Per the spec:
- `is`: a mutable ref and a const ref to the "same" underlying object are **not** identity-equal (the const ref points to a frozen clone).
- `==`: structural comparison is unaffected by constness.
- `clone()` on a const Value: returns a mutable deep copy (existing `clone()` behavior, applied to the frozen clone's state).

---

## Edge Cases and Considerations

### Cycles and Alias Preservation in Object Graphs
Frozen clones can form cycles just like regular objects. The lazy materialization uses the per-snapshot `CloneContext` (stored in the `SnapshotToken`, shared by all frozen clones of that snapshot) to handle both cycles and aliases — if a frozen clone for an object at a given epoch already exists in the context, reuse it rather than creating a new one. This ensures that alias identity is preserved within the snapshot: if `o.a is o.b` (same underlying object), then `c.a is c.b` (same frozen clone).

### Multiple Snapshots of the Same Object
An object may be part of multiple snapshots at different epochs. Each snapshot has its own frozen clone tree. The version chain on the object stores versions at multiple epochs. Each snapshot resolves to its own epoch independently.

### Nested `const` Conversions
`const c1 = o; const c2 = o` at different times creates two independent frozen clone trees with different snapshot epochs. Mutations between the two snapshots are visible in `c2` but not `c1`.

### `move()` and Sole Ownership
`move()` transfers a value out of its binding and nils the source. It always succeeds — no alias check at the move site. Sole ownership is enforced at actor boundaries: `queueCall()` skips cloning when the value has no other live references (sole owner), and raises an error if a `mutable` actor parameter receives an aliased value. For sole-owner values, `createFrozenSnapshot()` also skips the shallow clone and freezes in-place, making the full `move()` → actor path zero-copy.

### `move()` isolation gap

Currently the createFrozenSnapshot() has a 'sole ownership' short-circuit, but this doesn't check for outstanding interior references, only that the root reference is uniquely owned.  This is a gap in isolation.  Consider ways around it.
(e.g. possible to check if a unique reference is to an isolated graph island?  Is that cheaper than just deep-cloning?)


### Actor Method Parameters (Implicit Const)
Actor method parameters are implicitly const per the spec. At the actor boundary (`queueCall()`), non-primitive arguments are cloned for isolation (unless sole-owned, see above). Inside the actor method, `MakeConst` is emitted to freeze parameters via `createFrozenSnapshot()`. For sole-owner values (e.g. moved or temporary), `createFrozenSnapshot()` skips the shallow clone and just sets the const bit — truly zero overhead. This replaces the current eager deep-copy (`clone()`) on actor calls.

### Builtin Function and Method Const Declarations

Many builtin functions and methods are declared in `.rox` module files (e.g., standard library modules). Their parameter declarations should be updated to add `const` type annotations where appropriate — i.e., parameters that the function reads but does not mutate. For example, a `contains(haystack: const List, needle)` declaration communicates to callers that the list will not be modified.

### Builtin (C++ Implemented) No-Mutate Optimization

For builtins implemented in C++ and registered via native function/method registration, we can add no-mutate flags at registration time to skip unnecessary `T → const T` shallow-clones at call dispatch.

Two independent flags are needed:

- **`noMutateSelf`** — the method does not mutate the receiver's state. Without this, calling a builtin method on a const instance would require the VM to shallow-clone the receiver for snapshot isolation — even for read-only methods like `len()` or `contains()`.
- **Per-argument `noMutate` flags** — each argument is independently annotated. Different arguments can have different mutation requirements (e.g., `copyInto(source, dest)` reads `source` but mutates `dest`). A bitmask (bit N = "arg N won't be mutated") is a compact representation.

| Method | `noMutateSelf` | Arg flags |
|--------|:-:|:-:|
| `list.contains(item)` | yes | `{item: yes}` |
| `list.append(item)` | no | `{item: yes}` |
| `list.sort(comparator)` | no | `{comparator: yes}` |
| `list.indexOf(item)` | yes | `{item: yes}` |

This optimization is worthwhile because the dispatch-time check is essentially free — a single branch on metadata already loaded during function lookup — while even an O(1) shallow clone involves a small allocation plus atomic refcount operations on each child Value. For frequently called builtins that take ref-type arguments but never mutate them (e.g., `len()`, `contains()`, `indexOf()`, math/string query functions), this eliminates unnecessary allocation churn on hot paths.

The key insight: we can still get const **enforcement** without snapshot **isolation**. For a `noMutate` parameter (or `noMutateSelf` receiver), the VM sets bit 48 (ConstMask) on a copy of the Value on the stack — just a bit-flip, truly O(1) — without creating a frozen clone. If the builtin (incorrectly) tried to mutate through that Value, the const flag would still catch it at runtime. But since we trust the `noMutate` annotation, snapshot isolation is unnecessary: no mutable alias will modify the object during the call.

**Constraint:** `noMutate` is only valid for **non-escaping** arguments — the builtin must not store, return, or otherwise make the Value accessible beyond the call frame. If a builtin returns or persists a `noMutate` argument, the const-bit-only Value (lacking a frozen clone) would escape without snapshot isolation, violating the spec's `T → const T` semantics. Builtins that need to return or store a const ref must use the full `T → const T` shallow-clone path.

### Const-to-Const Passthrough

If a Value is already const (bit 48 set, pointing to a frozen clone), a `T → const T` conversion should be a **no-op** — reuse the existing frozen clone and its `snapshotEpoch`. No re-snapshot, no new shallow clone. This arises naturally with `const c2 = c1.m` (where `c1.m` already returns a const Value) and avoids redundant work.

### Method Dispatch on Const Receivers

When a method is called on a const receiver (e.g., `constObj.someMethod()`), the `this` binding must carry const + epoch context. For user-defined Roxal methods, `this` is a const Value (bit 48 set) pointing to a frozen clone — any internal SetProp on `this` hits the const mutation check and errors. For builtin methods, the `noMutateSelf` flag (see above) determines whether snapshot isolation is needed for the receiver.

### Version Save Deduplication

If an object is mutated multiple times while no new snapshots are created between mutations, only the *first* mutation needs to save a version. Subsequent mutations within the same "epoch window" overwrite state that no snapshot can observe.

The skip condition must account for new snapshots created between mutations. Maintain a global `latestSnapshotCreationEpoch` (updated on each `T → const T`). Per-object, track `lastSaveEpoch` (the `globalWriteEpoch` at which `saveVersion()` was last called for this object). The dedup rule:

- **Skip** `saveVersion()` if `obj->lastSaveEpoch >= latestSnapshotCreationEpoch` — no new snapshot was created since the last save, so no snapshot can observe the intermediate state.
- **Save** otherwise — a new snapshot may need the current state as its floor version.

This reduces version chain bloat under write-heavy workloads while remaining correct across interleaved snapshot creations.

**Memory ordering:** Under concurrent snapshot creation (e.g., two actor calls simultaneously), `latestSnapshotCreationEpoch` must be updated **before** `activeSnapshotCount` is incremented (use release semantics on the former, acquire on the latter), so that any mutation seeing `activeSnapshotCount > 0` also sees the updated `latestSnapshotCreationEpoch`. Otherwise, a mutation could skip `saveVersion()` based on a stale `latestSnapshotCreationEpoch`, failing to capture state needed by the newly created snapshot.

### ObjControl Size

Adding `writeEpoch`, `snapshotToken`, and `versionChain` to `ObjControl` costs ~24 bytes per object (8 + 8 + 8). Note that `snapshotToken` is only non-null for frozen clones and `versionChain` is only populated while snapshots are active, but the fields exist on every object. This is an acceptable trade-off for keeping the version infrastructure uniform and avoiding per-type branching. If space becomes a concern, `snapshotToken` and `versionChain` could be moved into the Obj subtypes (at the cost of per-type code duplication for version resolution).

### Thread Safety and Concurrent Actor Access

When a mutable Value is passed to an actor method, the implicit `T → const T` conversion creates a frozen clone root. The actor then reads through that const ref on its own thread while the caller thread may concurrently mutate the same underlying objects. The key concurrent accesses are:

**Version chain reads (actor's const read path):**
The version chain is a singly-linked list where new versions are only ever prepended (never removed while readers are active). The head pointer can be updated with an atomic store (or CAS — compare-and-swap, i.e., `std::atomic::compare_exchange`, a lock-free primitive that says "set X to new_value only if X currently equals expected_value, otherwise retry"). Readers walk from head to tail. Any newly prepended nodes have epochs newer than the actor's snapshot epoch, so readers naturally skip them. This makes the common **read path lock-free and uncontended** — which is the main advantage of MVCC over locking: we assume no mutation is the usual case and optimize for reads.

**`saveVersion()` and frozen clone materialization vs concurrent mutation:**
Both `saveVersion()` (shallow-cloning current state) and frozen clone materialization (reading properties to copy into a clone) need to read the object's property data. If another thread is simultaneously mutating those properties, this is a data race.

A seqlock is **not sufficient** here: C++ containers like `std::unordered_map` (currently used for `ObjectInstance::properties`) exhibit undefined behavior on concurrent read/write regardless of whether the torn state is detected afterward. The UB occurs during the read itself (e.g., following a dangling internal pointer mid-rehash), before the seqlock can detect the conflict.

**Solutions** (choose per Obj subtype based on access pattern):

1. **Fixed-schema types (ObjectInstance, ActorInstance):** The type definition knows all property names at compile time. Layer 0 encapsulation provides the opportunity to change internal storage from `unordered_map` to a **flat array of Value slots** indexed by property slot number (assigned at type definition time). Flat arrays are POD-like — concurrent read/write of different slots is safe, and a seqlock protects reads of the same slot being written. This is the preferred approach for the hot path.

2. **Dynamic containers (ObjDict, ObjList):** These can grow and have dynamic keys/indices. Use a **per-object reader-writer lock** (rwlock): writers acquire exclusive access for mutation and `saveVersion()`; readers (frozen clone materialization) acquire shared access. The rwlock is only acquired when `activeSnapshotCount > 0` (zero overhead otherwise). For ObjList, if the elements vector is not resized during mutation (e.g., element assignment vs append), a seqlock on the size + per-element atomics may suffice.

3. **Conservative fallback:** Use a per-object rwlock for all Obj subtypes. Simple, correct, and the overhead is bounded to the snapshot-active window.

**Summary of synchronization:**

| Operation | Mechanism | Overhead |
|-----------|-----------|----------|
| `writeEpoch` read/write | `std::atomic<uint64_t>` | negligible |
| `versionChain` prepend | atomic head pointer (CAS) | lock-free |
| `versionChain` read (walk) | lock-free (append-only list) | zero |
| Property read/write (fixed-schema) | flat array + seqlock | near-zero |
| Property read/write (dynamic containers) | per-object rwlock (only when snapshots active) | low |
| `activeSnapshotCount`, `minActiveSnapshotEpoch` | `std::atomic<uint64_t>` | negligible |

No global locks are needed on any path. The per-object synchronization (seqlock or rwlock) only matters when snapshots are active and concurrent mutation occurs on the same object — a narrow window in practice.

### GC Integration

**Frozen clones** participate in normal ref counting and GC — they are regular Obj instances with their own ObjControl.

**ObjVersion nodes** hold a snapshot `Obj*` that must be kept alive while the version entry exists. Current object allocation starts with `strong = 0` — ownership is only established via `Value` or explicit `incRef`. Since `ObjVersion` stores a raw `Obj*` (not a `Value`), it must **explicitly call `incRef()` on the snapshot's ObjControl** when the version node is created, and **`decRef()` when the version node is freed**. Alternatively, the snapshot can be stored as a `Value` member inside `ObjVersion` (leveraging existing RAII ref counting), which is simpler and less error-prone at the cost of 8 bytes per version node.

**GC tracing:** Each mutable Obj subtype's `trace()` method must be extended to visit all Values held in version chain snapshot objects, so that the GC marks objects referenced only from historical snapshots. Similarly, `dropReferences()` must clear the version chain (releasing all snapshot objects and their Values) when the owning object is collected.

**Version chain trimming** can be integrated into the GC sweep: discard versions older than `minActiveSnapshotEpoch` (keeping the floor version — see §6). Since readers may be concurrently walking version chains (lock-free), trimming must use safe reclamation: either perform trimming only during GC stop-the-world pauses, or use epoch-based reclamation (RCU-style) where freed nodes are deferred until no reader can hold a reference.
