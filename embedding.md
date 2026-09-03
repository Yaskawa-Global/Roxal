# Embedding Roxal

For host authors: how to run Roxal inside another program.

`implementation-notes.md` covers the runtime internals behind this — rooting
rules, the stop machinery, how the driver and finalizer hand ownership over.
This file is the contract you program against.

There are three shapes. Pick by what your host owns.

| Your host | Recipe |
| --- | --- |
| has no loop of its own | [synchronous program](#1-synchronous-program) |
| offers a persistent prompt or console | [fragment session](#2-fragment-session) |
| owns a periodic loop with a deadline | [embedded driver](#3-embedded-driver) |

---

## 1. Synchronous program

The command line's shape. Compile and run to completion on the calling thread.

```cpp
std::ifstream source(path);
roxal::ProgramOptions options;
options.sourceName = path;
const roxal::ExecutionStatus status = vm.executeProgramSync(source, std::move(options));
```

```
calling thread
  executeProgramSync(src)
    compile -> run to completion -> join threads -> completion hooks -> OK

  (debugger launch)
  stageProgramSync(src) -> set breakpoints -> executeStagedSync()
     compile, bind thread,                     onScriptStart, then body
     run preludes
```

Preludes run during staging, before a debugger can be configured: they are
your own pre-setup, not the user's program, so a breakpoint or `stopOnEntry`
never lands inside them, and `onScriptStart` fires after them on every path.

`executeProgramSync()` blocks for as long as the program runs, joins the
threads the program started, and runs module completion hooks before returning.
It refuses with `Busy` if an embedded driver owns execution.

**Debugger launches** need the two halves apart, so breakpoints can be set
between compilation and the first statement:

```cpp
if (vm.stageProgramSync(source, std::move(options)) == roxal::ExecutionStatus::OK) {
    configureBreakpoints();
    vm.executeStagedSync();
}
```

## 2. Fragment session

A REPL line and a fresh program are different source lifetimes. A fragment
sees what earlier fragments declared; a fresh program never does. That
distinction is a session, which owns the compiler, module and thread that
fragments share.

```cpp
roxal::ReplSession session = vm.defaultReplSession();
std::stringstream line("var x = 5\n");
session.evaluateFragmentSync(line);
```

How a fragment is advanced depends on whether a driver is attached:

```
no driver attached                   driver attached
------------------                   ---------------
calling thread                       producer            driver thread
  evaluateFragmentSync(src)            prepareFragment(src)
    compiles                             -> PreparedProgram
    runs to completion on              submit(..) --------> driveFor(budget)
    the SESSION's thread                                      binds the
  -> OK                                                       session's
                                                              thread; fires
                                                              no script hooks
```

`prepareFragment()` returns `SessionBusy` rather than compiling against state a
live fragment is still mutating. The claim it takes on the session belongs to
the prepared fragment: drop the fragment without running it and the session is
idle again, with nothing to release by hand. With a driver attached, `evaluateFragmentSync()`
refuses — it would be a second driver on the same VM — so submit the fragment
through the runtime instead (recipe 3).

A fragment runs on its session's thread, so handlers and actors an earlier
fragment registered are still serviced. It fires no script start/complete hooks
and is not joined: it is a continuation of its session, and its actors may
outlive it deliberately.

## 3. Embedded driver

Your host owns a periodic loop and cannot afford unbounded work in it.

```cpp
// Once, at startup.
auto attached = vm.attachEmbeddedRuntime();
roxal::EmbeddedRuntime& runtime = *attached.runtime;
vm.stopCoordinator().setHostControl(&myHoldSink);      // optional; see below
```

```
producer thread              driver thread                 finalizer thread
---------------              -------------                 ----------------
attachEmbeddedRuntime()
prepareProgram(src)
  compiles; unbounded;
  never on the driver
submit(prepared) ----------> driveFor(500us) -> Idle
                             driveFor(500us) -> Yielded    (activate, preludes)
                             driveFor(500us) -> Yielded    (body)
                                    ...
                             driveFor(500us) -> ExecutionEnded
                                                     |
                                                     +---> run.wait()
                                                             join threads,
                                                             completion hooks
                                                           -> Completed
```

**Prepare off the driver.** Compilation parses, reads and writes the bytecode
cache, allocates and updates shared registries. It has no budget parameter and
must never run in a control cycle.

```cpp
roxal::ProgramOptions options;
options.sourceName = "program.rox";
options.imports  = { myStdlibModule };            // visible without editing source
options.preludes = { { receiver, u"bind" } };     // runs before the body
auto prepared = vm.prepareProgram(source, std::move(options));
```

**Submit.** Ownership transfers *only* on acceptance: on any rejection you
still own your `PreparedProgram` and can retry or drop it. Nothing the VM holds
can execute it later.

```cpp
auto submitted = runtime.submit(std::move(prepared.program));
switch (submitted.status) {
case roxal::SubmitStatus::Accepted:  break;
case roxal::SubmitStatus::RunActive: /* one run at a time; you still own it */ break;
default: break;
}
```

**Drive.** On your loop thread, every period:

```cpp
const roxal::SliceResult r = runtime.driveFor(roxal::TimeDuration::microSecs(500));
```

| `SliceState` | What to do |
| --- | --- |
| `Idle` | nothing to advance |
| `Yielded` | budget spent — call again next period |
| `Blocked` | waiting on a deadline or event; `retryAt` says when, if known |
| `DebugPaused` | a debugger stop is active — **keep calling at your normal cadence** |
| `MainReturned` | the body returned; not completion under the quiescent policy |
| `ExecutionEnded` / `ExecutionFailed` | published once: hand this run to a non-driver finalizer |

The first `driveFor()` latches your thread as *the* driver. A second thread
calling it advances nothing.

**Bracket the slice in a GC yield section.** A slice declines when a
collection is pending, but a collection can begin in the instant between that
check and the slice taking its mutator cover — and the cover then waits on the
collection barrier. A hard-real-time host closes that window the way the
reference harness does: enter `SimpleMarkSweepGC::GCYieldScope` first, and if
it refuses (a collection is pending), do no Roxal work this cycle at all.

```cpp
if (roxal::SimpleMarkSweepGC::GCYieldScope gcs{}; gcs)
    r = runtime.driveFor(roxal::TimeDuration::microSecs(500));
else
    /* collection pending: skip Roxal this cycle */;
```

**Finalize off the driver.** Joining the launch's threads and running
completion hooks is unbounded, so it never happens in a slice.

```cpp
const roxal::FinalizeResult done = submitted.run.wait();   // NOT on the driver thread
```

Exactly one caller performs the finalization; others block for its result.
The run reaches `Completed` — or `Failed` — only after that: both are
**post-finalization** states. Before it, a run that failed reports
`FinalizationPending` with `RunHandle::failed()` set, so a waiter never has to
infer failure from a state finalization has not reached yet. A completion hook
that throws is reported as `Failed` too; it cannot leave the run parked.

Until that finalization is done the run still owns its slot: `submit()`
refuses with `RunActive`, and `detachEmbeddedRuntime()` refuses, because the
next run's terminal transition would otherwise overwrite this one's record.

**What a handle can tell you afterwards.** `RunHandle::diagnostic()` is why
the run failed (from the handover on), and `RunHandle::result()` is the
program's terminal value, rendered — bounded, with no user code run, a body
that returns nothing rendering as `nil`. Both are copied text, not Roxal
values: a handle holds no `Value`, which is what lets it outlive the VM.
`RunHandle::exitCode()` is the code the program passed to `exit()`, if it
ended that way.

**`exit()` in a driven program ends that run — nothing else.** It sets the
run's own exit flag, wakes the run's own threads, and returns; the driver's
next slice sees the body return and hands the run over as `Completed`, with
the code on the handle. It does not stop the dataflow engine, does not join
anything on the driver, and does not touch the REPL session or the services.
(On the synchronous path, where the process *is* the program, `exit()` keeps
its process semantics.)

### Being told to hold

A host with machinery attached implements `DebugHostControl` so debugger
activity can request a hold:

```cpp
struct MyHost : roxal::DebugHostControl {
    roxal::DebugHostResult onDebugHoldRequested(
            const roxal::DebugHoldRequest& r) noexcept override {
        return holdQueue.tryPush(r.holdGeneration) ? roxal::DebugHostResult::Queued
                                                   : roxal::DebugHostResult::Rejected;
    }
    // ...onDebugStopped / onDebugContinueRequested / onDebugStopFailed /
    //    onDebugSessionEnded
};
```

Every method must be bounded, non-blocking and `noexcept`, and must not re-enter
Roxal. Perform the actual halt on your own thread, reached through a queue. The
request carries only copied data — no `Value`, no frame pointer, no borrowed
string — so it is safe to hand across threads. The full interface is
[compiler/debug/DebugHostControl.h](compiler/debug/DebugHostControl.h).

What you are guaranteed:

- `onDebugHoldRequested` fires **exactly once per hold generation**, never on
  the thread that discovered the stop and never on your driver.
- `Queued` means you accepted responsibility for a controlled halt. **It does
  not mean anything is stationary**, and the debugger never waits for it.
- **Stepping keeps the hold.** Step in/next/out emit no continue notice, so
  stepping can never restart what you are holding.
- A normal continue emits **exactly one** `onDebugContinueRequested`.
- Terminate, disconnect, fatal error, failed stop and session end never
  silently release a hold. You get the notice; the policy is yours.

## Rules that apply to every shape

**One driver per VM.** The `Busy` semantics and the debugger's stop protocol
both assume it.

**`Busy` means no work was done.** It is not completion. `OK` is.

**A slice never blocks.** The debugger returns `DebugPaused` rather than
parking your driver, and a pending collection makes a slice decline rather than
delay the collector.

**Handles are safe to keep; the runtime pointer is not.** A `RunHandle` holds
no Roxal values and may outlive the VM: it reaches its runtime through a lease
that keeps the object alive only for the duration of one call, so a `wait()`
racing shutdown returns (`Cancelled`, or the finalization it won) rather than
touching freed memory. `embeddedRuntime()` is a non-owning observation, valid
only while attached. Shutdown refuses the driver's next slice, waits out any
slice already in flight — bounded by your budget — and only then cancels the
run that slice was advancing. For that reason, never shut down or detach the
runtime from *inside* a slice — from driven script code or a hook it calls —
the shutdown would be waiting on itself.

**Output sinks must not block.** Install an `OutputSink` that drops when full;
producers include the driver.

```cpp
roxal::OutputRouter::setSink(&mySink);    // before execution starts
```

## Moving from the superseded API

`run()`, `runWithImports()`, `setup()`, `setupLine()`, `runLine()`,
`runPrepared()`, `runFor()`, `addScriptPrelude()` and the `RTState` surface
have been removed. `implementation-notes.md` carries the [old-to-new
table](implementation-notes.md#migration-from-the-superseded-api).

## Worked example

`tests/embedded_host_harness.cpp` is a complete host: producer, periodic
driver, debug controller and hold sink, with the contract above asserted rather
than described. It is the reference to read when this file is ambiguous.
