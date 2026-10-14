#pragma once

#include <cstdint>
#include <string>

namespace roxal {

// Why a debugger stop is being requested.
enum class DebugStopReason : uint8_t {
    Pause,        // explicit user/host pause request
    Breakpoint,   // a source breakpoint was hit
    Step,         // a step (in/next/out) completed
    Exception,    // exception/fatal-error stop policy
    Host,         // embedding-host-initiated stop
};

// Plain copied data only -- no Value, no frame pointer, no callback into the
// VM, no borrowed string.
struct DebugHoldRequest {
    uint64_t sessionId { 0 };
    uint64_t holdGeneration { 0 };
    uint64_t stopEpoch { 0 };
    DebugStopReason reason { DebugStopReason::Pause };
    uint64_t threadId { 0 };          // discovering thread, when known
    std::string sourceName;           // stop location, when known
    int line { 0 };
};

struct DebugStopNotice   { uint64_t sessionId {0}; uint64_t holdGeneration {0}; uint64_t stopEpoch {0}; };
struct DebugContinueNotice { uint64_t sessionId {0}; uint64_t holdGeneration {0}; };
struct DebugStopFailure  { uint64_t sessionId {0}; uint64_t holdGeneration {0}; uint64_t stopEpoch {0};
                           std::string failedThread; };
struct DebugSessionEnd   { uint64_t sessionId {0}; bool wasStopped {false}; };
struct DebugStepNotice   { uint64_t sessionId {0}; uint64_t holdGeneration {0}; uint64_t threadId {0}; };

enum class DebugHostResult : uint8_t {
    Queued,     // host accepted responsibility for asynchronously performing
                // a controlled motion hold (nominally within ~1s).  Does NOT
                // mean anything is physically stationary.
    Rejected,   // host could not accept (queue full, shutting down, ...) --
                // recorded and surfaced as a debugger warning; a safety-critical
                // host integration treats this as a safety fault and keeps
                // execution stopped.
};

// Local embedding hook for a host that must be notified when debugger
// activity requires external activity -- robot motion -- to be held
// (stop-now-and-notify; the host owns the controlled halt).  Independent
// of any debug transport.
//
// Contract:
//  - every method is bounded, non-blocking, noexcept, and must not
//    synchronously re-enter Roxal or the debugger;
//  - every notice comes from a non-RT thread, never from the RT/interpreter
//    callback that discovers a stop condition, and never from the driver;
//  - onDebugHoldRequested() is invoked exactly once per running-to-held
//    hold generation;
//  - stepping retains the hold generation -- no continue notice between
//    steps.  Each step that resumes the domain sends one
//    onDebugStepResumed() (every thread in the domain runs during a step,
//    not only the stepping one);
//  - a stop that forms while the generation is already held -- the step's
//    own landing, a breakpoint / pause / host hold / exception preempting
//    it, or the first stop after an execution that ended held -- sends
//    onDebugHoldReasserted(), never a second onDebugHoldRequested();
//  - ordering: a step's onDebugStepResumed() is delivered before any
//    onDebugHoldReasserted() for the stop that ends it (both are sent under
//    the coordinator's control mutex), so a host can arm a debounced
//    release on the former and cancel it on the latter;
//  - a normal continue produces exactly one onDebugContinueRequested;
//  - terminate/disconnect-while-stopped/fatal/failed-stop never silently
//    release a hold: the corresponding notice is sent and the host applies
//    its own fail-safe policy;
//  - an exit never releases the hold, whether its stop had committed or was
//    still pending: no continue notice.  The generation outlives the run;
//    the host closes it with StopCoordinator::holdDropped() once it has dealt
//    with what it held, and the next stop opens a fresh generation.
struct DebugHostControl {
    virtual ~DebugHostControl() = default;
    virtual DebugHostResult onDebugHoldRequested(const DebugHoldRequest&) noexcept = 0;
    virtual void onDebugStopped(const DebugStopNotice&) noexcept = 0;
    virtual void onDebugContinueRequested(const DebugContinueNotice&) noexcept = 0;
    virtual void onDebugStopFailed(const DebugStopFailure&) noexcept = 0;
    virtual void onDebugSessionEnded(const DebugSessionEnd&) noexcept = 0;

    // A step released the domain: all its threads run until the next stop.
    // The hold generation is STILL held -- this is not a continue.  A host
    // may choose to release external activity for the duration of the step
    // (e.g. after a short debounce), re-asserting on onDebugHoldReasserted.
    virtual void onDebugStepResumed(const DebugStepNotice&) noexcept {}

    // A stop began forming while the current hold generation was already
    // held: a step landed, or a breakpoint / pause / host hold / exception
    // preempted it, or a generation is still held from an earlier execution
    // (see StopCoordinator::holdDropped).  Same payload and timing as
    // onDebugHoldRequested -- the current, unchanged generation; before
    // quiescence.  A host that released anything during a step re-asserts
    // its hold here.
    virtual void onDebugHoldReasserted(const DebugHoldRequest&) noexcept {}
};

} // namespace roxal
