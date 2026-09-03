#pragma once

// DebugHandleTable: session-local opaque integer handles for every
// debugger reference -- thread, frame, scope, variable.
//
//  - Handles are SMALL ints (DAP numbers; JS clients lose precision above
//    2^53, so 64-bit ids like Thread::id() are never packed into one).
//  - Ids are monotonically assigned and NEVER reused within a session, so a
//    stale id from a previous stop can only miss -- it can never alias a
//    fresh entry.  Every entry is additionally stamped with the stop epoch
//    it was created under and validated against the CURRENT epoch on
//    lookup: old generations cannot reach new frames or Values.
//  - Variable handles retain their Value STRONGLY: the table is a typed
//    persistent GC root (TracedMember with a custom tracer, the
//    ServerActorRegistry pattern), so a Value reachable only through a
//    debugger handle survives collections under both the conservative
//    native GC and the precise wasm GC.  Strong, not weak -- a weak entry
//    would vanish exactly when the user expands it.
//  - clearForResume() runs on EVERY continue/step/rollback before any
//    debuggee code executes; the whole table empties and later lookups of
//    the old ids fail deterministically.
//  - Bounded: at most maxHandles entries per stop (0 is returned -- and is
//    never a valid id -- once the cap is hit).
//
// Construction registers the GC root; construct before any RT GC-yield
// section (root registration is forbidden from one).

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "../GCRoots.h"
#include "../Value.h"

namespace roxal {

enum class DebugHandleKind : uint8_t {
    Thread = 0,
    Frame  = 1,
    Scope  = 2,
    Variable = 3,
};

enum class DebugScopeKind : uint8_t {
    Locals    = 0,
    Upvalues  = 1,
    ModuleVars = 2,
};

struct DebugHandle {
    DebugHandleKind kind { DebugHandleKind::Thread };
    uint64_t epoch { 0 };          // stop epoch stamped at creation
    uint64_t threadId { 0 };       // Thread::id() (Thread/Frame/Scope)
    uint64_t activationId { 0 };   // frame identity across the frames vector
    uint32_t frameIndex { 0 };     // index into Thread::frames at creation
    DebugScopeKind scope { DebugScopeKind::Locals };
    Value value { Value::nilVal() };   // strong root (Variable handles)
};

class DebugHandleTable {
public:
    DebugHandleTable();

    // Returns the new handle id, or 0 (never a valid id) when the per-stop
    // cap is reached or the id space is exhausted.
    int32_t create(const DebugHandle& h);

    // Valid only when the entry exists AND was created under `currentEpoch`.
    std::optional<DebugHandle> lookup(int32_t id, uint64_t currentEpoch);

    // Invalidate everything (continue/step/rollback/session end).  Old ids
    // are never reassigned, so post-clear lookups fail deterministically.
    void clearForResume();

    size_t size();

    // Test hook (also the DAP host-configurable limit, downward only).
    void setMaxHandles(size_t m) { maxHandles_ = m; }
    size_t maxHandles() const { return maxHandles_; }

private:
    static void traceEntries(ValueVisitor& visitor,
                             const std::unordered_map<int32_t, DebugHandle>& m);

    std::mutex mutex_;
    TracedMember<std::unordered_map<int32_t, DebugHandle>> entries_ { &traceEntries };
    int32_t nextId_ { 1 };
    size_t maxHandles_ { 4096 };
};

} // namespace roxal
