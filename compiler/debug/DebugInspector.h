#pragma once

// DebugInspector: stackTrace / scopes / variables over a COMMITTED stop
// epoch.
//
// Contract: every call is valid only while the coordinator reports a
// committed stop; otherwise it returns empty.  No snapshotting, no
// marshalling -- the stopped threads' frames/slots/upvalues are frozen by
// the stop barrier and their Values stay alive through the epoch's strong
// Thread leases.  "Stopped" freezes the stopped threads' FRAMES, not
// everything reachable from them: every child enumeration below uses an
// existing synchronized accessor or declares the value opaque
// (VariablesMap::snapshot, ObjDict::itemsRange, Signal::lastValue under its
// values mutex, shared_future wait_for(0) poll, declared-property backing
// fields -- never getters, never user code).
//
// All references handed out are session-local opaque ints from the
// coordinator's DebugHandleTable, invalidated on resume before any debuggee
// code runs.
//
// Synchronization contract for container children: the stop barrier
// freezes every VM thread, and the runtime funnels ALL container
// mutation onto VM threads -- cross-thread producers (DDS reader lifts,
// Qt signal hub) queue onto engine/actor threads by design, and signal
// sample stores take the values mutex.  A native writer bypassing that
// would already break the GC (whose mark phase reads these containers
// with the same non-locking accessors, world-stopped); the debugger
// inherits exactly that invariant rather than adding its own locks.
//
// Frame model: stackTrace reports EVERY frame -- event-handler and native-
// continuation frames execute user closures (handlers, map/filter bodies),
// so hiding them would hide user code; a DAP presentationHint can label
// them later.  Stepping is where machinery must not leak: the step
// predicates anchor on the origin frame's activation and refuse to land
// under an event-handler dispatch that interposed above the origin (see
// VM::debugStatementBoundary).
//
// Every entry point takes a GC mutator cover, so any host thread may call
// it; the DAP adapter routes its calls through the debug worker.

#include <cstdint>
#include <string>
#include <vector>

#include "DebugHandleTable.h"
#include "ValueRender.h"

namespace roxal {

class StopCoordinator;
class Thread;

// Named limits, host-configurable downward.
struct DebugLimits {
    size_t maxFramesPerRequest  = 256;
    size_t maxVariablesPerPage  = 200;
    // Compiler-generated locals (__iterable__/__index__ loop temps etc.,
    // per the DebugLocalSynthetic flag) are hidden from the variables view
    // by default; a host can opt them back in.  Flag-based on purpose: a
    // name filter would also hide a user's own dunder-named variable, and
    // an UNflagged synthetic showing up is a compiler emission bug worth
    // seeing.
    bool includeSynthetic       = false;
    RenderOptions render;          // bounded value previews
};

struct DebugThreadDesc {
    int32_t handle { 0 };
    uint64_t threadId { 0 };
    std::string name;
};

struct DebugFrameDesc {
    int32_t handle { 0 };
    std::string name;              // function name ("<module>" for top level)
    std::string source;            // chunk source name
    int line { 0 };
    int column { 0 };
};

struct DebugScopeDesc {
    int32_t variablesHandle { 0 };
    std::string name;              // "Locals" / "Upvalues" / "Module"
};

struct DebugVarDesc {
    std::string name;
    std::string value;             // bounded render
    std::string type;              // describeValueType
    int32_t childHandle { 0 };     // 0 = no children
    bool synthetic { false };      // compiler-generated local (UIs hide)
};

class DebugInspector {
public:
    explicit DebugInspector(StopCoordinator& coord, DebugLimits limits = {});

    // The stopped threads of the current epoch (excluded controller threads
    // are not epoch members and do not appear).
    std::vector<DebugThreadDesc> threads();

    // Frames top-down (index 0 = innermost).  Assigns activation ids lazily.
    std::vector<DebugFrameDesc> stackTrace(uint64_t threadId,
                                           size_t startFrame, size_t levels);

    // Scopes of a stack frame: Locals, Upvalues (when captured), Module.
    std::vector<DebugScopeDesc> scopes(int32_t frameHandle);

    // Children of a scope or variable handle, paged [start, start+count).
    // count == 0 selects the default page size; both are clamped to
    // maxVariablesPerPage.
    std::vector<DebugVarDesc> variables(int32_t variablesHandle,
                                        size_t start, size_t count);

    // Tier 1 path evaluation: hover/watch resolution of `ident`,
    // `ident.prop`, `ident[literal]` chains against the stopped frame --
    // locals (innermost live shadow), upvalues, then module vars -- using
    // ONLY the same synchronized read contracts as the variables view.
    // No compilation, no execution, no getters/natives/actors/
    // conversions: an invalid path or opaque traversal fails cleanly and
    // provably cannot alter the stopped program.
    struct EvalResult {
        bool ok { false };
        std::string error;
        std::string value;
        std::string type;
        int32_t childHandle { 0 };
    };
    EvalResult evaluate(int32_t frameHandle, const std::string& expression);

    DebugLimits& limits() { return limits_; }

private:
    ptr<Thread> findEpochThread(uint64_t threadId);
    bool resolveFrame(const DebugHandle& h, ptr<Thread>& tOut, size_t& frameIndexOut);
    DebugVarDesc makeVar(std::string name, const Value& v, uint64_t epoch,
                         bool synthetic = false);
    std::vector<DebugVarDesc> scopeChildren(const DebugHandle& h,
                                            size_t start, size_t count);
    std::vector<DebugVarDesc> valueChildren(const DebugHandle& h,
                                            size_t start, size_t count);

    StopCoordinator& coord_;
    DebugLimits limits_;
};

} // namespace roxal
