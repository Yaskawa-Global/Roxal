#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "core/memory.h"

namespace roxal {

class StopCoordinator;

// One script/debuggee execution domain.
//
// Carries the domain identity, the interrupt/control word shared by every
// Thread that belongs to the domain, and the stop-coordination state below.
// The VM owns one default domain and all Threads currently join it.  Still
// to come: structured failure state and the debug-session association -- and
// debugger evaluation gets a domain of its own, so an evaluated expression's
// failure or exit() can never poison the debuggee's control state.
class ExecutionDomain {
public:
    // Interrupt/control word bits.  RuntimeError and Exit are live; the debug
    // bits are reserved for the debugger's stop machinery and never set yet.
    enum InterruptBit : uint32_t {
        IntrRuntimeError  = 1u << 0,  // a thread hit a fatal runtime error
        IntrExit          = 1u << 1,  // exit() requested
        IntrDebugStop     = 1u << 2,  // reserved: debugger stop request
        IntrDebugSlowPath = 1u << 3,  // reserved: debugger forces full-loop dispatch
    };

    // A standalone domain owns its interrupt word.
    ExecutionDomain() : word_(&owned_), id_(nextId_.fetch_add(1)) {}
    // The VM's default domain ALIASES the VM's own hot member instead
    // (benchmark-gated: hosting the word behind the domain pointer costs an
    // extra dependent load on the dispatch loop's per-instruction check, a
    // measured ~2-3% on dispatch_micro).  The storage must outlive the
    // domain -- true for VM::interrupts_ vs. VM::defaultDomain_.
    explicit ExecutionDomain(std::atomic<uint32_t>& externalWord)
      : word_(&externalWord), id_(nextId_.fetch_add(1)) {}
    ExecutionDomain(const ExecutionDomain&) = delete;
    ExecutionDomain& operator=(const ExecutionDomain&) = delete;

    uint64_t id() const { return id_; }

    // Consolidated cross-thread interrupt/control word: one atomic carrying
    // the execution-control bits every dispatch iteration must observe.
    // Writers use bit-targeted fetch_or/fetch_and (see the VM accessors) so
    // concurrent setters of OTHER bits are never lost.
    std::atomic<uint32_t>& interrupts() { return *word_; }
    const std::atomic<uint32_t>& interrupts() const { return *word_; }

    // ---- Stop coordination state ----
    // Owned by the domain so a later per-session or debugger-evaluation
    // domain carries its own, independent stop machinery.  Manipulated ONLY
    // by the StopCoordinator and the cooperative stop points; everything a
    // hard-RT path touches is one of the atomics (bounded, lock-free).
    //
    //   stopEpoch          current (or last) epoch id; monotonically bumped
    //   admissionClosed    execute-entry / registration gate: while true, a
    //                      thread may not begin executing user code without
    //                      first joining the epoch
    //   releaseGeneration  bumped by resume/abort; parked threads wait for a
    //                      change, so release is level-triggered (no lost
    //                      wakeups) and sleep state needs no save/restore
    std::atomic<uint64_t> stopEpoch { 0 };
    std::atomic<bool>     admissionClosed { false };
    std::atomic<uint64_t> releaseGeneration { 0 };
    // The coordinator managing this domain's stops (set/cleared by the
    // StopCoordinator itself).  The registration admission gate hands
    // late-registered threads' leases to it.
    std::atomic<StopCoordinator*> coordinator { nullptr };
    std::mutex stopMutex;              // guards the two condvars below
    std::condition_variable stopCv;    // parked debuggee threads wait here
    std::condition_variable ackCv;     // the coordinator's ack wait (notified
                                       // by non-RT ackers; RT ackers only
                                       // store atomics -- the coordinator's
                                       // wait uses a timed poll to cover them)

    // ---- Per-domain execution outcome ----
    // What a run reports about itself.  The RuntimeError and Exit bits say
    // THAT it failed or exited; these say why, and with what code.  Owned by
    // the domain so one run's outcome can never be read by the next one, or
    // by the REPL session, or by the services.
    std::atomic<int> exitCode { 0 };
    std::mutex errorMutex;
    std::string errorMessage;

private:
    std::atomic<uint32_t>* word_;
    std::atomic<uint32_t> owned_ { 0 };
    uint64_t id_;
    static inline std::atomic<uint64_t> nextId_ { 1 };
};

} // namespace roxal
