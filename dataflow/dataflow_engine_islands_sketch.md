# DataflowEngine island scheduling & event-driven signal sketch

## Overview

The current engine holds one global schedule derived from every signal period.
That forces unrelated subsets of the network to share a cadence and prevents
monitor-only signals from existing without a period. The proposed changes split
the engine into independently scheduled "network islands" and add an
explicit event-driven signal mode for monitors.

## 1. Partition scheduling by connected components

### New data structures

* `struct NetworkIsland`
  * `std::vector<ptr<Signal>> signals`
  * `std::vector<ptr<FuncNode>> funcs`
  * `TimeDuration tickPeriod` (per-island GCD)
  * `TimePoint nextWakeTime`
  * Cached topo order keyed by execution interval (replacement for the global
    `precomputedExecutionOrders`)
  * Maps from signal -> consumers / producers scoped to the island
  * Flags for `networkDirty`, `containsAsyncSignals` (see section 2)
* `std::vector<NetworkIsland> m_islands` inside `DataflowEngine`
* A dedicated `NetworkIsland m_monitorIsland` that aggregates signals with zero
  consumers and event-driven signals so they do not create one-island-per-signal

### Discovering islands

* Extend `buildSignalConsumers()` to fill a bidirectional adjacency graph:
  `signal -> funcs`, `func -> signals`.
* Add a DFS/BFS helper that visits only signals/funcs reachable from a seed.
  While visiting, accumulate them into a pending `NetworkIsland`.
* After traversal, stash the island, compute its own consumers/producers, and
  build its execution cache.
* After processing connected components, sweep remaining signals with zero
  consumers into `m_monitorIsland`.

### Replacing global scheduling state

* Remove `precomputedExecutionOrders` and the single `m_tickPeriod`.
* `tickPeriod()` becomes the minimum over island periods (or zero if only
  async islands exist). Expose a helper to query per-island periods for tests.
* Rework `run/runFor/tick/evaluate` to iterate over `m_islands`:
  * `tick()` now chooses the next island wake time (min of `nextWakeTime`).
  * For each island whose `nextWakeTime <= now`, call a refactored
    `evaluateIsland(NetworkIsland&, TimePoint now)` that executes functions in
    interval order and updates `nextWakeTime += island.tickPeriod`.
* `evaluate()` becomes a one-shot: it temporarily runs
  `evaluateIsland(...)` for each island at `TimePoint::currentTime()` without
  advancing `nextWakeTime`.

### Execution cache per island

* Move `precomputeFuncPeriods()` and `precomputeExecutionOrders()` logic into an
  island method. Each island maintains `std::map<TimeDuration, std::vector<ptr<FuncNode>>>`.
* Provide helper `NetworkIsland::executionOrderFor(TimeDuration)` that returns
  the cached topo order.
* Update `evaluateIsland()` to pull from the island’s cache instead of the
  global map.

### APIs & bookkeeping

* Update `consumersOfSignal`, `producersOfSignal`, and `consumerCount` to locate
  the island for the queried signal and use its scoped maps.
* When removing or copying signals, mark the owning island dirty. On the next
  tick/evaluate call, rebuild all islands if `m_networkModified` is set.
* Ensure island rebuild resets `nextWakeTime` to `now + tickPeriod` to avoid
  stale wake times.

### Testing

* Add an integration test that builds two disconnected networks with periods 5
  ms and 40 ms. Simulate ticks and assert that each island only advances on its
  own cadence (no cross-triggering).
* Add a case where removing signals collapses islands and the cache recomputes
  without stale references.

## 2. Event-driven (async) signals for monitors

### Signal-level changes

* Extend `Signal` with an optional period:
  * Change constructors to accept either `TimeDuration period` or
    `std::optional<TimeDuration>`.
  * Keep existing helpers calling through with a concrete period.
  * Add `bool isEventDriven() const` and return true when no period is supplied.
* Permit `frequency == 0` or `period == TimeDuration::zero()` as the sentinel
  for event-driven sources.

### Engine ingestion

* During island discovery, track whether a signal is event-driven. Such signals
  do not contribute to the island’s GCD. If an island contains only
  event-driven sources, its `tickPeriod` becomes `std::nullopt` and the island
  is added to `m_monitorIsland`.
* When computing execution intervals for functions, treat event-driven inputs
  as having no periodic constraint: the function is tagged event-driven if any
  input is event-driven.
* Store event-driven functions in the monitor island’s cache with a special
  interval token (e.g., `std::nullopt` key) executed on-demand.

### Dispatching updates

* Add `DataflowEngine::triggerSignal(ptr<Signal>, TimePoint timestamp)` that
  enqueues the signal into the monitor island and marks its consumers ready.
  `Signal::setValue`/`setValueAt` should call `triggerSignal` when the signal is
  event-driven.
* Modify `tick()` so the monitor island runs only when it has pending triggers.
  It can maintain a queue of pending signals/functions and process them
  immediately without waiting for a period boundary.
* Ensure event-driven execution respects dependencies: propagate through any
  derived functions that only depend on event-driven signals without scheduling
  periodic island ticks.

### API tweaks

* `tickPeriod()` should ignore event-driven islands so GUI monitors do not alter
  the reported engine period.
* Provide `bool Signal::isEventDriven()` to external callers so GUI components
  can confirm they do not affect timing.

### Testing

* Add unit/integration tests covering:
  * Event-driven source feeding a monitor function; verify it does not alter the
    periodic island’s cadence.
  * Mixed island with periodic & event-driven signals still produces a valid
    GCD from periodic inputs.
  * Triggering event-driven signal multiple times in a single engine tick causes
    immediate re-evaluation of dependent functions.

## Migration considerations

* Update call sites that create `Signal` objects to pass the explicit period or
  use a helper `Signal::createEventDriven(...)`.
* Review serialization/graphviz/debug output to label event-driven signals and
  islands.
* Ensure `clear()` resets monitor island queues and async flags.

