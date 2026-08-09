#pragma once

#ifdef __EMSCRIPTEN__

// Roxal <-> JavaScript bridge for the WebAssembly build.
//
// The VM runs on a Worker (-sPROXY_TO_PTHREAD), so it cannot touch the DOM: a
// Worker's `emscripten::val` sees the Worker's own global scope, not the page.
// Every JS operation is therefore proxied to the browser main thread, which owns
// a handle table of live JS values. Roxal holds int32 handles into that table.
//
// One generic (handle, op, name, args) protocol covers the whole JS surface, so
// new capabilities cost no C++ per API. See ../../web-integration-plan.md.
//
// Two rules, both load-bearing:
//
//  1. Roxal -> JS may block the VM thread. JS -> Roxal must NEVER block the main
//     thread (it cannot Atomics.wait). Callbacks therefore queue; they do not
//     wait for a result.
//  2. Property writes and releases are queued and flushed lazily, because each
//     synchronous round trip costs a main-thread event-loop turn. Anything that
//     returns a value flushes the queue first, so ordering is never violated.
//
// The wire format below is duplicated in wasm/roxal-bridge.js. Keep them in step;
// the encoding is deliberately small enough to read side by side.

#include "Value.h"
#include "core/ustring.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace roxal {
namespace web {

// ---------------------------------------------------------------- wire format

// Value tags. Payloads are little-endian and unaligned; the JS half reads them
// through a DataView, which does not care about alignment.
enum class Tag : uint8_t {
    Nil     = 0,
    False   = 1,
    True    = 2,
    Int     = 3,   // i32
    Real    = 4,   // f64
    Str     = 5,   // u32 byteLen + UTF-8 bytes
    Handle  = 6,   // u32 handle into the main-thread table
    List    = 7,   // u32 count + count values
    Dict    = 8,   // u32 count + count (Str key, value) pairs
    Func    = 9,   // u32 callbackId -- a Roxal callable, becomes a JS function
    Bytes   = 10,  // u32 byteLen + raw bytes (tensors -> Uint8Array, no copy)
    Error   = 11,  // u32 byteLen + UTF-8 message; responses only
    // u32 receiverHandle + Str name; responses only. Roxal compiles `x.foo(a)`
    // to GetProp(x,"foo") followed by a call, so reading a JS *function*
    // property must yield something callable rather than an opaque handle. The
    // receiver handle is a fresh one owned by the returned callable, so the
    // binding keeps its receiver alive independently of the original value.
    Method  = 12,
};

// Operations. Those marked (deferred) have no result and are queued rather than
// round-tripped; everything else flushes the queue and blocks for a reply.
enum class Op : uint8_t {
    Global   = 0,   // [Str name]                        -> value   (window, document, ...)
    Get      = 1,   // [u32 h][Str name]                 -> value
    Set      = 2,   // [u32 h][Str name][value]          -> (deferred)
    Call     = 3,   // [u32 h][Str name][u32 argc][args] -> value
    Index    = 4,   // [u32 h][value index]              -> value
    SetIndex = 5,   // [u32 h][value index][value]       -> (deferred)
    New      = 6,   // [u32 h][u32 argc][args]           -> value
    Release  = 7,   // [u32 h]                           -> (deferred)
    Listen   = 8,   // [u32 h][Str event][u32 cbId]      -> value (listener handle)
    Unlisten = 9,   // [u32 h]                           -> (deferred)
    TypeOf   = 10,  // [u32 h]                           -> Str

    // --- state bridge (layer 2) ---
    // Register a store and its initial snapshot. Methods are named up front so the
    // JS side can present them without a round trip per call site.
    StoreDefine  = 11,  // [Str name][Dict snapshot][List methodNames] -> nil
    // Coalesced property delta. Deferred: a UI update is never worth a round trip,
    // and the JS side batches notifications into a microtask anyway.
    StorePatch   = 12,  // [Str name][Dict delta]                      -> (deferred)
    // Settle a JS-side promise for a method call or a rejected write.
    StoreResolve = 13,  // [u32 callId][value result][Str error]       -> (deferred)

    // --- NN provider (ai.nn) ---
    // Ask the host's NN provider (Module.roxalNN) to create/run/close an
    // inference session. Deferred: the reply arrives as Inbound::NnResult
    // carrying the same callId. kind: 0=create [Bytes model][Str device],
    // 1=run [u32 session][u32 n]{[Str name][Str dtype][List shape][Bytes data]},
    // 2=close [u32 session] (no reply).
    NnRequest    = 14,  // [u32 callId][u8 kind][...]                  -> (deferred)

    // --- Unicode case mapping ---
    // Borrow the host's Unicode tables for upper/lower/title case (see
    // web/UnicodeHost.cpp). Synchronous by nature: it is the return value of
    // a string operation, so there is nothing useful to do until it lands.
    // mode: 0=lower 1=upper 2=title.
    UnicodeCase  = 15,  // [u8 mode][Str text]                         -> Str
};

// Inbound work posted by the main thread, drained on the VM thread.
enum class Inbound : uint8_t {
    Callback   = 0,   // a dom.on() listener fired
    StoreCall  = 1,   // JS invoked an exposed method
    StoreWrite = 2,   // JS wrote an exposed property
    NnResult   = 3,   // the NN provider settled an Op::NnRequest
};

// Handle 0 is always JS null/undefined, so a zero handle needs no special case.
constexpr uint32_t kNullHandle = 0;

// ------------------------------------------------------------------- encoding

// Append-only buffer for building a request. Kept as a plain byte vector rather
// than a fixed scratch block so a large argument (a tensor, a long string)
// cannot silently truncate.
class Encoder {
public:
    void op(Op o)             { raw(static_cast<uint8_t>(o)); }
    void u8(uint8_t v)        { raw(v); }
    void u32(uint32_t v);
    // Tag::Bytes + length + raw bytes. The JS half reads it as a Uint8Array
    // view straight over the wasm heap -- no copy on the way out.
    void bytesBlob(const void* data, size_t len);
    void tag(Tag t)           { raw(static_cast<uint8_t>(t)); }
    void str(const std::string& utf8);
    void str(const ustring& s);

    // Encode a Roxal Value. Reference types that have no JS counterpart raise a
    // Roxal exception rather than silently encoding as nil -- a UI that shows
    // "undefined" because a value quietly vanished is far worse to debug.
    void value(const Value& v);

    const std::vector<uint8_t>& bytes() const { return buf_; }
    void clear()                              { buf_.clear(); }
    bool empty() const                        { return buf_.empty(); }
    size_t size() const                       { return buf_.size(); }

private:
    void raw(uint8_t b) { buf_.push_back(b); }
    std::vector<uint8_t> buf_;
};

// Reads a response written by the JS half.
class Decoder {
public:
    Decoder(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    // Decode one value. A Tag::Error payload is thrown as a Roxal exception, so
    // a JS-side failure surfaces as a catchable Roxal error at the call site
    // rather than as a nil that propagates somewhere unrelated.
    Value value();

private:
    uint8_t  u8();
    uint32_t u32();
    double   f64();
    std::string str();

    const uint8_t* p_;
    const uint8_t* end_;
};

// ---------------------------------------------------------------------- calls

// Perform an operation and return its result. Flushes any queued deferred ops
// first. Blocks the calling (VM) thread until the main thread replies.
Value exec(const Encoder& request);

// Queue a deferred operation (Set / SetIndex / Release / Unlisten). Returns
// immediately; the batch is flushed by the next exec(), by flush(), or when it
// grows past an internal threshold.
void defer(const Encoder& request);

// Push any queued deferred ops to the main thread. Call at the end of a Roxal
// turn so the page reflects the script's writes without waiting for a read.
void flush();

// True when called on a thread that may issue bridge operations at all. The
// browser main thread must not: it would be asking itself to run the op and
// would deadlock.
bool canIssueOps();

// Wrap a main-thread handle as a Roxal value (nil for kNullHandle).
// Implemented in ObjJsValue.cpp.
Value jsValue(uint32_t handle);

// Enqueue work posted by the browser main thread. Never blocks.
//   Callback   — `id` is the callback id, `name`/`member` unused
//   StoreCall  — `name` is the store, `member` the method, `id` the promise id
//   StoreWrite — `name` is the store, `member` the property, `id` unused
void queueInboundFromMainThread(Inbound kind, uint32_t id,
                                const char* name, const char* member,
                                const uint8_t* args, uint32_t len);

// Installed by the web module so the bridge can route StoreCall / StoreWrite
// without JsBridge depending on the store implementation.
using StoreCallHandler  = std::function<void(const std::string& store,
                                             const std::string& method,
                                             const std::vector<Value>& args,
                                             uint32_t callId)>;
using StoreWriteHandler = std::function<void(const std::string& store,
                                             const std::string& prop,
                                             const Value& value)>;
void setStoreHandlers(StoreCallHandler onCall, StoreWriteHandler onWrite);

// Installed by the NN bridge (compiler/web/NnBridge.cpp) so drainInbound can
// route Inbound::NnResult without JsBridge depending on ai.nn. The shutdown
// hook settles outstanding requests when the bridge tears down.
using NnResultHandler   = std::function<void(uint32_t callId, const std::vector<Value>& args)>;
using NnShutdownHandler = std::function<void()>;
void setNnHandlers(NnResultHandler onResult, NnShutdownHandler onShutdown);

// Register a Roxal callable so JS can invoke it, returning the callback id
// embedded in Tag::Func. The callable is held as a GC root until
// releaseCallback(). Invocations arrive on the VM thread via the inbound queue.
uint32_t registerCallback(const Value& callable);
void releaseCallback(uint32_t id);

// Drain work queued by the main thread, running each on the VM thread. Called
// from the dispatch loop's host-event-loop hook.
void drainInbound();

// Drain ONLY inbound items of `kind`, leaving everything else queued, and
// return how many were processed. For native code that must block for a
// specific reply on the very thread that drains the queue (Model.init waiting
// for the provider): processing just the awaited kind cannot re-enter the VM,
// while a full drain could dispatch a store call mid-builtin.
size_t drainInboundOnly(Inbound kind);

// Release every handle and callback; used at module teardown.
void shutdown();

} // namespace web
} // namespace roxal

#endif // __EMSCRIPTEN__
