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
};

} // namespace roxal
