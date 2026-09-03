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

enum class DebugHostResult : uint8_t {
    Queued,     // host accepted responsibility for asynchronously performing
                // a controlled motion hold (nominally within ~1s).  Does NOT
                // mean anything is physically stationary.
    Rejected,   // host could not accept (queue full, shutting down, ...) --
                // recorded and surfaced as a debugger warning; a safety-critical
                // host integration treats this as a safety fault.and keeps
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
//  - onDebugHoldRequested() is invoked exactly once per running-to-held
//    hold generation, by a non-RT thread (never by the RT/interpreter
//    callback that discovers a stop condition);
//  - stepping retains the hold generation -- no continue notice between
//    steps; a normal continue produces exactly one onDebugContinueRequested;
//  - terminate/disconnect-while-stopped/fatal/failed-stop never silently
//    release a hold: the corresponding notice is sent and the host applies
//    its own fail-safe policy.
struct DebugHostControl {
    virtual ~DebugHostControl() = default;
    virtual DebugHostResult onDebugHoldRequested(const DebugHoldRequest&) noexcept = 0;
    virtual void onDebugStopped(const DebugStopNotice&) noexcept = 0;
    virtual void onDebugContinueRequested(const DebugContinueNotice&) noexcept = 0;
    virtual void onDebugStopFailed(const DebugStopFailure&) noexcept = 0;
    virtual void onDebugSessionEnded(const DebugSessionEnd&) noexcept = 0;
};

} // namespace roxal
