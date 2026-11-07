# Clocked signals are sampled: staged writes, tick-time publish

Status: agreed 2026-11-07 (David), implemented alongside this note.
Motivated by FC's unified I/O: hardware input ports as clocked signals fed
from C++, and output ports written by scripts.

## The two defects this closes

**1. A clocked source written twice within one period fires a phantom
change.** `set()` on a periodic signal wrote the sample for the next tick
boundary (`tickStart + period`), which is the right *sample* semantics, but
change detection ran at write time against the sample before the boundary.
With the signal false, `s <- true; s <- false` fired `becomes true` at once
(true differs from the previous tick's false), then `set(false)` compared
false with that same false and fired nothing. The history ended up right
(one false sample); the event, and any C++ subscriber such as a fieldbus
output write, had already acted on a value that never stood at a boundary.

**2. A clocked signal fed from C++ never re-evaluates its consumers.**
A node runs for tick t only when every input's "available at" time has
reached t. That time is advanced by the tick (for sources only), by `set()`
and by `publishSample` (event-driven only). A clocked NON-source signal that
a host wrote with `setValueAt` hit none of these, so a node lifted over it
evaluated once (at lift) and never again. `when`/`until` still worked, as
they ride on the change callbacks.

**3. (found while landing this) A wrapped source was pruned from the tick
schedule when a node over it died.** `removeFunc` called `removeSignal`
(unforced) on a dead node's inputs, and an unforced `removeSignal` dropped
any signal no node used, however many script values still held it. The
throwaway comparison in FC's `GripperImpl.move_to` (`bool(this.cmd_gap ==
nil)`) lifts such a node; once it was collected, `cmd_gap` was no longer
ticked. Under the old write-time publish this was invisible (only a clock
would have stopped counting); under sampling the source read its staged
write back forever and never published. Fix: an unforced `removeSignal`
leaves a signal with a live wrapper alone; the last wrapper's destructor
prunes it (forced). Test: `tests/signal_source_outlives_node.rox`.

## Semantics

A **clocked source** is sampled on its own grid:

- `set(v)` / `sig <- v` **stage** `v`. Nothing is published, no callback
  fires. A later write before the boundary replaces the staged value: the
  last write wins, intermediate writes are invisible everywhere.
- The signal's **tick** (engine thread, or the host's `tickFor`) publishes
  the staged value as the sample at that boundary, compares it with the
  previous sample, fires the change callbacks once if it differs, and marks
  availability for its consumers. A tick with nothing staged publishes
  nothing (the history zero-order-holds as before).
- `.value` (`lastValue()`) returns the **staged value when one is pending**,
  otherwise the last published sample. A writer reads its own command back
  immediately and deterministically; nodes, samplers (`valueAt`), `when`,
  `until` and C++ subscribers see only the grid.
- A clocked source that is **not script-writable** (`setScriptWritable(false)`)
  is a host-fed input: the engine ticks it like any source, the host stages
  its values with `set()`, and `.set` / `<-` from a script, or wiring it as
  the left side of `<-`, is refused by name. "Source" means *ticked by the
  engine*; writability is a separate property.

Unchanged:

- **Event-driven** signals (frequency 0): `set()` publishes immediately,
  every write is an event, as before.
- `setValueAt(t, v)` stays the engine's raw publish for node outputs, wires
  and derived signals at the evaluation time. Hosts do not call it on
  clocked signals any more; a clocked non-source has no tick, so `set()` on
  one is a logic error.
- Change detection compares with the sample before t (`lastValueBefore`).
  With staging there is at most one publish per boundary, so the
  write-time comparison can no longer misfire.

Clocks already work this way: `tick()` computes the value and publishes it
at t. A host-fed source is a clock whose next value comes from the host.

## Ordering in an embedding host

FC's cycle on the RT thread is: drain the bus snapshot (stage), `tickFor`
(publish, evaluate nodes, queue handler events), `driveFor` (the main
thread's slice runs its `when` handlers). A bus edge drained at the start
of cycle N is a sample at tick N, its nodes evaluate at tick N and its
handlers run in cycle N. `when` handlers always ran on the thread that
declared them (`scheduleEventHandlers` queues onto that Thread); only raw
C++ subscribers run inline, now on the ticking thread instead of the
writer's.

`set()` from any thread is a locked swap of the staged slot; the old
`tickStart + period` write from a non-engine thread could land on a tick the
engine had already processed.

## Host recipe (FC)

    auto sig = df::Signal::newSourceSignal(rateHz, initial, key);
    sig->setScriptWritable(false);       // an input
    ...
    sig->set(valueFromBus);              // every cycle, before tickFor

An output port is a plain clocked source; the bus write subscribes to its
changes and runs at the tick with the final value of the period.

## Tests

- `tests/signal_clocked_sampled.rox`: a 20 Hz source; `<- true; <- false`
  within a period fires nothing and reads false; a single write fires once
  at the next boundary; `.value` reads back the staged write before the
  tick; a lifted node over the source re-evaluates after each publish.
- `tests/dataflow_host_thread_test.cpp` scenario `host_fed_input`: a
  non-writable clocked source staged from the host and ticked with
  `tickFor`; the node over it follows each staged value (defect 2).
