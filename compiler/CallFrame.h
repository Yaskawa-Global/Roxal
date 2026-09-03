#pragma once

// CallFrame lives in its own header because both VM.h and Thread.h need the
// COMPLETE type, and VM.h includes Thread.h (so Thread.h cannot include VM.h).
// Thread declares a CallFrames member and reserve()s it in its constructor;
// instantiating std::vector<CallFrame> that way requires a complete CallFrame.
// libstdc++ happens to tolerate the incomplete-type forward declaration that
// used to live in Thread.h, but libc++ (and the standard) do not.

#include <vector>
#include <cstdint>

#include "Chunk.h"
#include "Value.h"

namespace roxal {

struct CallFrame;

typedef std::vector<CallFrame> CallFrames;

struct CallFrame {
    #ifdef DEBUG_BUILD
    CallFrame() : closure(Value::nilVal()), slots(nullptr), strict(false), callerStrict(false), isEventHandler(false), isContinuationCallback(false) {}
    #else
    CallFrame() : closure(Value::nilVal()), strict(false), callerStrict(false), isEventHandler(false), isContinuationCallback(false) {}
    #endif
    Value closure; // ObjClosure
    Chunk::iterator startIp;
    Chunk::iterator ip;
    Value* slots;

    CallFrames::iterator parent;

    bool strict; // whether current frame executes in strict mode
    bool callerStrict; // caller's lexical strict setting (for parameter conversion context)

    // on frame start, move argument Value (second) to end of the frame's
    //  argument list (in existing stack arg placeholder slots)
    std::vector<Value> tailArgValues;

    // if not empty, used to reorder call arguments on the stack
    std::vector<int8_t> reorderArgs; // reordering

    struct ExceptionHandler {
        Chunk::iterator handlerIp;
        size_t stackDepth;
        size_t frameDepth;
    };
    std::vector<ExceptionHandler> exceptionHandlers;

    bool isEventHandler { false }; // true for event handler frames (pushed by processEventDispatch)
    bool isContinuationCallback { false }; // true for native continuation callback frames (e.g., filter/map/reduce, native default params)

    // opReturn() unwinds a returning frame's slots (callee slot 0, arguments,
    // locals) only when a caller frame remains beneath it -- outermost frames
    // keep their slots because actor message handlers depend on that.  But the
    // re-entrant entry points (invokeClosure/invokeMethod) push their frame at
    // an otherwise-empty frame stack and OWN those slots: without cleanup each
    // completed call leaks its frame (the dataflow engine evaluates every
    // script node this way -- the leak killed the engine thread at the
    // 16384-slot stack limit).  The flag makes opReturn unwind such frames on
    // return, which also covers calls that complete later in a slice after a
    // deadline/future yield -- a path the entry point's own epilogue never
    // sees.
    bool unwindOnReturn { false }; // set by invokeClosure/invokeMethod on the frame they push

    // ---- Debugger ----
    // Monotonic per-thread activation identity: stable across frame-vector
    // relocation and distinguishes a recycled depth from the same live
    // activation.  Assigned LAZILY on first debugger observation (zero cost
    // when no debugger work is armed); 0 = not yet assigned.
    uint64_t activationId { 0 };
    // Statement-boundary window cache: the current statement's [start, next)
    // offsets.  Hot boundary test = two integer compares; a miss (or a
    // control-flow jump) re-locates with one binary search, and the rare
    // FIRE re-locates once more for the statement index.  Deliberately
    // small (the frame's chunk never changes, so no chunk pointer; the
    // index is recomputed on fire): CallFrame sits on the hottest push/pop
    // path and its size is benchmark-gated.
    uint32_t dbgStmtStart { 0xffffffffu };
    uint32_t dbgStmtNext { 0 };
};

} // namespace roxal
