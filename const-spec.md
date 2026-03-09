# Roxal Constness (Immutability) Specification

## Overview

Roxal supports transitive immutability via the `const` keyword, enabling safe data sharing while preventing unintended mutations. When a value is made const, it becomes a **frozen snapshot** - neither the value nor any of its reachable sub-objects can be mutated through that reference.

Mutable values can be converted to const values, but mutations via existing mutable references will not be visible via the const reference.

---

## Keywords

### `const`

Declares an identifier or type as immutable.

**On identifiers:** Prevents reassignment and (for reference types) mutation.
```roxal
const x: int = 5       # Cannot reassign x
const l: List = [1,2]  # Cannot reassign l, cannot mutate list
```

**On types:** Marks the type as immutable.
```roxal
var x: const List = [1,2,3]  # Can reassign x, cannot mutate the list
```

### `mutable`

Explicitly opts out of implicit const behavior. Used in specific contexts where const would otherwise be implied.

```roxal
const l: mutable List = [1,2,3]  # Cannot reassign l, CAN mutate list (rare)
```

---

## Implicit Const on Identifier Declarations

When `const` is applied to an identifier with a reference type, the type is **implicitly const** as well:

| Declaration | Meaning | Notes |
|-------------|---------|-------|
| `const l: List = ...` | `const l: const List` | Implicit const type |
| `const l: mutable List = ...` | const ref, mutable type | Explicit opt-out (rare) |
| `var l: const List = ...` | mutable ref, const type | Reassignable to different const list |
| `var l: List = ...` | mutable ref, mutable type | Fully mutable |

**Rationale:** When declaring `const`, developers expect full immutability. The Java-`final` pattern (const ref to mutable object) is a common source of bugs and rarely intentional.

**Initialization requirement:** `const` declarations require an initializer (since they can't be assigned later).

```roxal
const l: List = [1, 2, 3]  # OK
const l: List              # ERROR: const declaration requires initializer
```

---

## Transitive Constness

Constness is **transitive** - accessing a property of a const value yields a const value (unlike C++, for example):

```roxal
object Inner:
    var num: int

object Outer:
    var inner: Inner

const o: Outer = Outer(Inner(42))
o.inner.num = 99  # ERROR: Cannot mutate const (transitive)
```

This applies to:
- Object properties
- List/Dict elements
- Nested structures of any depth

---

## Type Conversions

| Conversion | Behavior |
|------------|----------|
| `T → const T` | Implicit, creates frozen snapshot |
| `const T → T` | Prohibited |
| `const T.clone() → T` | Explicit copy, returns mutable |
| `move(lvalue) → T` | Ownership transfer, invalidates original |

### T → const T: Frozen Snapshot

When converting a mutable value to const, a **frozen snapshot** is created. The const reference sees the value as it was at conversion time - subsequent mutations through other references do not affect it.

```roxal
var a = [1, 2, 3]
const b: List = a   # Frozen snapshot of a

a.append(4)         # Mutate through a
print(b)            # [1, 2, 3] - b sees original state
```

This applies transitively to nested objects:

```roxal
var inner = Inner(42)
var outer = Outer(inner)
const c: Outer = outer

inner.num = 99      # Mutate original inner
print(c.inner.num)  # 42 - c has isolated snapshot
```

### clone() Returns Mutable

The `clone()` method returns a mutable copy:

```roxal
const c: List = [1, 2]
var d = c.clone()   # Mutable copy
d.append(3)         # OK
```

`clone()` preserves 'is' reference identity relationships within the copy.

### move() Transfers Ownership

`move()` transfers sole ownership and invalidates the source:

```roxal
var a = [1, 2, 3]
var b = move(a)
print(a)            # ERROR: use of moved variable
```

`move()` **requires sole ownership** - it yields an error if the value has other references:

```roxal
var a = [1, 2, 3]
var b = a           # Aliased
var c = move(a)     # ERROR: Cannot move aliased reference
```

---

## Type Classification

| Type | Const Behavior |
|------|----------------|
| Primitives (int, real, bool, byte) | Already immutable (value types) |
| String | Already immutable |
| List | Prevents add/remove/modify elements |
| Dict | Prevents add/remove/modify entries |
| Object | Prevents property mutation |
| Actor | Prevents internal state (private property) mutation |
| Event | **Implicitly const** (see below) |
| Signal | `const Signal` disallowed |
| Vector/Matrix/Tensor | Prevents element mutation |

### Signal Restriction

Signals cannot be const - they exist to change over time:

```roxal
const s: Signal = ...  # ERROR: const Signal is not allowed
```

---

## Actor Method Mutability

### Parameters

All non-primitive parameters to actor methods are **implicitly const**:

```roxal
actor Worker:
    proc process(data: List):  # data is implicitly const List
        data.append(5)         # ERROR: Cannot mutate const list

        var local = data.clone()
        local.append(5)        # OK - local mutable copy
```

**Rationale:** Actors shouldn't mutate caller's data. This prevents shared-state bugs.

### Mutable Parameters

Use `mutable` to override implicit const. Caller must use `move()`:

```roxal
actor Processor:
    func mutate(data: mutable List) -> mutable List:
        data.append(5)  # OK - sole ownership
        return data

var myList = [1, 2, 3]
var result = processor.mutate(move(myList))  # myList invalidated
```

### Return Types

| Return Type | Behavior |
|-------------|----------|
| `-> T` (default) | Returns frozen snapshot (const) |
| `-> mutable T` | Returns sole mutable ownership |

```roxal
actor DataStore:
    var data: List = [1, 2, 3]

    func getData() -> List:
        return data           # Caller receives const snapshot

    func extractData() -> mutable List:
        return move(data)     # Transfers ownership, invalidates internal
```

For `mutable` return, the value must have sole ownership:

```roxal
actor Factory:
    func create() -> mutable Widget:
        var w = Widget()
        return w              # OK - local, sole reference

    func badReturn() -> mutable Widget:
        var a = Widget()
        var b = a             # Aliased
        return a              # ERROR: Cannot return mutable reference
```

---

## Event Implicit Const

Event types have all members **implicitly const**:

TODO: rethink this - perhaps event members don't need to be const, but delivered event instance can be const reference

```roxal
event UserClicked:
    var x: int
    var y: int
    var target: Widget

# In handler:
on UserClicked(e):
    print(e.x)            # OK - read
    e.x = 50              # ERROR: Cannot mutate const event member
    e.target.name = "x"   # ERROR: Cannot mutate const (transitive)
```

**Rationale:**
- Events are immutable snapshots of "what happened"
- Once emitted, event data shouldn't change
- Multiple handlers may receive the same event

No `mutable` override is allowed for event members.

---

## Identity and Equality

### `is` - Reference Identity

`is` compares reference identity (same object) for all reference types:

```roxal
var a = [1, 2, 3]
var b = a
a is b              # TRUE - same object

var c = [1, 2, 3]
a is c              # FALSE - different objects with same content
```

**Note:** A mutable reference and const reference to the same underlying object are **not** considered identical via `is`.

### `==` - Value Comparison

`==` behavior varies by type:

| Type | `==` Behavior |
|------|---------------|
| Primitives (int, real, bool, etc.) | Value comparison |
| String | Value comparison |
| List | Structural comparison (element-wise) |
| Dict | Structural comparison (key-value pairs) |
| Vector/Matrix/Tensor | Structural comparison (element-wise) |
| Object | Reference equality (same as `is`) |
| Actor | Reference equality (same as `is`) |

```roxal
# List: structural comparison
var a = [1, 2, 3]
var b = [1, 2, 3]
a == b              # TRUE - same contents
a is b              # FALSE - different objects

# Object: reference comparison
var p1 = Point(1, 2)
var p2 = Point(1, 2)
p1 == p2            # FALSE - different objects
p1 is p2            # FALSE - different objects

var p3 = p1
p1 == p3            # TRUE - same object
p1 is p3            # TRUE - same object
```

---

## Summary

1. **`const` on identifier** → Cannot reassign, and for reference types, implicitly applies `const` to the type
2. **`const` on type** → Cannot mutate instances
3. **Transitive** → Nested access yields const
4. **T → const T** → Creates frozen snapshot (isolated from future mutations)
5. **const T → T** → Prohibited (use `clone()` for mutable copy)
6. **Actor params** → Implicitly const (use `mutable` + `move()` for ownership transfer)
7. **Actor returns** → Default const snapshot (use `mutable` for ownership transfer)
8. **Events** → Implicitly const (immutable snapshots)
9. **Signals** → Cannot be const
