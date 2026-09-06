#ifdef ROXAL_ENABLE_WEB

#include "JsBridge.h"
#ifdef __EMSCRIPTEN__
#include "ObjJsValue.h"
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#endif

#include "ArgsView.h"
#include "Object.h"
#include "VM.h"
#include "dataflow/Signal.h"   // signal properties cross as their current value

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace roxal {
namespace web {

namespace {

// Deferred writes are flushed once the batch reaches this size, so a script that
// only ever writes (a render loop touching many nodes) still makes progress
// instead of growing an unbounded buffer.
constexpr size_t kFlushThreshold = 64 * 1024;

// Per-thread, because in principle any Roxal thread may touch JS. Only one of
// them is the script thread today, but an actor could hold a handle.
thread_local Encoder t_deferred;

// ---------------------------------------------------------------- callbacks

struct CallbackEntry {
    Value callable;      // GC root while registered; see traceCallbacks()
};

std::mutex g_cbMutex;
std::unordered_map<uint32_t, CallbackEntry> g_callbacks;
uint32_t g_nextCallbackId = 1;

// Work posted by the main thread, drained on the VM thread.
struct PendingInbound {
    Inbound kind;
    uint32_t id;                 // callback id, or promise id for StoreCall
    std::string store;
    std::string member;
    std::vector<uint8_t> args;   // encoded argument list / value
};
std::mutex g_pendingMutex;
std::condition_variable g_pendingCv;
std::deque<PendingInbound> g_pending;

Transport* g_transport = nullptr;

StoreCallHandler  g_onStoreCall;
StoreWriteHandler g_onStoreWrite;
StoreIsActorFn    g_storeIsActor;
// Ops held back while a debugger stop is active (VM thread only -- the
// drain is single-threaded); re-spliced to the FRONT of the pending queue
// on the first drain after release, preserving order.
std::deque<PendingInbound> g_deferredInbound;
NnResultHandler   g_onNnResult;
NnShutdownHandler g_onNnShutdown;

} // namespace

// The main thread's only entry point into the VM's address space. Deliberately
// tiny: it touches a mutex-protected queue and nothing else, so it can neither
// corrupt VM state nor block the main thread.
std::atomic<std::uint64_t> g_inboundQueued{0};
std::atomic<std::uint64_t> g_inboundDrained{0};

void queueInboundFromMainThread(Inbound kind, uint32_t id,
                                const char* name, const char* member,
                                const uint8_t* args, uint32_t len)
{
    g_inboundQueued.fetch_add(1, std::memory_order_relaxed);
    PendingInbound work;
    work.kind   = kind;
    work.id     = id;
    work.store  = name   ? name   : "";
    work.member = member ? member : "";
    work.args.assign(args, args + len);
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pending.push_back(std::move(work));
    }
    g_pendingCv.notify_all();
}

void waitForInbound(int64_t maxWaitUs)
{
    if (maxWaitUs <= 0) return;
    std::unique_lock<std::mutex> lock(g_pendingMutex);
    if (!g_pending.empty()) return;
    g_pendingCv.wait_for(lock, std::chrono::microseconds(maxWaitUs),
                         [] { return !g_pending.empty(); });
}

void setTransport(Transport* transport) { g_transport = transport; }

void setStoreActorPredicate(StoreIsActorFn isActor)
{
    g_storeIsActor = std::move(isActor);
}

void setStoreHandlers(StoreCallHandler onCall, StoreWriteHandler onWrite)
{
    g_onStoreCall  = std::move(onCall);
    g_onStoreWrite = std::move(onWrite);
}

void setNnHandlers(NnResultHandler onResult, NnShutdownHandler onShutdown)
{
    g_onNnResult   = std::move(onResult);
    g_onNnShutdown = std::move(onShutdown);
}

// ------------------------------------------------------------------ Encoder

void Encoder::u32(uint32_t v)
{
    buf_.push_back(static_cast<uint8_t>(v & 0xff));
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void Encoder::bytesBlob(const void* data, size_t len)
{
    raw(static_cast<uint8_t>(Tag::Bytes));
    u32(static_cast<uint32_t>(len));
    const uint8_t* b = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), b, b + len);
}

void Encoder::str(const std::string& utf8)
{
    tag(Tag::Str);
    u32(static_cast<uint32_t>(utf8.size()));
    buf_.insert(buf_.end(), utf8.begin(), utf8.end());
}

void Encoder::str(const ustring& s)
{
    str(toUTF8StdString(s));
}

void Encoder::value(const Value& v)
{
    if (v.isNil())  { tag(Tag::Nil);  return; }
    if (v.isBool()) { tag(v.asBool() ? Tag::True : Tag::False); return; }
    if (v.isInt()) {
        const int64_t i = v.asInt();
        // JS numbers are doubles; anything past 2^53 would arrive wrong. Encode
        // out-of-range integers as Real so the loss is the documented float one
        // rather than a silent truncation to 32 bits.
        if (i >= INT32_MIN && i <= INT32_MAX) {
            tag(Tag::Int);
            u32(static_cast<uint32_t>(static_cast<int32_t>(i)));
        } else {
            tag(Tag::Real);
            const double d = static_cast<double>(i);
            uint8_t tmp[8];
            std::memcpy(tmp, &d, 8);
            buf_.insert(buf_.end(), tmp, tmp + 8);
        }
        return;
    }
    if (v.isReal()) {
        tag(Tag::Real);
        const double d = v.asReal();
        uint8_t tmp[8];
        std::memcpy(tmp, &d, 8);
        buf_.insert(buf_.end(), tmp, tmp + 8);
        return;
    }
    if (isString(v)) { str(asStringObj(v)->toStdString()); return; }

    if (isList(v)) {
        tag(Tag::List);
        const std::vector<Value> elts = asList(v)->getElements();
        u32(static_cast<uint32_t>(elts.size()));
        for (const Value& e : elts) value(e);
        return;
    }
    if (isDict(v)) {
        tag(Tag::Dict);
        const std::vector<std::pair<Value, Value>> items = asDict(v)->items();
        u32(static_cast<uint32_t>(items.size()));
        for (const auto& kv : items) {
            // JS object keys are strings; a non-string key would be stringified
            // by JS anyway, so make that explicit rather than surprising.
            if (isString(kv.first)) str(asStringObj(kv.first)->toStdString());
            else                    str(std::string("<non-string key>"));
            value(kv.second);
        }
        return;
    }

    // A signal crosses as its CURRENT VALUE, not as an opaque handle: a view binds
    // to what the signal reads right now.
    if (isSignal(v)) { value(asSignal(v)->signal->lastValue()); return; }

    // A tensor crosses by value: dtype, shape, then its bytes. rawData() brings
    // an ORT-backed (possibly GPU-resident) tensor to host memory first.
    if (isTensor(v)) {
        const ObjTensor* t = asTensor(v);
        tag(Tag::Tensor);
        str(to_string(t->dtype()));
        const auto& shape = t->shape();
        u32(static_cast<uint32_t>(shape.size()));
        for (int64_t d : shape) u32(static_cast<uint32_t>(d));
        const size_t len = static_cast<size_t>(t->numel()) * tensorDTypeSize(t->dtype());
        u32(static_cast<uint32_t>(len));
        const uint8_t* b = static_cast<const uint8_t*>(t->rawData());
        buf_.insert(buf_.end(), b, b + len);
        return;
    }

    // A callable becomes a JS function that queues an invocation back to the VM.
    if (isClosure(v)) {
        tag(Tag::Func);
        u32(registerCallback(v));
        return;
    }

    throw std::runtime_error(
        "web: cannot pass this value to JavaScript (unsupported type); "
        "pass a number, string, bool, nil, list, dict, tensor, function, or a JS handle");
}

// ------------------------------------------------------------------ Decoder

uint8_t Decoder::u8()
{
    if (p_ >= end_) throw std::runtime_error("dom: truncated response from JavaScript");
    return *p_++;
}

uint32_t Decoder::u32()
{
    const uint32_t a = u8(), b = u8(), c = u8(), d = u8();
    return a | (b << 8) | (c << 16) | (d << 24);
}

double Decoder::f64()
{
    if (p_ + 8 > end_) throw std::runtime_error("dom: truncated response from JavaScript");
    double d;
    std::memcpy(&d, p_, 8);
    p_ += 8;
    return d;
}

std::string Decoder::str()
{
    const uint32_t len = u32();
    if (p_ + len > end_) throw std::runtime_error("dom: truncated string from JavaScript");
    std::string s(reinterpret_cast<const char*>(p_), len);
    p_ += len;
    return s;
}

Value Decoder::value()
{
    switch (static_cast<Tag>(u8())) {
        case Tag::Nil:    return Value::nilVal();
        case Tag::False:  return Value::boolVal(false);
        case Tag::True:   return Value::boolVal(true);
        case Tag::Int:    return Value::intVal(static_cast<int32_t>(u32()));
        case Tag::Real:   return Value::realVal(f64());
        case Tag::Str:    return Value::stringVal(ustring::fromUTF8(str()));
        case Tag::Handle:
#ifdef __EMSCRIPTEN__
            return jsValue(u32());
#else
            throw std::runtime_error("web: JavaScript handles do not exist on this host");
#endif
        case Tag::List: {
            const uint32_t n = u32();
            std::vector<Value> elts;
            elts.reserve(n);
            for (uint32_t i = 0; i < n; ++i) elts.push_back(value());
            return Value::listVal(elts);
        }
        case Tag::Dict: {
            const uint32_t n = u32();
            std::vector<std::pair<Value, Value>> entries;
            entries.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                if (static_cast<Tag>(u8()) != Tag::Str)
                    throw std::runtime_error("dom: malformed dict key from JavaScript");
                Value k = Value::stringVal(ustring::fromUTF8(str()));
                entries.emplace_back(k, value());
            }
            return Value::dictVal(entries);
        }
        case Tag::Method: {
#ifndef __EMSCRIPTEN__
            throw std::runtime_error("web: JavaScript methods do not exist on this host");
#else
            // Bind receiver+name into a Roxal callable. `recv` is a Value, so the
            // receiver handle is released by the GC when the binding dies -- the
            // binding cannot outlive its handle, nor leak it.
            const Value recv = jsValue(u32());
            // The name is written as a tagged Str (that is what Encoder::str
            // emits everywhere else), so consume the tag before the payload.
            if (static_cast<Tag>(u8()) != Tag::Str)
                throw std::runtime_error("dom: malformed method name from JavaScript");
            const std::string name = str();
            // A BOUND native, not a bare one: ObjBoundNative::trace visits its
            // receiver, so the ObjJsValue stays reachable for as long as the
            // binding does.  A plain nativeVal() with `recv` in the lambda
            // capture put it in the std::function's storage instead, which
            // ObjNative::trace cannot see (it visits only defaultValues) and no
            // scanner reaches -- the mark missed it, the sweep freed it, and the
            // handle was released back to JS while the binding still used it.
            return Value::boundNativeVal(recv, [recv, name](VM&, ArgsView args) -> Value {
                // Bound natives are dispatched with the receiver prepended as
                // args[0]; other call paths pass only the real arguments. Skip
                // it only when it IS the receiver, so both shapes encode the
                // same JS call.
                size_t first = 0;
                if (args.size() > 0 && args[0].isObj() && recv.isObj()
                    && args[0].asObj() == recv.asObj())
                    first = 1;
                Encoder e;
                e.op(Op::Call);
                e.u32(recv.isNil() ? kNullHandle : asJsValue(recv)->handle);
                e.str(name);
                e.u32(static_cast<uint32_t>(args.size() - first));
                for (size_t i = first; i < args.size(); ++i) e.value(args[i]);
                return exec(e);
            });
#endif
        }
        case Tag::Tensor: {
            if (static_cast<Tag>(u8()) != Tag::Str)
                throw std::runtime_error("web: malformed tensor dtype");
            const TensorDType dtype = tensorDTypeFromString(str());
            const uint32_t ndim = u32();
            std::vector<int64_t> shape;
            shape.reserve(ndim);
            for (uint32_t i = 0; i < ndim; ++i) shape.push_back(static_cast<int64_t>(u32()));
            const uint32_t len = u32();
            if (p_ + len > end_)
                throw std::runtime_error("web: truncated tensor payload");
            int64_t numel = 1;
            for (int64_t d : shape) numel *= d;
            if (static_cast<size_t>(numel) * tensorDTypeSize(dtype) != len)
                throw std::runtime_error("web: tensor payload does not match its shape and dtype");
            std::vector<uint8_t> bytes(p_, p_ + len);
            p_ += len;
            return Value::objVal(newTensorObj(shape, dtype, std::move(bytes)));
        }
        case Tag::Bytes: {
            // Raw bytes become a PACKED byte list -- the compact list
            // representation shared with fileio binary reads and serialize().
            const uint32_t len = u32();
            if (p_ + len > end_)
                throw std::runtime_error("dom: truncated bytes payload");
            std::vector<uint8_t> bytes(p_, p_ + len);
            p_ += len;
            Value listVal = Value::listVal();
            asList(listVal)->adoptPackedBytes(std::move(bytes));
            return listVal;
        }
        case Tag::Error:
            // Surfaces at the Roxal call site as a catchable error, rather than
            // as a nil that goes wrong somewhere else entirely.
            throw std::runtime_error("dom: " + str());
        default:
            throw std::runtime_error("dom: unknown value tag from JavaScript");
    }
}

// --------------------------------------------------------------------- calls

bool opNeedsReply(Op op)
{
    switch (op) {
        case Op::Global: case Op::Get: case Op::Call: case Op::Index:
        case Op::New: case Op::Listen: case Op::TypeOf: case Op::UnicodeCase:
            return true;
        default:
            return false;
    }
}

#ifdef __EMSCRIPTEN__
namespace {

// The wasm transport: hand a batch to the browser main thread through the
// proxying queue and block this (Worker) thread for its malloc'd response.
class EmscriptenTransport final : public Transport {
public:
    void send(const std::vector<uint8_t>& batch) override
    {
        uint8_t* response = proxyExec(batch.data(), batch.size());
        if (response) std::free(response);
    }

    std::vector<uint8_t> roundTrip(const std::vector<uint8_t>& batch) override
    {
        uint8_t* response = proxyExec(batch.data(), batch.size());
        if (!response) return {};
        // Response layout: [u32 byteLen][payload].
        uint32_t len;
        std::memcpy(&len, response, 4);
        std::vector<uint8_t> out(response + 4, response + 4 + len);
        std::free(response);
        return out;
    }

    bool canIssueOps() override
    {
        // The main thread must not: exec() proxies TO the main thread and blocks
        // for the reply, so issuing from there is a self-deadlock.
        return !emscripten_is_main_browser_thread();
    }

private:
    static uint8_t* proxyExec(const uint8_t* data, size_t len)
    {
        return reinterpret_cast<uint8_t*>(MAIN_THREAD_EM_ASM_INT({
            return Module.roxalBridge.exec($0, $1);
        }, reinterpret_cast<int>(data), static_cast<int>(len)));
    }
};

} // namespace
#endif

Transport* transport()
{
#ifdef __EMSCRIPTEN__
    // The wasm build has exactly one possible transport; install it on first
    // use so the host needs no setup call.
    static EmscriptenTransport s_emscripten;
    if (!g_transport) g_transport = &s_emscripten;
#endif
    return g_transport;
}

bool canIssueOps()
{
    Transport* t = transport();
    return t != nullptr && t->canIssueOps();
}

namespace {

void requireIssuable(const char* what)
{
    if (!canIssueOps())
        throw std::runtime_error(
            std::string("web: ") + what + " attempted with no web host attached "
            "(a native VM needs `roxal --web-host`; in the browser, DOM work must "
            "run on the VM thread)");
}

} // namespace

Value exec(const Encoder& request)
{
    requireIssuable("a JS operation");

    // Queued writes must land before anything that reads, or a read observes a
    // stale page. Prepending them keeps the ordering the script wrote.
    std::vector<uint8_t> batch;
    if (!t_deferred.empty()) {
        const auto& d = t_deferred.bytes();
        batch.insert(batch.end(), d.begin(), d.end());
        t_deferred.clear();
    }
    const auto& r = request.bytes();
    batch.insert(batch.end(), r.begin(), r.end());

    if (r.empty() || !opNeedsReply(static_cast<Op>(r[0]))) {
        transport()->send(batch);
        return Value::nilVal();
    }
    const std::vector<uint8_t> response = transport()->roundTrip(batch);
    if (response.empty()) return Value::nilVal();
    Decoder dec(response.data(), response.size());
    return dec.value();   // throws on a JS-reported error
}

void defer(const Encoder& request)
{
    requireIssuable("a JS write");
    const auto& r = request.bytes();
    auto& d = const_cast<std::vector<uint8_t>&>(t_deferred.bytes());
    d.insert(d.end(), r.begin(), r.end());
    if (t_deferred.size() >= kFlushThreshold) flush();
}

void flush()
{
    if (t_deferred.empty() || !canIssueOps()) return;
    transport()->send(t_deferred.bytes());
    t_deferred.clear();
}

// ----------------------------------------------------------------- callbacks

// The registry is a C++ heap map, which no scanner reaches: the conservative
// stack scan walks parked STACKS only. Without this hook a registered closure
// is unreachable to the mark the moment the registering builtin returns, and
// the sweep frees it while JS still holds its id.
void traceCallbacks(ValueVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(g_cbMutex);
    for (const auto& e : g_callbacks)
        visitor.visit(e.second.callable);
}

uint32_t registerCallback(const Value& callable)
{
    std::lock_guard<std::mutex> lock(g_cbMutex);
    const uint32_t id = g_nextCallbackId++;
    g_callbacks.emplace(id, CallbackEntry{callable});
    return id;
}

void releaseCallback(uint32_t id)
{
    std::lock_guard<std::mutex> lock(g_cbMutex);
    g_callbacks.erase(id);
}

void drainInbound()
{
    const bool stopActive = VM::instance().debugStopRequested();
    if (!stopActive && !g_deferredInbound.empty()) {
        // Stop released: put the held-back work at the FRONT, in order.
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (auto it = g_deferredInbound.rbegin(); it != g_deferredInbound.rend(); ++it)
            g_pending.push_front(std::move(*it));
        g_deferredInbound.clear();
    }
    for (;;) {
        PendingInbound work;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            if (g_pending.empty()) return;
            work = std::move(g_pending.front());
            g_pending.pop_front();
        }
        if (stopActive
            && !(work.kind == Inbound::StoreCall && g_storeIsActor
                 && g_storeIsActor(work.store))) {
            g_deferredInbound.push_back(std::move(work));
            continue;
        }
        g_inboundDrained.fetch_add(1, std::memory_order_relaxed);

        // Decode the payload once. A malformed payload must not be fatal -- the
        // handler simply sees no arguments -- but it must be SAID, or the
        // symptom is an arity error at the call site with no hint why.
        std::vector<Value> args;
        try {
            Decoder dec(work.args.data(), work.args.size());
            Value arg = dec.value();
            if (isList(arg))        args = asList(arg)->getElements();
            else if (!arg.isNil())  args.push_back(arg);
        } catch (const std::exception& e) {
            VM::emitDiagnostic(
                std::string("web: could not decode inbound arguments: ") + e.what(),
                OutputSeverity::Error, "web.bridge");
        }

        // Containment: this runs from the dispatch loop, which has no call site to
        // propagate to, so an escaping exception would terminate the VM thread.
        try {
            switch (work.kind) {
                case Inbound::Callback: {
                    Value callable;
                    {
                        std::lock_guard<std::mutex> lock(g_cbMutex);
                        auto it = g_callbacks.find(work.id);
                        if (it == g_callbacks.end()) continue;   // released while queued
                        callable = it->second.callable;
                    }
                    if (isClosure(callable))
                        VM::instance().invokeClosure(asClosure(callable), args);
                    break;
                }
                case Inbound::StoreCall:
                    if (g_onStoreCall) g_onStoreCall(work.store, work.member, args, work.id);
                    break;
                case Inbound::NnResult:
                    if (g_onNnResult) g_onNnResult(work.id, args);
                    break;
                case Inbound::StoreWrite:
                    if (g_onStoreWrite)
                        g_onStoreWrite(work.store, work.member,
                                       args.empty() ? Value::nilVal() : args[0]);
                    break;
            }
        } catch (const std::exception& e) {
            VM::emitDiagnostic(
                std::string("web: inbound work failed: ") + e.what(),
                OutputSeverity::Error, "web.bridge");
        } catch (...) {
            VM::emitDiagnostic("web: inbound work failed",
                               OutputSeverity::Error, "web.bridge");
        }
    }
}

size_t drainInboundOnly(Inbound kind)
{
    std::vector<PendingInbound> matched;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (auto it = g_pending.begin(); it != g_pending.end(); ) {
            if (it->kind == kind) {
                matched.push_back(std::move(*it));
                it = g_pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& work : matched) {
        try {
            // Decode exactly as drainInbound does: a Tag::List becomes the
            // args vector, a single non-nil value becomes args[0].
            std::vector<Value> args;
            if (!work.args.empty()) {
                Decoder dec(work.args.data(), work.args.size());
                Value v = dec.value();
                if (isList(v)) {
                    ObjList* l = asList(v);
                    for (int i = 0; i < l->length(); ++i)
                        args.push_back(l->getElement(i));
                } else if (v.isNonNil()) {
                    args.push_back(v);
                }
            }
            switch (work.kind) {
                case Inbound::NnResult:
                    if (g_onNnResult) g_onNnResult(work.id, args);
                    break;
                default:
                    break;   // only side-effect-free kinds are safe here
            }
        } catch (const std::exception& e) {
            VM::emitDiagnostic(
                std::string("web: inbound (filtered) failed: ") + e.what(),
                OutputSeverity::Error, "web.bridge");
        } catch (...) {}
    }
    return matched.size();
}

void shutdown()
{
    // Settle outstanding NN requests FIRST -- their promises must not outlive
    // the bridge unresolved (a hung future would wedge whatever awaits it).
    if (g_onNnShutdown) g_onNnShutdown();
    g_onNnResult = nullptr;
    g_onNnShutdown = nullptr;

    { std::lock_guard<std::mutex> lock(g_pendingMutex); g_pending.clear(); }
    { std::lock_guard<std::mutex> lock(g_cbMutex); g_callbacks.clear(); }
    g_onStoreCall = nullptr;
    g_storeIsActor = nullptr;
    g_deferredInbound.clear();
    g_onStoreWrite = nullptr;
    t_deferred.clear();
}

} // namespace web
} // namespace roxal

#ifdef __EMSCRIPTEN__
// Called from JS on the browser main thread. `kind` selects the flavour of work;
// see roxal::web::Inbound. Deliberately one entry point rather than three, so the
// queueing discipline (never block the main thread) lives in exactly one place.
extern "C" EMSCRIPTEN_KEEPALIVE
void roxal_web_queue_inbound(int kind, uint32_t id, const char* name, const char* member,
                             const uint8_t* args, uint32_t len)
{
    roxal::web::queueInboundFromMainThread(
        static_cast<roxal::web::Inbound>(kind), id, name, member, args, len);
}
#endif

#endif // ROXAL_ENABLE_WEB
