#ifdef __EMSCRIPTEN__

#include "NnBridge.h"
#include "JsBridge.h"

#include "Object.h"
#include "VM.h"

#include <chrono>
#include <future>
#include <thread>
#include <algorithm>
#include <mutex>
#include <utility>

using namespace roxal;
using namespace roxal::web;

namespace {

// One in-flight request. `futureValue` is the ObjFuture handed to Roxal;
// tracing it covers the RESOLVED result too (ObjFuture::trace visits a ready
// shared state), so entries can be erased immediately after resolution.
// `heldValues` pins the input tensors for the request's lifetime -- the script
// may have dropped its own references the moment predict() returned.
struct PendingNn {
    uint32_t callId = 0;
    uint8_t kind = 0;                       // 0=create 1=run 2=close
    ptr<std::promise<Value>> promise;
    Value futureValue;
    std::vector<Value> heldValues;
};

struct NnState {
    std::mutex mutex;                       // requests come from any Roxal thread
    std::vector<PendingNn> pending;
    uint32_t nextCallId = 1;
    bool installed = false;
};

NnState& state()
{
    static NnState s;
    return s;
}

Value exceptionValue(const std::string& msg)
{
    return Value::exceptionVal(Value::stringVal(toUnicodeString(msg)));
}

// Rebuild a tensor from a provider reply entry:
//   { dtype: str, shape: [ints], data: <packed byte list> }
// The packed bytes are stolen, not copied -- the decoder already owns them.
Value tensorFromReply(const Value& entry)
{
    if (!isDict(entry))
        throw std::runtime_error("nn: malformed output entry from provider");
    ObjDict* d = asDict(entry);

    auto get = [&](const char* key) {
        const Value k = Value::stringVal(toUnicodeString(key));
        if (!d->contains(k))
            throw std::runtime_error(std::string("nn: provider output missing '") + key + "'");
        return d->at(k);
    };

    const Value dtypeVal = get("dtype");
    const Value shapeVal = get("shape");
    const Value dataVal  = get("data");
    if (!isString(dtypeVal) || !isList(shapeVal) || !isList(dataVal))
        throw std::runtime_error("nn: malformed output entry from provider");

    std::vector<int64_t> shape;
    ObjList* shapeList = asList(shapeVal);
    for (int i = 0; i < shapeList->length(); ++i) {
        const Value dim = shapeList->getElement(i);
        shape.push_back(dim.isInt() ? dim.asInt() : static_cast<int64_t>(dim.asReal()));
    }

    const TensorDType dtype = tensorDTypeFromString(toUTF8StdString(asStringObj(dtypeVal)->s));
    std::vector<uint8_t> bytes = asList(dataVal)->stealPackedBytes();
    return Value::objVal(newTensorObj(shape, dtype, std::move(bytes)));
}

// Runs on the VM thread at the drainInbound() safepoint. The payload is the
// provider's reply, generically decoded: [ok:int, body]. body is the error
// message when ok=0; otherwise the create-metadata dict or the run-output
// list. Resolution happens here -- including tensor construction -- so the
// promise is fulfilled with final Values and no consumer ever re-parses.
void onNnResult(uint32_t callId, const std::vector<Value>& args)
{
    PendingNn entry;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        auto& pending = state().pending;
        auto it = std::find_if(pending.begin(), pending.end(),
                               [&](const PendingNn& p) { return p.callId == callId; });
        if (it == pending.end())
            return;                          // already settled (e.g. shutdown)
        entry = std::move(*it);
        pending.erase(it);
    }

    try {
        // The drain split the [ok, body] reply list into the args vector.
        if (args.size() < 2)
            throw std::runtime_error("nn: malformed reply from provider");
        const bool ok = args[0].isInt() ? args[0].asInt() != 0 : args[0].asBool();
        const Value body = args[1];

        if (!ok) {
            const std::string msg = isString(body)
                ? toUTF8StdString(asStringObj(body)->s) : "provider error";
            entry.promise->set_value(exceptionValue(msg));
            return;
        }

        switch (entry.kind) {
            case 0:                          // create -> metadata dict, as-is
                entry.promise->set_value(body);
                break;
            case 1: {                        // run -> tensor | list of tensors
                if (!isList(body))
                    throw std::runtime_error("nn: provider run reply is not a list");
                ObjList* outs = asList(body);
                if (outs->length() == 1) {
                    entry.promise->set_value(tensorFromReply(outs->getElement(0)));
                } else {
                    Value listVal = Value::listVal();
                    for (int i = 0; i < outs->length(); ++i)
                        asList(listVal)->append(tensorFromReply(outs->getElement(i)));
                    entry.promise->set_value(listVal);
                }
                break;
            }
            default:
                entry.promise->set_value(Value::trueVal());
        }
    } catch (const std::exception& e) {
        entry.promise->set_value(exceptionValue(e.what()));
    }
}

void onNnShutdown()
{
    std::vector<PendingNn> orphans;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        orphans.swap(state().pending);
    }
    for (auto& p : orphans)
        p.promise->set_value(exceptionValue("nn: abandoned — bridge shutting down"));
}

void ensureInstalled()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    if (state().installed) return;
    setNnHandlers(onNnResult, onNnShutdown);
    state().installed = true;
}

// Encode, register, send. The request is flushed from THIS thread so a
// predict issued on the dataflow engine thread reaches the main thread
// without waiting for the VM pump.
Value submit(uint8_t kind, const std::function<void(Encoder&)>& payload,
             std::vector<Value> heldValues)
{
    ensureInstalled();

    PendingNn entry;
    entry.kind = kind;
    entry.promise = make_ptr<std::promise<Value>>();
    std::shared_future<Value> fut = entry.promise->get_future().share();
    entry.futureValue = Value::futureVal(fut);
    entry.heldValues = std::move(heldValues);

    Value futureValue = entry.futureValue;

    Encoder e;
    e.op(Op::NnRequest);
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        entry.callId = state().nextCallId++;
        e.u32(entry.callId);
        e.u8(kind);
        payload(e);
        state().pending.push_back(std::move(entry));
    }
    defer(e);
    flush();
    return futureValue;
}

} // namespace

Value roxal::web::nnCreate(std::vector<uint8_t>&& modelBytes, const std::string& device)
{
    // The Encoder copies the bytes into the request buffer; the model blob
    // itself need not outlive this call.
    return submit(0, [&](Encoder& e) {
        e.bytesBlob(modelBytes.data(), modelBytes.size());
        e.str(device);
    }, {});
}

Value roxal::web::nnRun(uint32_t sessionId, const std::vector<NnFeed>& feeds,
                        std::vector<Value> heldValues)
{
    return submit(1, [&](Encoder& e) {
        e.u32(sessionId);
        e.u32(static_cast<uint32_t>(feeds.size()));
        for (const auto& f : feeds) {
            e.str(f.name);
            e.str(f.dtype);
            Value shapeVal = Value::listVal();
            for (int64_t d : f.shape)
                asList(shapeVal)->append(Value::intVal(d));
            e.value(shapeVal);
            e.bytesBlob(f.data, f.len);
        }
    }, std::move(heldValues));
}

void roxal::web::nnClose(uint32_t sessionId)
{
    ensureInstalled();
    Encoder e;
    e.op(Op::NnRequest);
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        e.u32(state().nextCallId++);        // id unused: close has no reply
    }
    e.u8(2);
    e.u32(sessionId);
    defer(e);
    flush();
}

Value roxal::web::nnAwaitBlocking(const Value& future, int timeoutMs)
{
    if (!isFuture(future))
        return future;
    auto& sf = asFuture(future)->future;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (sf.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        flush();                                   // our request must reach JS
        drainInboundOnly(Inbound::NnResult);       // and its reply must reach us
        if (std::chrono::steady_clock::now() > deadline)
            throw std::invalid_argument("ai.nn: provider timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Value v = sf.get();
    if (isException(v)) {
        // Surface as a catchable error at the call site (callNativeFn converts).
        Value msg = asException(v)->message;
        throw std::invalid_argument(isString(msg)
            ? toUTF8StdString(asStringObj(msg)->s) : "ai.nn: provider error");
    }
    return v;
}

void roxal::web::nnTracePending(ValueVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    for (const auto& p : state().pending) {
        visitor.visit(p.futureValue);
        for (const auto& h : p.heldValues)
            visitor.visit(h);
    }
}

#endif // __EMSCRIPTEN__
