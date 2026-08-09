#pragma once

#ifdef __EMSCRIPTEN__

// The C++ half of the ai.nn provider protocol for WebAssembly builds.
//
// The wasm build has no ONNX Runtime: inference is delegated to a HOST NN
// PROVIDER registered on the JS side as Module.roxalNN (the browser plugs in
// onnxruntime-web with the WebGPU execution provider, node plugs in the same
// package's wasm EP, and an Electron host can plug in onnxruntime-node for
// native GPU) — one Roxal-facing module, best engine per host.
//
// Every request returns a future Value. That is the whole integration
// contract: predict hands the future straight to Roxal, where the existing
// machinery — FuncNode's yield-on-future for signal graphs, resolveArgMask,
// auto-resolve-on-index — consumes it exactly as it consumes the native
// InferenceWorker's futures. A failed request resolves the future to an
// exception Value, which raises catchably at the await point.
//
// Thread-safety: create/run/close may be called from any Roxal thread (the
// dataflow engine thread calls predict inside lifted FuncNodes). Requests are
// encoded and flushed from the calling thread; the registry is mutex-guarded;
// results are applied on the VM thread at the drainInbound() safepoint.

#include "Value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace roxal {
namespace web {

struct NnFeed {
    std::string name;
    std::string dtype;              // "float32", "int64", ...
    std::vector<int64_t> shape;
    const void* data;
    size_t len;
};

// Create a session from model bytes. Resolves to a dict:
//   { device: str, inputs: [ {name, shape, dtype}, ... ], outputs: [...] }
// `heldValues` pins any Values the request depends on until resolution.
Value nnCreate(std::vector<uint8_t>&& modelBytes, const std::string& device);

// Run inference. Resolves to one tensor, or a list of tensors for multi-output
// models — the same shape contract as the native backend.
Value nnRun(uint32_t sessionId, const std::vector<NnFeed>& feeds,
            std::vector<Value> heldValues);

// Fire-and-forget session teardown (no reply).
void nnClose(uint32_t sessionId);

// Block until `future` resolves, processing ONLY NnResult inbound items while
// waiting (the reply is drained by the calling thread itself, so a plain wait
// would deadlock). For Model.init and warmup — native init blocks comparably
// long inside Ort::Session construction, so the semantics match. Throws
// std::invalid_argument on provider error or timeout; otherwise returns the
// resolved Value.
Value nnAwaitBlocking(const Value& future, int timeoutMs);

// GC root hook: pins in-flight requests' futures and held input tensors.
// Called from SimpleMarkSweepGC::visitRoots; locks internally.
void nnTracePending(ValueVisitor& visitor);

} // namespace web
} // namespace roxal

#endif // __EMSCRIPTEN__
