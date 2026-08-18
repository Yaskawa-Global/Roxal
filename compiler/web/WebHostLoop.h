#pragma once
#include <atomic>
#include <cstdint>

#ifdef __EMSCRIPTEN__

#include "Value.h"

namespace roxal {

class VM;

namespace web {

// The single host-event-loop integration shared by the `dom` and `web` modules.
//
// Shared on purpose: VM::setHostEventLoop() holds exactly one loop, so two modules
// each installing their own would silently disable whichever went first. Importing
// both dom and web must not break either.
//
// The loop is what makes the browser -> Roxal direction work at all. It runs from
// the VM's dispatch loop, which is the only context with a live Roxal thread to
// invoke handlers on.
void installHostLoop(VM& vm);      // idempotent
void uninstallHostLoop(VM& vm);

// Park the calling Roxal thread until stopRequested() or `seconds` elapses.
// Does NOT block in C++: it sets threadSleep and returns, leaving the dispatch
// loop free to service events and run handlers. Blocking here instead would hold
// the VM inside a native frame, and a handler invoked from there re-enters the
// dispatch loop in a state it does not expect (a wasm trap, in practice).
void parkCurrentThread(bool timed, double seconds);

// Ask a parked thread to resume. Only records the request: the un-park happens in
// the host loop after the current handler returns, because the scope that runs a
// nested handler re-parks on return.
void requestStop();

// diagnostics: pump turns since boot
extern std::atomic<std::uint64_t> g_pumpCount;
// Inbound ops queued by the browser main thread vs drained by the VM thread.
// A persistent gap means the JS->VM direction is stalled (the program may be
// running fine), which is otherwise indistinguishable from a dead script.
extern std::atomic<std::uint64_t> g_inboundQueued;
extern std::atomic<std::uint64_t> g_inboundDrained;

} // namespace web
} // namespace roxal

#endif // __EMSCRIPTEN__
