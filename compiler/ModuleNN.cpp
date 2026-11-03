#include "ModuleNN.h"
#include "VM.h"
#include "SimpleMarkSweepGC.h"
#include "Object.h"
#include "GCRoots.h"
#include "core/json5.h"
#include <stdexcept>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <list>
#include <future>
#include <optional>
#include <functional>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#ifdef ROXAL_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#ifdef ROXAL_ENABLE_ONNX
#include "CudaRuntime.h"
#endif

#ifdef ROXAL_ENABLE_TRITON
#include "nn/TritonEndpoint.h"
#include "nn/TritonInfer.h"
#include "nn/TritonDeploy.h"
#include "RuntimeConfig.h"
#include "triton_inference.pb.h"
#include "GrpcStatus.h"
#include "ModuleSys.h"
#endif

#include "dataflow/Signal.h"

#ifdef __EMSCRIPTEN__
// The wasm build has no ONNX Runtime; inference is delegated to the host's
// NN provider (onnxruntime-web in the browser, onnxruntime-node under
// Electron) through the JS bridge. See compiler/web/NnBridge.h.
#include "web/NnBridge.h"
#include "web/WebHostLoop.h"
#endif

using namespace roxal;

// ============================================================
// OnnxEnvironment — lazy singleton managing the global Ort::Env
// ============================================================

#ifdef ROXAL_ENABLE_ONNX

class OnnxEnvironment {
public:
    static OnnxEnvironment& instance() {
        static OnnxEnvironment env;
        return env;
    }

    // Created on first use (any thread: actors construct models
    // concurrently), under a lock -- an unsynchronized check-then-create
    // could build two environments and destroy one another thread was
    // already using.  release() takes the same lock.
    Ort::Env& env() {
        std::lock_guard<std::mutex> lock(envMutex_);
        if (!env_)
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "roxal-nn");
        return *env_;
    }
    // Destroy the environment NOW -- at the end of VM shutdown, every session
    // gone -- rather than from __run_exit_handlers, where Ort::Env's
    // destructor ran the CUDA provider's teardown against memory another exit
    // handler had already freed (valgrind: invalid read in
    // libonnxruntime_providers_cuda.so under Ort::Env::~Env, of a block the
    // provider allocated while being appended to session options). That was
    // the intermittent "corrupted double-linked list" at exit of every script
    // that had used a CUDA session.
    void release() {
        std::lock_guard<std::mutex> lock(envMutex_);
        env_.reset();
    }

    Ort::SessionOptions createSessionOptions(bool requestGpu = true) {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        if (requestGpu && cudaAvailable()) {
            try {
                OrtCUDAProviderOptions cuda_opts{};
                cuda_opts.device_id = 0;
                opts.AppendExecutionProvider_CUDA(cuda_opts);
            } catch (...) {
                // CUDA provider not available; fall through to CPU
            }
        }
        return opts;
    }

    // Probed LAZILY, on the first request that could actually use the GPU
    // ('auto'/'cuda' models, memory_info for a non-'cpu' device).  ORT's probe
    // dlopens the CUDA provider and, on a box without the CUDA runtime, prints
    // a red "Failed to load libonnxruntime_providers_cuda.so" to stderr -- noise
    // a program that only ever asks for device='cpu' has no reason to see.
    bool cudaAvailable() {
        std::call_once(cudaProbeOnce_, [this] {
            try {
                // The provider registers against the environment's default
                // logger: probing before the (lazily created) Ort::Env
                // exists fails with "DefaultLogger ... none has been
                // registered" -- and that failure would be cached, putting
                // every model of the run on the CPU.
                env();
                Ort::SessionOptions probe;
                OrtCUDAProviderOptions cuda_opts{};
                cuda_opts.device_id = 0;
                probe.AppendExecutionProvider_CUDA(cuda_opts);
                cudaAvailable_ = true;
            } catch (const std::exception& e) {
                cudaAvailable_ = false;
                cudaProbeError_ = e.what();
            } catch (...) {
                cudaAvailable_ = false;
                cudaProbeError_ = "unknown error";
            }
        });
        return cudaAvailable_;
    }

    // Why the last probe failed (empty while it succeeded or never ran).
    const std::string& cudaProbeError() const { return cudaProbeError_; }

private:
    OnnxEnvironment() = default;
    std::unique_ptr<Ort::Env> env_;
    std::mutex envMutex_;
    std::once_flag cudaProbeOnce_;
    bool cudaAvailable_ = false;
    std::string cudaProbeError_;
};

static std::string ortDtypeToString(ONNXTensorElementDataType dt) {
    switch (dt) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:   return "float32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:  return "float64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return "float16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:    return "int8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:   return "int16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:   return "int32";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:   return "int64";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:   return "uint8";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:  return "uint16";
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:    return "bool";
        default: return "unknown";
    }
}

#endif // ROXAL_ENABLE_ONNX

// ============================================================
// InferenceWorker — per-model background thread for async inference
// ============================================================
// Follows the AsyncIOManager pattern: submit work, get future.
// Each model has its own worker to naturally serialize Session::Run()
// calls (ONNX sessions are NOT thread-safe for concurrent Run()).
//
// The thread outlives the model that started it.  A job holds what it runs
// (the session), never the model, and releasing a model only RETIRES its
// worker: the thread drains its queue and exits on its own, and the module
// joins it later (when the next worker starts, or at shutdown).  A model is
// released wherever its last reference goes -- in VM::freeObjects on the
// collector thread, where a join would hold up every collection for the
// length of an inference; and a job that held the model used to release it
// on the worker itself, whose join of its own thread was std::terminate.

class InferenceWorker {
    struct Job {
        std::function<Value()> work;
        std::shared_ptr<NativePromise> promise;
        std::vector<Value> heldValues;  // prevent GC of input tensors
    };

    // Everything the thread touches, shared with it: it lives as long as
    // either the thread or the worker does.
    struct State {
        std::mutex queueMutex;
        std::condition_variable queueCV;
        std::deque<Job> pendingJobs;
        // The job currently executing, kept here (not a workerLoop local) so
        // the registry trace can reach its heldValues -- the conservative
        // stack scan does not cover this thread. Same reasoning as
        // AsyncIOManager's processingOps.
        Job currentJob;
        bool currentActive = false;
        bool running = true;    // false: no new work; exit once the queue is empty
        bool exited = false;
    };

    // Every worker thread not yet joined, so shutdown can join them, and a
    // GC root: the held input tensors of queued and in-flight jobs are
    // reachable from nothing else -- a fire-and-forget predict whose caller
    // dropped its references could have its inputs swept MID-INFERENCE.
    struct Entry {
        std::shared_ptr<State> state;
        std::thread thread;
    };
    struct Registry {
        mutable std::mutex mutex;
        std::list<Entry> entries;   // a list: shutdownAll joins through pointers into it

        // Reached with live entries only if shutdownAll never ran (a VM that
        // was never shut down) and the process is exiting: a joinable
        // std::thread's destructor would terminate it.
        ~Registry() {
            for (auto& e : entries)
                if (e.thread.joinable())
                    e.thread.detach();
        }
    };
    static void traceRegistry(ValueVisitor& visitor, const Registry& r) {
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const auto& e : r.entries) {
            State& s = *e.state;
            std::lock_guard<std::mutex> qlock(s.queueMutex);
            for (const Job& job : s.pendingJobs)
                for (const Value& v : job.heldValues)
                    visitor.visit(v);
            if (s.currentActive)
                for (const Value& v : s.currentJob.heldValues)
                    visitor.visit(v);
        }
    }
    // Constructed with the program, not on first use: a root may not be
    // created inside an RT yield section, which is where a host-driven run
    // starts its first worker.
    static TracedMember<Registry> registry_;
    static Registry& registry() { return *registry_; }

    std::shared_ptr<State> state_;

    // Join the threads of retired workers that have run dry.  Each has left
    // its loop, so the join returns at once.
    static void reapExitedLocked(Registry& reg) {
        auto& r = reg.entries;
        for (auto it = r.begin(); it != r.end();) {
            bool exited;
            {
                std::lock_guard<std::mutex> lock(it->state->queueMutex);
                exited = it->state->exited;
            }
            if (exited) {
                it->thread.join();
                it = r.erase(it);
            } else {
                ++it;
            }
        }
    }

    // No new work: the thread exits once its queue is empty.  Given `abandonWhy`,
    // queued jobs are not run -- each future resolves to that error instead.
    // That drops the jobs' held Values and allocates, so only a mutator thread
    // passes one (Model.close(), shutdown) -- never a release from
    // VM::freeObjects.  The job in flight is left to finish: Session::Run
    // cannot be interrupted.  Never waits for the thread.
    static void retireState(State& s, const char* abandonWhy) {
        std::deque<Job> abandoned;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            s.running = false;
            if (abandonWhy)
                abandoned.swap(s.pendingJobs);
        }
        s.queueCV.notify_all();
        if (abandoned.empty())
            return;
        // Out of the queue the held inputs are on no root, and an allocation
        // may let a collection run: drop them BEFORE allocating the errors.
        std::vector<std::shared_ptr<NativePromise>> promises;
        for (auto& job : abandoned)
            promises.push_back(std::move(job.promise));
        abandoned.clear();
        for (auto& promise : promises)
            promise->resolve(Value::exceptionVal(Value::stringVal(toUnicodeString(abandonWhy))));
    }

    static void workerLoop(State& s) {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(s.queueMutex);
                s.queueCV.wait(lock, [&]{ return !s.pendingJobs.empty() || !s.running; });
                if (s.pendingJobs.empty())
                    break;   // retired, and nothing left to run
                s.currentJob = std::move(s.pendingJobs.front());
                s.pendingJobs.pop_front();
                s.currentActive = true;
            }
            Job& currentJob = s.currentJob;
            // The job's work holds no Values, but may hold the last reference
            // to its session (the model was closed or released meanwhile):
            // tearing that down is left until after the participant window.
            std::function<Value()> spentWork;
            {
                // A GC participant for the job's duration: the collector's
                // barrier waits for this thread between polls, so the output
                // tensors the job allocates on THIS (non-Roxal) thread are
                // never created behind a running collection -- which would
                // sweep them, unmarked, before the future ever published
                // them to a root. The Session::Run inside is bracketed
                // SafeBlocked (runOrt), so a long inference does not hold
                // the barrier; only the allocation of the results does.
                // A failure allocates too: it resolves the future to an
                // exception, raised where the future is consumed.
                SimpleMarkSweepGC::ExternalParticipant participant(SimpleMarkSweepGC::instance());
                Value result;
                try {
                    result = currentJob.work();
                } catch (const std::exception& e) {
                    result = Value::exceptionVal(Value::stringVal(
                        toUnicodeString(std::string("ai.nn: ") + e.what())));
                } catch (...) {
                    result = Value::exceptionVal(Value::stringVal(
                        toUnicodeString("ai.nn: unknown inference error")));
                }
                currentJob.promise->resolve(result);

                // Retire the job while still a participant: dropping its held
                // inputs decRefs GC objects, which an unregistered thread may
                // not do while a collection could be marking.
                Job finished;
                {
                    std::lock_guard<std::mutex> lock(s.queueMutex);
                    finished = std::move(s.currentJob);
                    s.currentJob = Job{};
                    s.currentActive = false;
                }
                spentWork = std::move(finished.work);
            }
        }
        std::lock_guard<std::mutex> lock(s.queueMutex);
        s.exited = true;
    }

public:
    Value submit(std::function<Value()> work, std::vector<Value> heldValues = {}) {
        auto promise = std::make_shared<NativePromise>();
        Value future = promise->future();
        {
            std::lock_guard<std::mutex> lock(state_->queueMutex);
            Job job;
            job.work = std::move(work);
            job.promise = std::move(promise);
            job.heldValues = std::move(heldValues);
            state_->pendingJobs.push_back(std::move(job));
        }
        state_->queueCV.notify_one();
        return future;
    }

    void start() {
        state_ = std::make_shared<State>();
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reapExitedLocked(reg);
        std::thread thread([s = state_] {
            VM::demoteCurrentThreadToNonRT();
            workerLoop(*s);
        });
        reg.entries.push_back(Entry{state_, std::move(thread)});
    }

    // See retireState.  Model.close() abandons what is queued; a model merely
    // released lets its queue drain.
    void retire(const char* abandonWhy = nullptr) {
        if (state_)
            retireState(*state_, abandonWhy);
    }

    // VM-shutdown quiesce: bring every worker to rest BEFORE the VM frees a
    // single object and before main() returns.  A worker is a plain
    // std::thread, not a VM Thread, so requestExit()'s joinAllThreads never
    // sees it; left alone it would still be inside Session::Run -- reading the
    // (GC-owned) input tensors -- while shutdown sweeps them, and then while
    // ONNX Runtime's own statics are destroyed from __run_exit_handlers after
    // main() returns.  Either is a use-after-free deep inside ORT (SIGSEGV at
    // exit whenever a script ends, normally or fatally, with inference in
    // flight).  Queued jobs are abandoned rather than drained: draining would
    // make exit take one full inference per backlogged frame.  The registry
    // keeps every entry until its thread is joined, so the trace still pins
    // the inputs of an inference finishing meanwhile.
    static void shutdownAll() {
        Registry& reg = registry();
        std::vector<Entry*> snapshot;
        {
            std::lock_guard<std::mutex> lock(reg.mutex);
            for (auto& e : reg.entries)
                snapshot.push_back(&e);
        }
        for (Entry* e : snapshot)
            retireState(*e->state, "ai.nn: inference abandoned at shutdown");
        for (Entry* e : snapshot)
            if (e->thread.joinable())
                e->thread.join();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.entries.clear();
    }

    // Released with its model: never waits (see the class comment).
    ~InferenceWorker() {
        retire();
    }
};

TracedMember<InferenceWorker::Registry> InferenceWorker::registry_ { &InferenceWorker::traceRegistry };

#ifdef ROXAL_ENABLE_TRITON
static void shutdownTriton();
#endif

void ModuleNN::onShutdown(VM&)
{
    InferenceWorker::shutdownAll();
#ifdef ROXAL_ENABLE_TRITON
    shutdownTriton();
#endif
}

std::function<void()> ModuleNN::shutdownCompleteAction()
{
#ifdef ROXAL_ENABLE_ONNX
    return [] { OnnxEnvironment::instance().release(); };
#else
    return {};
#endif
}

// ============================================================
// ModelWrapper — per-model native state
// ============================================================

// One input or output of a model.
struct TensorSpec {
    std::string name;
    std::vector<int64_t> shape;     // -1: a dynamic dimension
    std::string dtype;              // a TensorDType name ("float32", ...)
};

// Named input tensors, in the model's input order.
using NamedInputs = std::vector<std::pair<std::string, ObjTensor*>>;

// A model's backend.  The shared front end -- predict's argument checks,
// the predict closure, inputs()/outputs()/device(), close() -- drives one of
// these per model; opening one fills in the model's signature.
// The server a model runs on.  `kind` is how Roxal reached it -- the
// backend, named as its source scheme ("triton") -- and the rest is how the
// server describes itself (Triton's ServerMetadata): a server speaking the
// same protocol may name itself otherwise.  A model run in this process has
// none.
struct ModelServerInfo {
    std::string kind;
    std::string name;
    std::string version;
    std::vector<std::string> extensions;
};

// A model Roxal deployed to a server itself (Model(path, server=...)).
struct ModelDeployment {
    std::string server;         // host:port
    std::string name;           // what the server serves it as
    std::string transport;      // "upload", "repository", or "reused" (already served)
    std::string repository;     // the shared directory holding its files, if one does
};

class ModelSession {
public:
    virtual ~ModelSession() = default;

    // As read when the model was opened.
    virtual std::optional<ModelServerInfo> server() const { return std::nullopt; }

    virtual std::optional<ModelDeployment> deployment() const { return std::nullopt; }

    // Model.unload(): take a model this program deployed off its server
    // (removing its files from the shared directory too, if asked).
    virtual void unload(bool removeFiles) {
        (void)removeFiles;
        throw std::invalid_argument("Model.unload: only a model deployed with Model(path, server=...) "
                                    "can be unloaded");
    }

    // Start one inference on inputs already checked against the model;
    // heldValues pin the tensors until it is done.  Returns a future that
    // resolves to the output tensor, or a list of them for several outputs.
    virtual Value submit(const NamedInputs& inputs, std::vector<Value> heldValues) = 0;

    // Model.close(), on the VM thread: work not yet started resolves to an
    // error, and nothing waits for work in flight.
    virtual void close() = 0;
};

struct ModelWrapper {
    std::vector<TensorSpec> inputs;
    std::vector<TensorSpec> outputs;
    std::string device = "cpu";
    bool closed = false;
    std::unique_ptr<ModelSession> session;
};

// ============================================================
// Helpers
// ============================================================

static ModelWrapper* getModelWrapper(ObjectInstance* inst) {
    Value fpVal = inst->getProperty("_this");
    if (fpVal.isNil() || !isForeignPtr(fpVal))
        throw std::runtime_error("Model not properly initialized");
    auto* sp = static_cast<std::shared_ptr<ModelWrapper>*>(asForeignPtr(fpVal)->ptr);
    return sp->get();
}

static std::shared_ptr<ModelWrapper> getModelWrapperShared(ObjectInstance* inst) {
    Value fpVal = inst->getProperty("_this");
    if (fpVal.isNil() || !isForeignPtr(fpVal))
        throw std::runtime_error("Model not properly initialized");
    return *static_cast<std::shared_ptr<ModelWrapper>*>(asForeignPtr(fpVal)->ptr);
}

// Check if a tensor shape is compatible with a model's expected shape.
// Dynamic dimensions (-1) in the expected shape accept any positive value.
static bool isShapeCompatible(const std::vector<int64_t>& actual,
                              const std::vector<int64_t>& expected)
{
    if (actual.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] < 0)
            continue; // dynamic dimension, any positive value is fine
        if (actual[i] != expected[i])
            return false;
    }
    return true;
}

static std::string shapeToString(const std::vector<int64_t>& shape)
{
    std::string s = "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i > 0) s += ", ";
        if (shape[i] < 0)
            s += "?";
        else
            s += std::to_string(shape[i]);
    }
    return s + "]";
}

// Model(..., outputs=[names]): the outputs predict returns, in that order --
// a subset or a reordering of the model's own.  Applied to the signature
// before a backend binds its outputs.
using OutputSelection = std::optional<std::vector<std::string>>;

static OutputSelection outputSelectionArg(const Value& arg)
{
    if (arg.isNil())
        return std::nullopt;
    if (!isList(arg))
        throw std::invalid_argument("Model.init: outputs must be a list of output names");
    ObjList* list = asList(arg);
    std::vector<std::string> names;
    for (int32_t i = 0; i < list->length(); ++i) {
        Value v = list->getElement(i);
        if (!isString(v))
            throw std::invalid_argument("Model.init: outputs must be a list of output names");
        names.push_back(toUTF8StdString(asStringObj(v)->s));
    }
    if (names.empty())
        throw std::invalid_argument("Model.init: outputs must name at least one output");
    return names;
}

static std::string joinNames(const std::vector<TensorSpec>& specs);

static void selectOutputs(std::vector<TensorSpec>& outputs, const std::vector<std::string>& names)
{
    std::vector<TensorSpec> selected;
    for (const auto& name : names) {
        auto named = [&](const TensorSpec& spec) { return spec.name == name; };
        if (std::find_if(selected.begin(), selected.end(), named) != selected.end())
            throw std::invalid_argument("Model.init: output '" + name + "' is named twice in outputs");
        auto it = std::find_if(outputs.begin(), outputs.end(), named);
        if (it == outputs.end())
            throw std::invalid_argument("Model.init: the model has no output '" + name +
                                        "' (its outputs: " + joinNames(outputs) + ")");
        selected.push_back(*it);
    }
    outputs = std::move(selected);
}

// Zero-valued inputs for a warmup inference, which initializes the CUDA
// context and triggers ORT graph optimization so the first real predict() is
// fast and consistent.  None (false) when any input has a dynamic non-batch
// dimension: a guessed size can violate model-internal constraints (e.g. a
// detector's TopK over anchors) -- the failed kernel is harmless, but ONNX
// Runtime logs a scary error for it.  A dynamic batch dimension becomes 1.
// The tensors are kept in the returned list (nil: no warmup), which the
// caller holds in a local Value -- a stack reference the collector sees --
// for the run.
static Value warmupInputs(const ModelWrapper& model, NamedInputs& inputs)
{
    if (model.inputs.empty())
        return Value::nilVal();
    for (const auto& spec : model.inputs)
        for (size_t di = 1; di < spec.shape.size(); ++di)
            if (spec.shape[di] < 0)
                return Value::nilVal();
    Value tensors = Value::objVal(newListObj());
    for (const auto& spec : model.inputs) {
        auto shape = spec.shape;
        if (!shape.empty() && shape[0] < 0)
            shape[0] = 1;
        Value tensor = Value::tensorVal(shape, tensorDTypeFromString(spec.dtype));
        asList(tensors)->append(tensor);
        inputs.emplace_back(spec.name, asTensor(tensor));
    }
    return tensors;
}

#ifdef ROXAL_ENABLE_ONNX
// What an inference runs against.  A worker job holds its own reference, so
// it never keeps the model -- and with it the job's own worker -- alive (see
// InferenceWorker).
struct OrtRunTarget {
    std::shared_ptr<Ort::Session> session;
    std::vector<std::string> outputNames;
    bool cuda = false;
};

// One ORT inference with named inputs, already validated against the model
// (predict validates on the calling thread; warmup builds conforming inputs).
static Value runOrt(const OrtRunTarget& target, const NamedInputs& inputTensors) {
    // Use IoBinding for both CPU and CUDA: it allows binding each input
    // individually by name (ORT's Session::Run requires a contiguous array
    // of Ort::Value which we can't form from separate ObjTensor objects).
    Ort::IoBinding binding(*target.session);

    for (auto& [name, tensor] : inputTensors)
        binding.BindInput(name.c_str(), tensor->ortValue());

    std::vector<Ort::Value> outputs;

    if (target.cuda) {
        Ort::MemoryInfo cudaMemInfo("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        for (auto& name : target.outputNames)
            binding.BindOutput(name.c_str(), cudaMemInfo);
    } else {
        Ort::MemoryInfo cpuMemInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        for (auto& name : target.outputNames)
            binding.BindOutput(name.c_str(), cpuMemInfo);
    }

    {
        // Pure native work on bound buffers; the Roxal inputs are rooted by
        // the job's heldValues (or the caller's stack on the sync path), so
        // the thread can be quiescent for the GC while ORT runs.
        SimpleMarkSweepGC::GCBlockedScope blocked;
        target.session->Run(Ort::RunOptions{nullptr}, binding);
    }
    outputs = binding.GetOutputValues();

    if (outputs.size() == 1) {
        return Value::objVal(newTensorObj(std::move(outputs[0])));
    }

    // Multiple outputs: return list of tensors
    Value list = Value::objVal(newListObj());
    for (auto& out : outputs)
        asList(list)->append(Value::objVal(newTensorObj(std::move(out))));
    return list;
}

// ONNX Runtime in this process.  Inferences run one at a time on the model's
// worker thread: an ORT session must not Run concurrently.
class OrtSession : public ModelSession {
public:
    explicit OrtSession(std::shared_ptr<OrtRunTarget> target)
        : target_(std::move(target))
    {
        worker_.start();
    }

    Value submit(const NamedInputs& inputs, std::vector<Value> heldValues) override {
        for (auto& [name, tensor] : inputs)
            if (!tensor->isOrtBacked())
                throw std::runtime_error("ai.nn: input tensor '" + name + "' is not ORT-backed");
        // The job holds the session itself, not the model -- see InferenceWorker.
        auto work = [target = target_, inputs]() -> Value {
            return runOrt(*target, inputs);
        };
        return worker_.submit(std::move(work), std::move(heldValues));
    }

    // An inference in flight finishes on its own reference to the session.
    void close() override {
        worker_.retire("ai.nn: model closed");
        target_.reset();
    }

private:
    std::shared_ptr<OrtRunTarget> target_;
    InferenceWorker worker_;
};

static std::unique_ptr<ModelSession> openOrtSession(const std::string& path,
                                                    const std::string& deviceArg,
                                                    bool doWarmup,
                                                    const OutputSelection& selection,
                                                    ModelWrapper& model)
{
    bool requestGpu;
    if (deviceArg == "auto")
        requestGpu = true;
    else if (deviceArg == "cuda")
        requestGpu = true;
    else if (deviceArg == "cpu")
        requestGpu = false;
    else
        throw std::invalid_argument("Model.init: device must be 'auto', 'cpu', or 'cuda'");

    auto& ortEnv = OnnxEnvironment::instance();
    // An explicit 'cuda' that cannot be honoured is an error, not a silent
    // CPU session: the caller asked for the GPU by name.  ('auto' keeps the
    // quiet fallback.)
    if (deviceArg == "cuda" && !ortEnv.cudaAvailable())
        throw std::invalid_argument("Model.init: device='cuda' but the CUDA execution provider "
                                    "is not available: " + ortEnv.cudaProbeError());
    auto opts = ortEnv.createSessionOptions(requestGpu);

    std::shared_ptr<Ort::Session> session;
    try {
        // Loading reads and optimizes the whole model -- seconds for a large
        // one -- and touches nothing GC-managed: collections need not wait.
        SimpleMarkSweepGC::GCBlockedScope blocked;
        session = std::make_shared<Ort::Session>(ortEnv.env(), path.c_str(), opts);
    } catch (const Ort::Exception& e) {
        throw std::invalid_argument(std::string("Model.init: failed to load '") + path + "': " + e.what());
    }
    model.device = (requestGpu && ortEnv.cudaAvailable()) ? "cuda" : "cpu";

    Ort::AllocatorWithDefaultOptions allocator;
    for (size_t i = 0; i < session->GetInputCount(); ++i) {
        auto typeInfo = session->GetInputTypeInfo(i);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        model.inputs.push_back({session->GetInputNameAllocated(i, allocator).get(),
                                tensorInfo.GetShape(),
                                ortDtypeToString(tensorInfo.GetElementType())});
    }
    for (size_t i = 0; i < session->GetOutputCount(); ++i) {
        auto typeInfo = session->GetOutputTypeInfo(i);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        model.outputs.push_back({session->GetOutputNameAllocated(i, allocator).get(),
                                 tensorInfo.GetShape(),
                                 ortDtypeToString(tensorInfo.GetElementType())});
    }

    // ORT computes just the outputs bound, in the order bound.
    if (selection)
        selectOutputs(model.outputs, *selection);

    auto target = std::make_shared<OrtRunTarget>();
    target->session = std::move(session);
    for (const auto& spec : model.outputs)
        target->outputNames.push_back(spec.name);
    target->cuda = model.device == "cuda";

    if (doWarmup) {
        NamedInputs inputs;
        const Value tensors = warmupInputs(model, inputs);
        if (!tensors.isNil()) {
            try {
                runOrt(*target, inputs);
            } catch (const std::exception&) {
                // Warmup failure is non-fatal — the model is still usable.
            }
        }
    }
    return std::make_unique<OrtSession>(std::move(target));
}
#endif // ROXAL_ENABLE_ONNX

#ifdef ROXAL_ENABLE_TRITON
// ============================================================
// Triton — models served by an NVIDIA Triton Inference Server
// ============================================================
// Model("triton://host[:port]/model[?version=N]"): the server runs the
// inference, reached over gRPC (Triton's GRPCInferenceService) through a
// TritonEndpoint shared by every model on that host:port.  Each predict is
// its own call -- there is no queue here: the server schedules, and may batch
// concurrent requests -- and its inputs are copied into the request when it
// is made, so a call in flight holds no Roxal values.

struct TritonSource {
    std::string address;    // host:port
    std::string model;
    std::string version;    // empty: the server's choice
};

// Null for a source that is not triton://; the port defaults to 8001,
// Triton's gRPC port.
static std::optional<TritonSource> parseTritonSource(const std::string& path)
{
    static const std::string scheme = "triton://";
    if (path.rfind(scheme, 0) != 0)
        return std::nullopt;
    const std::string form = "Model.init: '" + path +
                             "' is not triton://host[:port]/model[?version=N]";
    std::string rest = path.substr(scheme.size());
    TritonSource source;
    const auto query = rest.find('?');
    if (query != std::string::npos) {
        const std::string q = rest.substr(query + 1);
        rest.resize(query);
        if (q.rfind("version=", 0) != 0 || q.size() == 8)
            throw std::invalid_argument(form);
        source.version = q.substr(8);
    }
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == rest.size()
        || rest.find('/', slash + 1) != std::string::npos)
        throw std::invalid_argument(form);
    source.address = nn::normalizeAddress(rest.substr(0, slash));
    source.model = rest.substr(slash + 1);
    return source;
}

// Triton's datatype names, against the tensor dtypes they are (BF16, UINT32,
// UINT64 and BYTES have no tensor dtype).
static const std::pair<const char*, const char*> tritonDatatypes[] = {
    {"FP16", "float16"}, {"FP32", "float32"}, {"FP64", "float64"},
    {"INT8", "int8"}, {"INT16", "int16"}, {"INT32", "int32"}, {"INT64", "int64"},
    {"UINT8", "uint8"}, {"UINT16", "uint16"}, {"BOOL", "bool"},
};

static std::string dtypeOfTriton(const std::string& datatype)
{
    for (const auto& [triton, dtype] : tritonDatatypes)
        if (datatype == triton)
            return dtype;
    return "";
}

static const char* tritonOfDtype(const std::string& dtype)
{
    for (const auto& [triton, name] : tritonDatatypes)
        if (dtype == name)
            return triton;
    return nullptr;
}

// The exception types a reply raises, looked up on a VM thread when a model
// opens: the endpoint's thread, which builds the exceptions, does not read
// the VM's globals.  A root for the program's lifetime (one may not be
// created inside an RT yield section), emptied at shutdown.
struct TritonErrorTypes {
    mutable std::mutex mutex;
    std::unordered_map<std::string, Value> types;
};

static void traceTritonErrorTypes(ValueVisitor& visitor, const TritonErrorTypes& t)
{
    std::lock_guard<std::mutex> lock(t.mutex);
    for (const auto& [name, type] : t.types)
        visitor.visit(type);
}

static TracedMember<TritonErrorTypes> tritonErrorTypes { &traceTritonErrorTypes };

static void rememberTritonErrorTypes(VM& vm)
{
    std::lock_guard<std::mutex> lock(tritonErrorTypes->mutex);
    for (const char* name : {"UnavailableError", "ProgramException", "RuntimeException"})
        if (!tritonErrorTypes->types.count(name))
            tritonErrorTypes->types[name] =
                vm.loadGlobal(toUnicodeString(name)).value_or(Value::nilVal());
}

static Value tritonErrorType(const char* name)
{
    std::lock_guard<std::mutex> lock(tritonErrorTypes->mutex);
    auto it = tritonErrorTypes->types.find(name);
    return it != tritonErrorTypes->types.end() ? it->second : Value::nilVal();
}

// What a call in flight completes against: shared with the session, so a
// model closed or released meanwhile still settles its calls.
struct TritonCallTarget {
    std::string where;                  // "Triton model 'm' at host:port", for messages
    std::vector<TensorSpec> outputs;    // as predict returns them
    std::chrono::microseconds timeout { 0 };
    // The model's `available` signal, weakly: a call must not keep a signal
    // -- and the Values in it -- alive past the model and its run.
    roxal::weak_ptr<df::Signal> available;
    std::atomic<bool> lastAvailable { true };
};

// A failed call as the exception its future resolves to.
static Value tritonFailure(const TritonCallTarget& target, const grpc::Status& status)
{
    const auto code = status.error_code();
    std::string message = "ai.nn: " + target.where + ": " + status.error_message();
    if (code == grpc::StatusCode::DEADLINE_EXCEEDED)
        message += " (timeout " + std::to_string(target.timeout.count() / 1000) + " ms)";
    message += std::string(" [") + grpcStatusCodeName(code) + "]";
    Value detail = Value::dictVal();
    ObjDict* dict = asDict(detail);
    dict->store(Value::stringVal(toUnicodeString("grpc_status_code")),
                Value::intVal(static_cast<int64_t>(code)));
    dict->store(Value::stringVal(toUnicodeString("grpc_status_name")),
                Value::stringVal(toUnicodeString(grpcStatusCodeName(code))));
    dict->store(Value::stringVal(toUnicodeString("grpc_status_message")),
                Value::stringVal(toUnicodeString(status.error_message())));
    return Value::exceptionVal(Value::stringVal(toUnicodeString(message)),
                               tritonErrorType(grpcExceptionTypeName(code)),
                               Value::nilVal(), detail);
}

// The outputs predict returns, from a decoded reply: matched by NAME -- a
// server's output order is its own -- and checked against the model's
// signature.  Each tensor adopts its output's block.
static Value tritonOutputs(const TritonCallTarget& target,
                           const roxal::triton::ModelInferResponse& response,
                           const std::vector<nn::InferOutputBytes>& contents)
{
    if (contents.size() != static_cast<size_t>(response.outputs_size()))
        throw std::runtime_error("the reply does not carry every output's contents");

    Value list = target.outputs.size() > 1 ? Value::objVal(newListObj()) : Value::nilVal();
    Value single;
    for (const auto& spec : target.outputs) {
        int index = -1;
        for (int i = 0; i < response.outputs_size(); ++i)
            if (response.outputs(i).name() == spec.name)
                index = i;
        if (index < 0)
            throw std::runtime_error("the reply has no output '" + spec.name + "'");
        const auto& out = response.outputs(index);
        if (dtypeOfTriton(out.datatype()) != spec.dtype)
            throw std::runtime_error("output '" + spec.name + "' is " + out.datatype() +
                                     ", not the model's " + spec.dtype);
        const std::vector<int64_t> shape(out.shape().begin(), out.shape().end());
        const TensorDType dtype = tensorDTypeFromString(spec.dtype);
        const auto& bytes = contents[index];
        size_t numel = 1;
        for (auto dim : shape)
            numel *= static_cast<size_t>(dim < 0 ? 0 : dim);
        if (numel * tensorDTypeSize(dtype) != bytes.size)
            throw std::runtime_error("output '" + spec.name + "' has " + std::to_string(bytes.size) +
                                     " bytes for shape " + shapeToString(shape));
        Value tensor = Value::objVal(newTensorObj(shape, dtype, bytes.block, bytes.block.get()));
        if (list.isNil())
            single = tensor;
        else
            asList(list)->append(tensor);
    }
    return list.isNil() ? single : list;
}

// `available` follows the server: false after a call fails as unavailable,
// true again after any reply.  Set on a change only -- every set of an
// event-driven source evaluates its island.
static void noteAvailability(TritonCallTarget& target, bool available)
{
    if (target.lastAvailable.exchange(available) == available)
        return;
    if (auto signal = target.available.lock())
        signal->set(Value::boolVal(available));
}

// Raised as a typed Roxal exception (a server unreachable is an
// UnavailableError, a model it does not have a ProgramException).
struct TypedNnError : std::runtime_error {
    TypedNnError(const char* type, const std::string& what)
        : std::runtime_error(what), typeName(type) {}
    const char* typeName;
};

// A failed call to the server's model repository (load, unload, index).  A
// server that only loads its own models -- not started in explicit mode --
// refuses with UNAVAILABLE, which would read as "try again later": it is
// told apart by its message, and raised as the permanent refusal it is.
static TypedNnError repositoryFailure(const std::string& what, const std::string& address,
                                      const grpc::Status& status)
{
    const std::string message = status.error_message();
    if (status.error_code() == grpc::StatusCode::UNAVAILABLE && message.find("not allowed") != std::string::npos)
        return TypedNnError("ProgramException",
                            what + ": the server at " + address + " does not load models on request "
                            "(start it with --model-control-mode=explicit): " + message);
    return TypedNnError(grpcExceptionTypeName(status.error_code()),
                        what + ": " + message + " [" + grpcStatusCodeName(status.error_code()) + "]");
}

class TritonSession : public ModelSession {
public:
    TritonSession(std::shared_ptr<nn::TritonEndpoint> endpoint, TritonSource source,
                  std::shared_ptr<TritonCallTarget> target, std::optional<ModelServerInfo> server,
                  std::optional<ModelDeployment> deployment)
        : endpoint_(std::move(endpoint)), source_(std::move(source)), target_(std::move(target)),
          server_(std::move(server)), deployment_(std::move(deployment)) {}

    std::optional<ModelServerInfo> server() const override { return server_; }

    std::optional<ModelDeployment> deployment() const override { return deployment_; }

    void unload(bool removeFiles) override {
        if (!deployment_)
            throw std::invalid_argument("Model.unload: '" + source_.model + "' was not deployed by this "
                                        "program -- a triton:// model is its server's to manage");
        roxal::triton::RepositoryModelUnloadRequest request;
        request.set_model_name(deployment_->name);
        roxal::triton::RepositoryModelUnloadResponse reply;
        const auto status = endpoint_->call("RepositoryModelUnload", request, reply, std::chrono::seconds(60));
        if (!status.ok())
            throw repositoryFailure("Model.unload: '" + deployment_->name + "' at " + deployment_->server,
                                    deployment_->server, status);
        if (removeFiles && !deployment_->repository.empty()) {
            SimpleMarkSweepGC::GCBlockedScope blocked;
            std::error_code ignored;
            std::filesystem::remove_all(std::filesystem::path(deployment_->repository) / deployment_->name,
                                        ignored);
        }
    }

    Value submit(const NamedInputs& inputs, std::vector<Value>) override {
        auto promise = std::make_shared<NativePromise>();
        Value future = promise->future();

        // A server already known unreachable would refuse the call at once:
        // fail without copying the inputs (a clocked island retries every
        // tick).
        if (endpoint_->unreachable()) {
            noteAvailability(*target_, false);
            promise->resolve(tritonFailure(*target_,
                grpc::Status(grpc::StatusCode::UNAVAILABLE, "the server is unreachable")));
            return future;
        }

        // Each input goes out as the tensor's own bytes, held (not copied)
        // until sent: a write to the tensor meanwhile copies first.
        roxal::triton::ModelInferRequest request;
        request.set_model_name(source_.model);
        request.set_model_version(source_.version);
        std::vector<nn::InferInputBytes> payload;
        for (const auto& [name, tensor] : inputs) {
            auto* in = request.add_inputs();
            in->set_name(name);
            in->set_datatype(tritonOfDtype(to_string(tensor->dtype())));
            for (auto dim : tensor->shape())
                in->add_shape(dim);
            payload.push_back({tensor->sharedData(),
                               static_cast<size_t>(tensor->numel()) * tensorDTypeSize(tensor->dtype())});
        }
        for (const auto& spec : target_->outputs)
            request.add_outputs()->set_name(spec.name);

        auto target = target_;
        const bool started = endpoint_->callAsync("ModelInfer",
                                                  nn::encodeInferRequest(request, std::move(payload)),
                                                  target_->timeout,
            [target, promise](const grpc::Status& status, grpc::ByteBuffer& reply) {
                // On the endpoint's thread.  The reply's bytes first, before
                // joining the GC: copying them involves no Value.
                roxal::triton::ModelInferResponse response;
                std::vector<nn::InferOutputBytes> contents;
                const bool decoded = status.ok() && nn::decodeInferResponse(reply, response, contents);

                // A GC participant while the reply becomes tensors (or an
                // exception) and the future resolves.
                SimpleMarkSweepGC::ExternalParticipant participant(SimpleMarkSweepGC::instance());
                Value result;
                try {
                    if (!status.ok())
                        result = tritonFailure(*target, status);
                    else if (!decoded)
                        throw std::runtime_error("malformed inference reply");
                    else
                        result = tritonOutputs(*target, response, contents);
                } catch (const std::exception& e) {
                    result = tritonFailure(*target, grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
                }
                noteAvailability(*target, status.ok()
                    || classifyGrpcStatus(status.error_code()) != GrpcFailureKind::Unavailable);
                promise->resolve(result);
            });
        if (!started)
            promise->resolve(Value::exceptionVal(Value::stringVal(
                toUnicodeString("ai.nn: inference abandoned at shutdown"))));
        return future;
    }

    // Nothing is queued here; calls in flight finish.
    void close() override {}

private:
    std::shared_ptr<nn::TritonEndpoint> endpoint_;
    TritonSource source_;
    std::shared_ptr<TritonCallTarget> target_;
    std::optional<ModelServerInfo> server_;
    std::optional<ModelDeployment> deployment_;
};

// Who the server at `endpoint` says it is (a blocking call, GC-blocked).
static grpc::Status queryTritonServer(nn::TritonEndpoint& endpoint, ModelServerInfo& info)
{
    roxal::triton::ServerMetadataRequest request;
    roxal::triton::ServerMetadataResponse reply;
    auto status = endpoint.call("ServerMetadata", request, reply, std::chrono::milliseconds(5000));
    if (status.ok()) {
        info.kind = "triton";
        info.name = reply.name();
        info.version = reply.version();
        info.extensions.assign(reply.extensions().begin(), reply.extensions().end());
    }
    return status;
}

static std::unique_ptr<ModelSession> openTritonSession(const TritonSource& source,
                                                       const std::string& deviceArg,
                                                       const OutputSelection& selection,
                                                       std::chrono::microseconds timeout,
                                                       const ptr<df::Signal>& available,
                                                       ModelWrapper& model,
                                                       std::optional<ModelDeployment> deployment = std::nullopt)
{
    if (deviceArg != "auto")
        throw std::invalid_argument("Model.init: a Triton model runs where its server places it: "
                                    "device must be 'auto'");
    rememberTritonErrorTypes(VM::instance());
    auto endpoint = nn::TritonEndpoint::get(source.address);
    std::string where = "Triton model '" + source.model + "' at " + source.address;
    auto initFailure = [&](const grpc::Status& status) {
        return TypedNnError(grpcExceptionTypeName(status.error_code()),
                              "Model.init: " + where + ": " + status.error_message() +
                              " [" + grpcStatusCodeName(status.error_code()) + "]");
    };
    const auto deadline = std::chrono::milliseconds(5000);

    // Who the server is, first.  One that cannot be reached fails here; one
    // that will not say is left unnamed, and the model calls below report
    // whatever else it does not do.  Named, it is named in every message.
    std::optional<ModelServerInfo> server;
    {
        ModelServerInfo info;
        auto status = queryTritonServer(*endpoint, info);
        if (status.ok()) {
            where += " (" + info.name + " " + info.version + ")";
            server = std::move(info);
        } else if (classifyGrpcStatus(status.error_code()) == GrpcFailureKind::Unavailable) {
            throw initFailure(status);
        }
    }

    // Metadata first: for a model the server does not have, it says so
    // (NOT_FOUND), where readiness only says "not ready".
    roxal::triton::ModelMetadataRequest metaRequest;
    metaRequest.set_name(source.model);
    metaRequest.set_version(source.version);
    roxal::triton::ModelMetadataResponse meta;
    auto status = endpoint->call("ModelMetadata", metaRequest, meta, deadline);
    if (!status.ok())
        throw initFailure(status);
    roxal::triton::ModelReadyRequest readyRequest;
    readyRequest.set_name(source.model);
    readyRequest.set_version(source.version);
    roxal::triton::ModelReadyResponse ready;
    status = endpoint->call("ModelReady", readyRequest, ready, deadline);
    if (!status.ok())
        throw initFailure(status);
    if (!ready.ready())
        throw TypedNnError("UnavailableError", "Model.init: " + where + " is not ready");

    for (const auto& t : meta.inputs())
        model.inputs.push_back({t.name(), {t.shape().begin(), t.shape().end()},
                                dtypeOfTriton(t.datatype())});
    for (const auto& t : meta.outputs())
        model.outputs.push_back({t.name(), {t.shape().begin(), t.shape().end()},
                                 dtypeOfTriton(t.datatype())});
    if (selection)
        selectOutputs(model.outputs, *selection);
    auto unsupported = [&](const std::vector<TensorSpec>& specs, const char* kind,
                           const google::protobuf::RepeatedPtrField<
                               roxal::triton::ModelMetadataResponse::TensorMetadata>& described) {
        for (const auto& spec : specs)
            if (spec.dtype.empty())
                for (const auto& t : described)
                    if (t.name() == spec.name)
                        throw std::invalid_argument("Model.init: " + where + ": " + kind + " '" +
                                                    spec.name + "' is " + t.datatype() +
                                                    ", which no tensor dtype holds");
    };
    unsupported(model.inputs, "input", meta.inputs());
    unsupported(model.outputs, "output", meta.outputs());
    model.device = "triton";

    auto target = std::make_shared<TritonCallTarget>();
    target->where = where;
    target->outputs = model.outputs;
    target->timeout = timeout;
    target->available = available;
    return std::make_unique<TritonSession>(std::move(endpoint), source, std::move(target),
                                           std::move(server), std::move(deployment));
}

// ---- Model(path, server=...): deploying a model file to a Triton server ----

// The shared repositories scripts set (ai.nn.use_repository), by server.
struct ScriptRepositories {
    std::mutex mutex;
    std::map<std::string, std::filesystem::path> byServer;
};

static ScriptRepositories& scriptRepositories()
{
    static ScriptRepositories repositories;
    return repositories;
}

// The directory, as this machine sees it, that the server at `address` serves
// as one of its model repositories -- set by the script, else by the host or
// command line (RuntimeConfig "triton.repositories"), else by the environment
// (ROXAL_TRITON_REPOSITORIES).  None: models are uploaded.
static std::optional<std::filesystem::path> sharedRepositoryFor(const std::string& address)
{
    {
        auto& script = scriptRepositories();
        std::lock_guard<std::mutex> lock(script.mutex);
        auto it = script.byServer.find(address);
        if (it != script.byServer.end())
            return it->second;
    }
    std::optional<std::string> lists[] = {
        RuntimeConfig::get("triton.repositories"),
        std::getenv("ROXAL_TRITON_REPOSITORIES") ? std::optional<std::string>(std::getenv("ROXAL_TRITON_REPOSITORIES"))
                                                 : std::nullopt,
    };
    const char* origins[] = {"triton.repositories", "ROXAL_TRITON_REPOSITORIES"};
    for (int i = 0; i < 2; ++i) {
        if (!lists[i])
            continue;
        try {
            const auto byServer = nn::parseRepositoryList(*lists[i]);
            auto it = byServer.find(address);
            if (it != byServer.end())
                return it->second;
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument(std::string(origins[i]) + ": " + e.what());
        }
    }
    return std::nullopt;
}

// Check, once per server and directory, that the server really serves the
// directory: a uniquely named directory made in it must show in the server's
// repository index.
static void verifySharedRepository(nn::TritonEndpoint& endpoint, const std::string& address,
                                   const std::filesystem::path& directory, const std::string& what)
{
    static std::mutex mutex;
    static std::set<std::pair<std::string, std::string>> verified;
    const auto key = std::make_pair(address, directory.string());
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (verified.count(key))
            return;
    }
    const std::string notServed =
        what + ": '" + directory.string() + "' is not one of " + address + "'s model repositories "
        "(start the server with it mounted and passed to --model-repository, or correct "
        "ai.nn.use_repository / triton.repositories / ROXAL_TRITON_REPOSITORIES)";
    if (!std::filesystem::is_directory(directory))
        throw TypedNnError("ProgramException", notServed);
    std::random_device random;
    const std::string probe = "roxal.probe." + std::to_string(random()) + std::to_string(random());
    std::error_code ec;
    std::filesystem::create_directory(directory / probe, ec);
    if (ec)
        throw TypedNnError("ProgramException", what + ": cannot write to '" + directory.string() + "': " +
                                               ec.message());
    roxal::triton::RepositoryIndexRequest request;
    roxal::triton::RepositoryIndexResponse index;
    const auto status = endpoint.call("RepositoryIndex", request, index, std::chrono::seconds(10));
    std::filesystem::remove_all(directory / probe, ec);
    if (!status.ok())
        throw repositoryFailure(what, address, status);
    bool seen = false;
    for (const auto& model : index.models())
        seen = seen || model.name() == probe;
    if (!seen)
        throw TypedNnError("ProgramException", notServed);
    std::lock_guard<std::mutex> lock(mutex);
    verified.insert(key);
}

// Servers and directories where a symlink did not resolve for the server:
// copy from then on.
static bool symlinksFailed(const std::pair<std::string, std::string>& key, bool note = false)
{
    static std::mutex mutex;
    static std::set<std::pair<std::string, std::string>> failed;
    std::lock_guard<std::mutex> lock(mutex);
    if (note)
        failed.insert(key);
    return failed.count(key) > 0;
}

static grpc::Status loadOnServer(nn::TritonEndpoint& endpoint, const std::string& name,
                                 const std::string& configJson, const nn::ModelFileSet* upload)
{
    roxal::triton::RepositoryModelLoadResponse reply;
    // Generous: an upload of up to 2 GB, then the server loading the model.
    return endpoint.call("RepositoryModelLoad", nn::encodeLoadRequest(name, configJson, upload), reply,
                         std::chrono::minutes(10));
}

static std::unique_ptr<ModelSession> deployTritonModel(const std::string& path, const std::string& serverArg,
                                                       const std::string& configJson,
                                                       const std::string& deviceArg,
                                                       const OutputSelection& selection,
                                                       std::chrono::microseconds timeout,
                                                       const ptr<df::Signal>& available,
                                                       ModelWrapper& model)
{
    std::string address = serverArg.rfind("triton://", 0) == 0 ? serverArg.substr(9) : serverArg;
    if (address.empty() || address.find('/') != std::string::npos)
        throw std::invalid_argument("Model.init: server='" + serverArg +
                                    "' is not host[:port] or triton://host[:port]");
    address = nn::normalizeAddress(address);
    rememberTritonErrorTypes(VM::instance());
    const std::string what = "Model.init: deploying '" + path + "' to " + address;

    // Which files the model is, and what it is called by their content.
    nn::ModelFileSet files;
    std::string name;
    {
        SimpleMarkSweepGC::GCBlockedScope blocked;
        files = nn::modelFiles(path);
        name = nn::deploymentName(files, configJson);
    }

    auto endpoint = nn::TritonEndpoint::get(address);
    ModelServerInfo server;
    auto status = queryTritonServer(*endpoint, server);
    if (!status.ok())
        throw TypedNnError(grpcExceptionTypeName(status.error_code()),
                           what + ": " + status.error_message() + " [" +
                           grpcStatusCodeName(status.error_code()) + "]");
    if (std::find(server.extensions.begin(), server.extensions.end(), "model_repository") == server.extensions.end())
        throw TypedNnError("ProgramException", what + ": the server (" + server.name + " " + server.version +
                                               ") does not load models on request");

    ModelDeployment deployment { address, name, "reused", "" };
    const auto repository = sharedRepositoryFor(address);

    // Served already (an earlier run, another program): use it as it is.
    roxal::triton::ModelReadyRequest readyRequest;
    readyRequest.set_name(name);
    roxal::triton::ModelReadyResponse ready;
    status = endpoint->call("ModelReady", readyRequest, ready, std::chrono::seconds(5));
    if (!(status.ok() && ready.ready())) {
        if (repository) {
            verifySharedRepository(*endpoint, address, *repository, what);
            const auto key = std::make_pair(address, repository->string());
            bool symlinked = false;
            {
                SimpleMarkSweepGC::GCBlockedScope blocked;
                symlinked = nn::placeInRepository(files, *repository, name, !symlinksFailed(key));
            }
            status = loadOnServer(*endpoint, name, configJson, nullptr);
            if (!status.ok() && symlinked) {
                // The server may not see the files at the paths they link to.
                {
                    SimpleMarkSweepGC::GCBlockedScope blocked;
                    nn::copyOverSymlinks(*repository, name);
                }
                symlinksFailed(key, /*note=*/true);
                status = loadOnServer(*endpoint, name, configJson, nullptr);
            }
            deployment.transport = "repository";
        } else {
            if (files.totalBytes > nn::maxUploadBytes)
                throw TypedNnError("ProgramException",
                    what + ": its files total " + std::to_string(files.totalBytes) + " bytes, more than "
                    "one request to the server can carry (2 GB).  Share a directory the server serves as a "
                    "model repository (ai.nn.use_repository, or ROXAL_TRITON_REPOSITORIES), or put the "
                    "model in the server's repository yourself and load it as 'triton://" + address +
                    "/<its name>'");
            status = loadOnServer(*endpoint, name, configJson, &files);
            deployment.transport = "upload";
        }
        if (!status.ok())
            throw repositoryFailure(what, address, status);
    }
    if (repository && std::filesystem::exists(*repository / name))
        deployment.repository = repository->string();

    return openTritonSession(TritonSource { address, name, "" }, deviceArg, selection, timeout, available,
                             model, std::move(deployment));
}

// Model(..., timeout=): how long a Triton inference may take -- a time
// quantity (200ms) or seconds; 1 s by default.  Past it the call fails as
// unavailable, and a node holds its last outputs.
static std::chrono::microseconds tritonTimeoutArg(const Value& arg)
{
    if (arg.isNil())
        return std::chrono::seconds(1);
    std::optional<double> seconds;
    if (arg.isNumber())
        seconds = arg.isReal() ? arg.asReal() : static_cast<double>(arg.asInt());
    else
        seconds = sysTimeQuantitySeconds(arg, "Model.init timeout");
    if (!seconds)
        throw std::invalid_argument("Model.init: timeout must be a time quantity (e.g. 200ms) or seconds");
    if (!(*seconds > 0))
        throw std::invalid_argument("Model.init: timeout must be positive");
    return std::chrono::microseconds(static_cast<int64_t>(*seconds * 1e6));
}

// VM shutdown: every call in flight settled, the endpoints' threads joined,
// and then the exception types let go while the VM can still release them.
static void shutdownTriton()
{
    nn::TritonEndpoint::shutdownAll();
    std::lock_guard<std::mutex> lock(tritonErrorTypes->mutex);
    tritonErrorTypes->types.clear();
}
#endif // ROXAL_ENABLE_TRITON

#ifdef __EMSCRIPTEN__
// The host provider's feeds for a set of named inputs.  The encoder copies
// the input bytes into the request, so no lifetime coupling with the tensors.
static std::vector<web::NnFeed> webFeeds(
    const std::vector<std::pair<std::string, ObjTensor*>>& inputTensors)
{
    std::vector<web::NnFeed> feeds;
    feeds.reserve(inputTensors.size());
    for (auto& [name, tensor] : inputTensors) {
        web::NnFeed f;
        f.name  = name;
        f.dtype = to_string(tensor->dtype());
        f.shape = tensor->shape();
        f.data  = tensor->rawData();
        f.len   = static_cast<size_t>(tensor->numel()) * tensorDTypeSize(tensor->dtype());
        feeds.push_back(std::move(f));
    }
    return feeds;
}

// The host's NN provider (onnxruntime-web in the browser, onnxruntime-node
// under Electron), reached through the JS bridge.  The provider itself is
// asynchronous, so there is no worker thread.
class WebSession : public ModelSession {
public:
    explicit WebSession(uint32_t id) : id_(id) {}

    // nnRun's future plugs into the same machinery the native worker's
    // future does (FuncNode yield, resolveArgMask, resolve-on-index).  The
    // bridge registry pins heldValues until the reply lands.
    Value submit(const NamedInputs& inputs, std::vector<Value> heldValues) override {
        return web::nnRun(id_, webFeeds(inputs), std::move(heldValues));
    }

    void close() override {
        if (open_) {
            web::nnClose(id_);
            open_ = false;
        }
    }

private:
    uint32_t id_;           // in the host provider's table
    bool open_ = true;
};

static std::unique_ptr<ModelSession> openWebSession(const std::string& path,
                                                    const std::string& deviceArg,
                                                    bool doWarmup,
                                                    const OutputSelection& selection,
                                                    ModelWrapper& model)
{
    // The provider's reply carries no output names to select by.
    if (selection)
        throw std::invalid_argument("Model.init: outputs= is not supported in the web build");

    // 'webgpu' is a first-class device here; 'cuda' is not a thing a browser
    // has.
    if (deviceArg != "auto" && deviceArg != "cpu" && deviceArg != "webgpu")
        throw std::invalid_argument(
            "Model.init: device must be 'auto', 'cpu', or 'webgpu' in the web build");

    // The weights are read in one piece into a buffer of the file's size
    // (growing one byte by byte peaked at about three times a model's size,
    // and wasm memory never shrinks back), handed to the provider, and freed
    // before the wait: only the request in flight holds them after that.
    Value created;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            throw std::invalid_argument("Model.init: cannot open '" + path + "'");
        const std::streamoff fileSize = in.tellg();
        if (fileSize < 0)
            throw std::invalid_argument("Model.init: cannot size '" + path + "'");
        std::vector<uint8_t> modelBytes(static_cast<size_t>(fileSize));
        in.seekg(0, std::ios::beg);
        if (fileSize > 0 && !in.read(reinterpret_cast<char*>(modelBytes.data()), fileSize))
            throw std::invalid_argument("Model.init: cannot read '" + path + "'");
        created = web::nnCreate(std::move(modelBytes), deviceArg);
    }

    // Block until the provider compiled the model. Native init blocks the
    // same way inside Ort::Session construction; a WebGPU shader compile can
    // take a while on first load, hence the generous ceiling.
    Value meta = web::nnAwaitBlocking(created, 120000);
    if (!isDict(meta))
        throw std::invalid_argument("Model.init: malformed provider reply");
    ObjDict* md = asDict(meta);
    auto metaGet = [&](const char* key) {
        const Value k = Value::stringVal(toUnicodeString(key));
        if (!md->contains(k))
            throw std::invalid_argument(
                std::string("Model.init: provider reply missing '") + key + "'");
        return md->at(k);
    };

    auto session = std::make_unique<WebSession>(static_cast<uint32_t>(metaGet("id").asInt()));
    {
        const Value dv = metaGet("device");
        model.device = isString(dv) ? toUTF8StdString(asStringObj(dv)->s) : "wasm";
    }
    auto fillIO = [&](const char* key, std::vector<TensorSpec>& specs) {
        const Value lv = metaGet(key);
        if (!isList(lv)) return;
        ObjList* l = asList(lv);
        for (int i = 0; i < l->length(); ++i) {
            const Value ev = l->getElement(i);
            if (!isDict(ev)) continue;
            ObjDict* ed = asDict(ev);
            auto want = [&](const char* k) {
                return ed->at(Value::stringVal(toUnicodeString(k)));
            };
            TensorSpec spec;
            const Value nv = want("name");
            spec.name = isString(nv) ? toUTF8StdString(asStringObj(nv)->s) : "";
            const Value sv = want("shape");
            if (isList(sv)) {
                ObjList* sl = asList(sv);
                for (int d = 0; d < sl->length(); ++d) {
                    const Value dim = sl->getElement(d);
                    spec.shape.push_back(dim.isInt() ? dim.asInt()
                                                     : static_cast<int64_t>(dim.asReal()));
                }
            }
            const Value tv = want("dtype");
            spec.dtype = isString(tv) ? toUTF8StdString(asStringObj(tv)->s) : "float32";
            specs.push_back(std::move(spec));
        }
    };
    fillIO("inputs",  model.inputs);
    fillIO("outputs", model.outputs);

    // The provider's run is a future; await it and ignore failure -- warmup
    // is an optimization, not a contract.
    if (doWarmup) {
        NamedInputs inputs;
        const Value tensors = warmupInputs(model, inputs);
        if (!tensors.isNil()) {
            try {
                web::nnAwaitBlocking(session->submit(inputs, {}), 120000);
            } catch (const std::exception&) {
                // non-fatal
            }
        }
    }
    return session;
}
#endif

static std::string joinNames(const std::vector<TensorSpec>& specs)
{
    std::string s;
    for (size_t i = 0; i < specs.size(); ++i)
        s += (i > 0 ? ", " : "") + specs[i].name;
    return s;
}

// The named inputs of one predict, checked against the model's: a tensor
// (single-input models), a dict by input name, or a list in input order.
// Every input must be given exactly once, with the model's dtype and a
// compatible shape -- anything else is an invalid_argument, raised to the
// script before an inference starts.  The result is in the model's input
// order; the argument and its tensors are added to heldValues.
static NamedInputs parsePredictInputs(const ModelWrapper& model, const Value& arg,
                                      std::vector<Value>& heldValues)
{
    const auto& specs = model.inputs;
    std::vector<ObjTensor*> bound(specs.size(), nullptr);   // per model input
    heldValues.push_back(arg);

    if (isTensor(arg)) {
        if (specs.size() != 1)
            throw std::invalid_argument(
                "ai.nn: the model has " + std::to_string(specs.size()) + " inputs (" +
                joinNames(specs) + "): pass a dict by name, or a list in that order");
        bound[0] = asTensor(arg);
    }
    else if (isDict(arg)) {
        for (auto& [key, val] : asDict(arg)->items()) {
            if (!isString(key))
                throw std::invalid_argument("ai.nn: predict dict keys must be strings");
            const std::string name = toUTF8StdString(asStringObj(key)->s);
            if (!isTensor(val))
                throw std::invalid_argument(
                    "ai.nn: predict dict value for '" + name + "' is not a tensor");
            auto it = std::find_if(specs.begin(), specs.end(),
                                   [&](const TensorSpec& spec) { return spec.name == name; });
            if (it == specs.end())
                throw std::invalid_argument(
                    "ai.nn: the model has no input '" + name + "' (its inputs: " +
                    joinNames(specs) + ")");
            bound[it - specs.begin()] = asTensor(val);
            heldValues.push_back(val);
        }
    }
    else if (isList(arg)) {
        ObjList* list = asList(arg);
        if (static_cast<size_t>(list->length()) != specs.size()) {
            throw std::invalid_argument(
                "ai.nn: predict list has " + std::to_string(list->length()) +
                " elements but model expects " + std::to_string(specs.size()) +
                " inputs");
        }
        for (size_t i = 0; i < specs.size(); ++i) {
            Value elem = list->getElement(i);
            if (!isTensor(elem))
                throw std::invalid_argument(
                    "ai.nn: predict list element " + std::to_string(i) + " is not a tensor");
            bound[i] = asTensor(elem);
            heldValues.push_back(elem);
        }
    }
    else {
        throw std::invalid_argument(
            "ai.nn: predict expects a tensor, dict of tensors, or list of tensors");
    }

    NamedInputs inputs;
    inputs.reserve(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
        const TensorSpec& spec = specs[i];
        ObjTensor* tensor = bound[i];
        if (!tensor)
            throw std::invalid_argument("ai.nn: missing input '" + spec.name + "'");
        const std::string dtype = to_string(tensor->dtype());
        if (dtype != spec.dtype)
            throw std::invalid_argument(
                "ai.nn: dtype mismatch for input '" + spec.name + "': model expects " +
                spec.dtype + " but got " + dtype);
        if (!isShapeCompatible(tensor->shape(), spec.shape))
            throw std::invalid_argument(
                "ai.nn: shape mismatch for input '" + spec.name +
                "': model expects " + shapeToString(spec.shape) +
                " but got " + shapeToString(tensor->shape()));
        inputs.emplace_back(spec.name, tensor);
    }
    return inputs;
}

// predict: checks the argument on the calling thread, so its errors are
// catchable by Roxal try/except, and starts the inference on the model's
// backend, returning its future.
static Value safeRunInferenceFromValueAsync(VM& vm,
                                            const std::shared_ptr<ModelWrapper>& wrapper,
                                            const Value& arg) {
    try {
        if (wrapper->closed)
            throw std::invalid_argument("ai.nn: Model session is closed");
        std::vector<Value> heldValues;   // keep the inputs alive during async inference
        auto inputs = parsePredictInputs(*wrapper, arg, heldValues);
        return wrapper->session->submit(inputs, std::move(heldValues));
    } catch (const std::invalid_argument& e) {
        Value msg = Value::stringVal(toUnicodeString(e.what()));
        vm.raiseException(Value::exceptionVal(msg));
        return Value::nilVal();
    }
}

// ============================================================
// ModuleNN lifecycle
// ============================================================

ModuleNN::ModuleNN()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("nn")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleNN::~ModuleNN()
{
    destroyModuleType(moduleTypeValue);
}

#ifdef __EMSCRIPTEN__
void ModuleNN::onModuleLoaded(VM& vm)
{
    // Provider replies only reach the VM through the host loop's inbound
    // drain; without it, awaiting a predict future would block forever.
    web::installHostLoop(vm);
}
#endif

void ModuleNN::registerBuiltins(VM& vm)
{
    setVM(vm);

    // Module-level functions
    link("tensor_device", [this](VM&, ArgsView a) { return nn_tensor_device_builtin(a); },
         /*defaults=*/{}, /*resolveArgMask=*/0x1);
    link("memory_info", [this](VM&, ArgsView a) { return nn_memory_info_builtin(a); });
    link("server_info", [this](VM&, ArgsView a) { return nn_server_info_builtin(a); });
    link("use_repository", [this](VM&, ArgsView a) { return nn_use_repository_builtin(a); });

    // Model object methods
    linkMethod("Model", "init", [this](VM&, ArgsView a) { return nn_model_init_builtin(a); });
    // predict is declared here for help()/docstring visibility; the instance property
    // closure (set in Model.init) shadows this for actual calls and signal integration.
    linkMethod("Model", "predict", [](VM& vm, ArgsView a) -> Value {
        if (a.size() < 2 || !isObjectInstance(a[0]))
            throw std::invalid_argument("predict expects an input argument");
        return safeRunInferenceFromValueAsync(vm, getModelWrapperShared(asObjectInstance(a[0])), a[1]);
    }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Model", "inputs",  [this](VM&, ArgsView a) { return nn_model_inputs_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Model", "outputs", [this](VM&, ArgsView a) { return nn_model_outputs_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Model", "device",  [this](VM&, ArgsView a) { return nn_model_device_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Model", "server_info", [this](VM&, ArgsView a) { return nn_model_server_info_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Model", "deployment", [this](VM&, ArgsView a) { return nn_model_deployment_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Model", "unload", [this](VM&, ArgsView a) { return nn_model_unload_builtin(a); });
    linkMethod("Model", "close",   [this](VM&, ArgsView a) { return nn_model_close_builtin(a); });

    // Tokenizer object methods
    linkMethod("Tokenizer", "init",           [this](VM&, ArgsView a) { return nn_tokenizer_init_builtin(a); });
    linkMethod("Tokenizer", "encode",         [this](VM&, ArgsView a) { return nn_tokenizer_encode_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Tokenizer", "decode",         [this](VM&, ArgsView a) { return nn_tokenizer_decode_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Tokenizer", "vocab_size",     [this](VM&, ArgsView a) { return nn_tokenizer_vocab_size_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Tokenizer", "special_tokens", [this](VM&, ArgsView a) { return nn_tokenizer_special_tokens_builtin(a); }, {}, 0, /*noMutateSelf=*/true);
    linkMethod("Tokenizer", "close",          [this](VM&, ArgsView a) { return nn_tokenizer_close_builtin(a); });
}

// ============================================================
// Model.init(path, device, warmup)
// ============================================================

Value ModuleNN::nn_model_init_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.init expects receiver");
    if (!isString(args[1]))
        throw std::invalid_argument("Model.init expects a model path string");

    ObjectInstance* inst = asObjectInstance(args[0]);
    std::string path = toUTF8StdString(asStringObj(args[1])->s);
    std::string deviceArg = (args.size() >= 3 && isString(args[2]))
        ? toUTF8StdString(asStringObj(args[2])->s) : "auto";
    bool doWarmup = (args.size() < 4) || args[3].asBool();
    const OutputSelection selection =
        outputSelectionArg(args.size() >= 5 ? args[4] : Value::nilVal());
    const Value timeoutArg = args.size() >= 6 ? args[5] : Value::nilVal();
    const Value serverArg = args.size() >= 7 ? args[6] : Value::nilVal();
    const Value configArg = args.size() >= 8 ? args[7] : Value::nilVal();
    if (!serverArg.isNil() && !isString(serverArg))
        throw std::invalid_argument("Model.init: server must be a string, 'host[:port]'");
    if (!configArg.isNil() && serverArg.isNil())
        throw std::invalid_argument("Model.init: config applies to a model deployed with server= only");
    if (!configArg.isNil() && !isString(configArg) && !isDict(configArg))
        throw std::invalid_argument("Model.init: config must be a dict (or its JSON text)");

#if defined(ROXAL_ENABLE_ONNX) || defined(__EMSCRIPTEN__)
    auto wrapper = std::make_shared<ModelWrapper>();
    // Whether the model's server answers (see noteAvailability); a model run
    // in this process is always available.
    auto available = df::Signal::newSourceSignal(0, Value::boolVal(true), std::string("available"));
#ifdef ROXAL_ENABLE_TRITON
    if (!serverArg.isNil()) {
        if (path.rfind("triton://", 0) == 0)
            throw std::invalid_argument("Model.init: server= deploys a model file; '" + path +
                                        "' names a model its server already has");
        const std::string configJson = configArg.isNil() ? std::string()
            : isString(configArg) ? toUTF8StdString(asStringObj(configArg)->s) : sysToJson(configArg);
        try {
            wrapper->session = deployTritonModel(path, toUTF8StdString(asStringObj(serverArg)->s), configJson,
                                                 deviceArg, selection, tritonTimeoutArg(timeoutArg),
                                                 available, *wrapper);
        } catch (const TypedNnError& e) {
            const Value type = vm().loadGlobal(toUnicodeString(e.typeName)).value_or(Value::nilVal());
            vm().raiseException(Value::exceptionVal(Value::stringVal(toUnicodeString(e.what())), type));
            return Value::nilVal();
        }
    } else if (auto source = parseTritonSource(path)) {
        try {
            wrapper->session = openTritonSession(*source, deviceArg, selection,
                                                 tritonTimeoutArg(timeoutArg), available, *wrapper);
        } catch (const TypedNnError& e) {
            const Value type = vm().loadGlobal(toUnicodeString(e.typeName)).value_or(Value::nilVal());
            vm().raiseException(Value::exceptionVal(Value::stringVal(toUnicodeString(e.what())), type));
            return Value::nilVal();
        }
    } else
#endif
    {
        if (path.rfind("triton://", 0) == 0 || !serverArg.isNil())
            throw std::invalid_argument("Model.init: this build has no Triton support "
                                        "(ROXAL_ENABLE_TRITON)");
        if (!timeoutArg.isNil())
            throw std::invalid_argument("Model.init: timeout applies to Triton models only");
#ifdef ROXAL_ENABLE_ONNX
        wrapper->session = openOrtSession(path, deviceArg, doWarmup, selection, *wrapper);
#else
        wrapper->session = openWebSession(path, deviceArg, doWarmup, selection, *wrapper);
#endif
    }
    inst->setProperty("available", Value::signalVal(available));

    // Store wrapper as foreignPtr on the instance
    auto* sharedWrapper = new std::shared_ptr<ModelWrapper>(wrapper);
    Value fp = Value::foreignPtrVal(sharedWrapper);
    asForeignPtr(fp)->registerCleanup([](void* p) {
        delete static_cast<std::shared_ptr<ModelWrapper>*>(p);
    });
    inst->setProperty("_this", fp);

    // Create the `predict` closure property.
    // This is a standalone closure (not a method) that captures the model wrapper
    // and can be used in dataflow signal expressions like any other func.
    // predict returns a future (async inference on worker thread).
    std::shared_ptr<ModelWrapper> capturedWrapper = wrapper;
    NativeFn predictFn = [capturedWrapper](VM& vm, ArgsView a) -> Value {
        if (a.size() < 1)
            throw std::invalid_argument("predict expects an input argument");
        return safeRunInferenceFromValueAsync(vm, capturedWrapper, a[0]);
    };

    auto funcObj = newFunctionObj(
        toUnicodeString("predict"),
        toUnicodeString("ai"),
        toUnicodeString("nn"),
        toUnicodeString("<native>")
    );
    ObjFunction* func = funcObj.get();
    func->arity = 1;
    func->upvalueCount = 0;
    func->builtinInfo = make_ptr<BuiltinFuncInfo>(predictFn, std::vector<Value>{}, 0x1);
    func->doc = toUnicodeString(
        "Run inference. Input may be a tensor, a dict {name: tensor}, or a list "
        "of tensors. Returns output tensor (or list if multiple outputs). "
        "Also works with signals for reactive dataflow: a signal input yields "
        "a derived signal per model output.");
    // The signature: one untyped input, and one tensor return PER MODEL
    // OUTPUT.  An ONNX graph fixes its output count, and it is known here, so
    // predict carries the same '-> [tensor, ..]' declaration a Roxal func
    // would write by hand: a plain call returns the outputs as a list (as it
    // always did), and a lifted call mints one output signal per model output
    // -- 'var [logits, boxes] = model.predict(sig)' -- instead of a single
    // list-valued signal.  The declaration decides the arity, not the use
    // site, so the two forms agree with the rest of the language (see
    // "Nodes with several outputs" in roxal-for-devs.md).
    const size_t outputCount = wrapper->outputs.empty() ? 1 : wrapper->outputs.size();
    func->funcType = makeFuncType({{"input", std::nullopt}}, {}, /*isProc=*/false,
                                  std::vector<type::BuiltinType>(outputCount, type::BuiltinType::Tensor));

    Value funcVal = Value::objVal(std::move(funcObj));
    Value closureVal = Value::objVal(newClosureObj(funcVal));
    inst->setProperty("predict", closureVal);

    return Value::nilVal();
#else
    (void)inst; (void)path; (void)doWarmup; (void)selection; (void)timeoutArg; (void)serverArg;
    throw std::runtime_error("Model.init requires ONNX Runtime (build with -DROXAL_ENABLE_ONNX=ON)");
#endif
}

// ============================================================
// Model.inputs() → list of {name, shape, dtype}
// ============================================================

static Value buildIODescriptorList(const std::vector<TensorSpec>& specs)
{
    Value list = Value::objVal(newListObj());
    for (const auto& spec : specs) {
        Value dict = Value::objVal(newDictObj());
        asDict(dict)->store(
            Value::stringVal(toUnicodeString("name")),
            Value::stringVal(toUnicodeString(spec.name))
        );

        Value shapeList = Value::objVal(newListObj());
        for (auto dim : spec.shape)
            asList(shapeList)->append(Value::intVal(dim));
        asDict(dict)->store(
            Value::stringVal(toUnicodeString("shape")),
            shapeList
        );

        asDict(dict)->store(
            Value::stringVal(toUnicodeString("dtype")),
            Value::stringVal(toUnicodeString(spec.dtype))
        );

        asList(list)->append(dict);
    }
    return list;
}

Value ModuleNN::nn_model_inputs_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.inputs expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    return buildIODescriptorList(wrapper->inputs);
}

// ============================================================
// Model.outputs() → list of {name, shape, dtype}
// ============================================================

Value ModuleNN::nn_model_outputs_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.outputs expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    return buildIODescriptorList(wrapper->outputs);
}

// ============================================================
// Model.device() → string
// ============================================================

Value ModuleNN::nn_model_device_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.device expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    return Value::stringVal(toUnicodeString(wrapper->device));
}

// {kind, name, version, extensions}: see ModelServerInfo.
static Value serverInfoValue(const ModelServerInfo& info)
{
    Value extensions = Value::listVal();
    for (const auto& extension : info.extensions)
        asList(extensions)->append(Value::stringVal(toUnicodeString(extension)));
    Value dict = Value::dictVal();
    asDict(dict)->store(Value::stringVal(toUnicodeString("kind")), Value::stringVal(toUnicodeString(info.kind)));
    asDict(dict)->store(Value::stringVal(toUnicodeString("name")), Value::stringVal(toUnicodeString(info.name)));
    asDict(dict)->store(Value::stringVal(toUnicodeString("version")), Value::stringVal(toUnicodeString(info.version)));
    asDict(dict)->store(Value::stringVal(toUnicodeString("extensions")), extensions);
    return dict;
}

// ============================================================
// Model.server_info() → dict | nil: the server a Triton model was opened on
// ============================================================

Value ModuleNN::nn_model_server_info_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.server_info expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    if (!wrapper->session)
        return Value::nilVal();
    const auto server = wrapper->session->server();
    return server ? serverInfoValue(*server) : Value::nilVal();
}

// ============================================================
// Model.deployment() → dict | nil; Model.unload(remove=false)
// ============================================================

Value ModuleNN::nn_model_deployment_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.deployment expects receiver");
    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    const auto deployment = wrapper->session ? wrapper->session->deployment() : std::nullopt;
    if (!deployment)
        return Value::nilVal();
    auto str = [](const std::string& s) { return Value::stringVal(toUnicodeString(s)); };
    Value dict = Value::dictVal();
    asDict(dict)->store(str("server"), str(deployment->server));
    asDict(dict)->store(str("name"), str(deployment->name));
    asDict(dict)->store(str("transport"), str(deployment->transport));
    asDict(dict)->store(str("repository"),
                        deployment->repository.empty() ? Value::nilVal() : str(deployment->repository));
    return dict;
}

Value ModuleNN::nn_model_unload_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.unload expects receiver");
    const bool removeFiles = args.size() >= 2 && !args[1].isNil() && args[1].asBool();
    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    if (!wrapper->session)
        throw std::invalid_argument("Model.unload: the model is not open");
#ifdef ROXAL_ENABLE_TRITON
    try {
        wrapper->session->unload(removeFiles);
    } catch (const TypedNnError& e) {
        const Value type = vm().loadGlobal(toUnicodeString(e.typeName)).value_or(Value::nilVal());
        vm().raiseException(Value::exceptionVal(Value::stringVal(toUnicodeString(e.what())), type));
        return Value::nilVal();
    }
#else
    wrapper->session->unload(removeFiles);
#endif
    // Unloaded, it serves nothing more: closed, as close() leaves it.
    if (!wrapper->closed) {
        wrapper->closed = true;
        wrapper->session->close();
    }
    return Value::nilVal();
}

// ============================================================
// ai.nn.use_repository(server, directory): a shared repository, for this run
// ============================================================

Value ModuleNN::nn_use_repository_builtin(ArgsView args)
{
    if (args.size() < 2 || !isString(args[0]) || !(isString(args[1]) || args[1].isNil()))
        throw std::invalid_argument("use_repository expects a server string and a directory string (or nil)");
#ifdef ROXAL_ENABLE_TRITON
    const std::string given = toUTF8StdString(asStringObj(args[0])->s);
    std::string address = given.rfind("triton://", 0) == 0 ? given.substr(9) : given;
    if (address.empty() || address.find('/') != std::string::npos)
        throw std::invalid_argument("use_repository: '" + given + "' is not host[:port] or triton://host[:port]");
    address = nn::normalizeAddress(address);
    auto& script = scriptRepositories();
    if (args[1].isNil()) {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.byServer.erase(address);
    } else {
        const auto directory = nn::repositoryDirectory(toUTF8StdString(asStringObj(args[1])->s));
        std::lock_guard<std::mutex> lock(script.mutex);
        script.byServer[address] = directory;
    }
    return Value::nilVal();
#else
    throw std::invalid_argument("use_repository: this build has no Triton support (ROXAL_ENABLE_TRITON)");
#endif
}

// ============================================================
// ai.nn.server_info('host[:port]') → dict: ask a Triton server who it is
// ============================================================

Value ModuleNN::nn_server_info_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0]))
        throw std::invalid_argument("server_info expects an address string");
    const std::string given = toUTF8StdString(asStringObj(args[0])->s);
#ifdef ROXAL_ENABLE_TRITON
    static const std::string scheme = "triton://";
    std::string address = given.rfind(scheme, 0) == 0 ? given.substr(scheme.size()) : given;
    if (address.empty() || address.find('/') != std::string::npos)
        throw std::invalid_argument("server_info: '" + given +
                                    "' is not host[:port] or triton://host[:port]");
    address = nn::normalizeAddress(address);
    ModelServerInfo info;
    const auto status = queryTritonServer(*nn::TritonEndpoint::get(address), info);
    if (!status.ok()) {
        const Value type = vm().loadGlobal(toUnicodeString(grpcExceptionTypeName(status.error_code())))
                               .value_or(Value::nilVal());
        vm().raiseException(Value::exceptionVal(Value::stringVal(toUnicodeString(
            "server_info: " + address + ": " + status.error_message() +
            " [" + grpcStatusCodeName(status.error_code()) + "]")), type));
        return Value::nilVal();
    }
    return serverInfoValue(info);
#else
    throw std::invalid_argument("server_info('" + given + "'): this build has no Triton support "
                                "(ROXAL_ENABLE_TRITON)");
#endif
}

// ============================================================
// ai.nn.tensor_device(t) → string
// ============================================================

Value ModuleNN::nn_tensor_device_builtin(ArgsView args)
{
    if (args.size() < 1 || !isTensor(args[0]))
        throw std::invalid_argument("ai.nn.tensor_device expects a tensor argument");

#ifdef ROXAL_ENABLE_ONNX
    ObjTensor* t = asTensor(args[0]);
    if (t->isOrtBacked() && t->isOnGpu())
        return Value::stringVal(toUnicodeString("cuda"));
#endif
    return Value::stringVal(toUnicodeString("cpu"));
}

// ============================================================
// ai.nn.memory_info(device='auto') → {device, total, free, used}
// ============================================================

Value ModuleNN::nn_memory_info_builtin(ArgsView args)
{
    std::string deviceArg = (args.size() >= 1 && isString(args[0]))
        ? toUTF8StdString(asStringObj(args[0])->s) : "auto";

    if (deviceArg != "auto" && deviceArg != "cpu" && deviceArg != "cuda")
        throw std::invalid_argument("ai.nn.memory_info: device must be 'auto', 'cpu', or 'cuda'");

    Value dict = Value::objVal(newDictObj());

#ifdef ROXAL_ENABLE_ONNX
    if (deviceArg != "cpu") {
        auto& ortEnv = OnnxEnvironment::instance();
        auto& cuda = CudaRuntime::instance();
        if (ortEnv.cudaAvailable() && cuda.available()) {
            size_t free = 0, total = 0;
            if (cuda.memGetInfo(&free, &total) == 0) {
                asDict(dict)->store(Value::stringVal(toUnicodeString("device")),
                                    Value::stringVal(toUnicodeString("cuda")));
                asDict(dict)->store(Value::stringVal(toUnicodeString("total")),
                                    Value::realVal(static_cast<double>(total)));
                asDict(dict)->store(Value::stringVal(toUnicodeString("free")),
                                    Value::realVal(static_cast<double>(free)));
                asDict(dict)->store(Value::stringVal(toUnicodeString("used")),
                                    Value::realVal(static_cast<double>(total - free)));
                return dict;
            }
        }
        if (deviceArg == "cuda")
            throw std::invalid_argument("ai.nn.memory_info: CUDA not available");
    }
#else
    if (deviceArg == "cuda")
        throw std::invalid_argument("ai.nn.memory_info: CUDA not available in this build");
#endif

    asDict(dict)->store(Value::stringVal(toUnicodeString("device")),
                        Value::stringVal(toUnicodeString("cpu")));

    // Report system RAM (Linux only; other platforms return 0)
#ifdef __linux__
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        double total = static_cast<double>(si.totalram) * si.mem_unit;
        double free  = static_cast<double>(si.freeram)  * si.mem_unit;
        asDict(dict)->store(Value::stringVal(toUnicodeString("total")),
                            Value::realVal(total));
        asDict(dict)->store(Value::stringVal(toUnicodeString("free")),
                            Value::realVal(free));
        asDict(dict)->store(Value::stringVal(toUnicodeString("used")),
                            Value::realVal(total - free));
    } else
#endif
    {
        asDict(dict)->store(Value::stringVal(toUnicodeString("total")),
                            Value::realVal(0));
        asDict(dict)->store(Value::stringVal(toUnicodeString("free")),
                            Value::realVal(0));
        asDict(dict)->store(Value::stringVal(toUnicodeString("used")),
                            Value::realVal(0));
    }
    return dict;
}

// ============================================================
// Model.close()
// ============================================================

Value ModuleNN::nn_model_close_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.close expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    if (!wrapper->closed) {
        wrapper->closed = true;
        // Queued predicts resolve to an error now; nothing here waits for an
        // inference in flight.
        if (wrapper->session)
            wrapper->session->close();
    }
    return Value::nilVal();
}

// ============================================================
// TokenizerWrapper — per-tokenizer native state (no ONNX dependency)
// ============================================================

struct TokenizerWrapper {
    // Vocab: token string → ID, and reverse
    std::unordered_map<std::string, int> vocab;
    std::unordered_map<int, std::string> id_to_token;
    // BPE merge priority: "left right" → rank (lower = higher priority)
    std::unordered_map<std::string, int> merge_rank;

    // Added/special tokens
    struct AddedToken {
        int id;
        std::string content;
        bool special, lstrip, rstrip;
    };
    std::vector<AddedToken> added_tokens;
    std::vector<AddedToken> added_tokens_sorted; // sorted by content length descending

    // Byte fallback tables
    std::unordered_map<uint8_t, int> byte_to_token_id;
    std::unordered_map<std::string, uint8_t> byte_token_to_byte;
    bool byte_fallback_enabled = false;

    // Normalizer pipeline
    struct NormalizerStep {
        enum Type { Prepend, Replace, Lowercase, Strip, NFC, NFKC, NFD, NFKD } type;
        std::string arg1, arg2;
    };
    std::vector<NormalizerStep> normalizer_steps;

    // Pre-tokenizer
    enum class PreTokenizerType { None, ByteLevel, Metaspace, Whitespace } pre_tokenizer_type = PreTokenizerType::None;
    std::unordered_map<uint8_t, std::string> byte_to_unicode;   // GPT-2 byte↔unicode
    std::unordered_map<std::string, uint8_t> unicode_to_byte;

    // Decoder pipeline
    struct DecoderStep {
        enum Type { Replace, ByteFallback, Fuse, Strip, Metaspace, ByteLevel } type;
        std::string arg1, arg2;
    };
    std::vector<DecoderStep> decoder_steps;

    // Model config
    std::string unk_token = "<unk>";
    int unk_token_id = -1;
    int vocab_size_count = 0;
    bool closed = false;
};

// ============================================================
// Tokenizer helpers
// ============================================================

static TokenizerWrapper* getTokenizerWrapper(ObjectInstance* inst) {
    Value fpVal = inst->getProperty("_this");
    if (fpVal.isNil() || !isForeignPtr(fpVal))
        throw std::runtime_error("Tokenizer not properly initialized");
    auto* sp = static_cast<std::shared_ptr<TokenizerWrapper>*>(asForeignPtr(fpVal)->ptr);
    return sp->get();
}

// Split a UTF-8 string into individual UTF-8 code point strings
static std::vector<std::string> utf8_chars(const std::string& s) {
    ustring us = toUnicodeString(s);
    std::vector<std::string> chars;
    int32_t i = 0;
    while (i < us.length()) {
        code_point cp = us.char32At(i);
        int32_t next = i + utf16_code_unit_count(cp);
        std::string ch;
        us.tempSubStringBetween(i, next).toUTF8String(ch);
        chars.push_back(std::move(ch));
        i = next;
    }
    return chars;
}

// Apply normalizer pipeline to text
static std::string applyNormalizer(const TokenizerWrapper& tw, const std::string& text) {
    std::string result = text;
    for (const auto& step : tw.normalizer_steps) {
        switch (step.type) {
            case TokenizerWrapper::NormalizerStep::Prepend:
                result = step.arg1 + result;
                break;
            case TokenizerWrapper::NormalizerStep::Replace: {
                std::string out;
                size_t pos = 0;
                while (pos < result.size()) {
                    size_t found = result.find(step.arg1, pos);
                    if (found == std::string::npos) {
                        out.append(result, pos, std::string::npos);
                        break;
                    }
                    out.append(result, pos, found - pos);
                    out.append(step.arg2);
                    pos = found + step.arg1.size();
                }
                result = out;
                break;
            }
            case TokenizerWrapper::NormalizerStep::Lowercase:
                std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                break;
            case TokenizerWrapper::NormalizerStep::Strip: {
                size_t start = result.find_first_not_of(" \t\n\r");
                size_t end = result.find_last_not_of(" \t\n\r");
                if (start == std::string::npos) result.clear();
                else result = result.substr(start, end - start + 1);
                break;
            }
            default:
                // NFC/NFKC/NFD/NFKD — skip for now (not used by Phi-3/Llama/GPT-2)
                break;
        }
    }
    return result;
}

// Build GPT-2 byte-to-unicode mapping table
static void initByteToUnicode(TokenizerWrapper& tw) {
    std::vector<int> bs;
    for (int i = 33; i <= 126; ++i) bs.push_back(i);
    for (int i = 161; i <= 172; ++i) bs.push_back(i);
    for (int i = 174; i <= 255; ++i) bs.push_back(i);
    std::vector<int> cs(bs.begin(), bs.end());
    int n = 0;
    for (int b = 0; b < 256; ++b) {
        if (std::find(bs.begin(), bs.end(), b) == bs.end()) {
            bs.push_back(b);
            cs.push_back(256 + n);
            ++n;
        }
    }
    for (size_t i = 0; i < bs.size(); ++i) {
        uint8_t byte_val = static_cast<uint8_t>(bs[i]);
        char32_t cp = static_cast<char32_t>(cs[i]);
        std::string utf8;
        if (cp < 0x80) {
            utf8 = std::string(1, static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        tw.byte_to_unicode[byte_val] = utf8;
        tw.unicode_to_byte[utf8] = byte_val;
    }
}

// BPE encode a single normalized text segment
static std::vector<int> bpe_encode_segment(const TokenizerWrapper& tw,
                                            const std::string& word) {
    auto chars = utf8_chars(word);
    if (chars.empty()) return {};

    std::vector<std::string> tokens(chars.begin(), chars.end());

    // Iteratively merge the best (lowest rank) pair
    while (tokens.size() > 1) {
        int best_rank = -1;
        int best_idx = -1;
        for (size_t j = 0; j < tokens.size() - 1; ++j) {
            std::string pair_key = tokens[j] + " " + tokens[j + 1];
            auto it = tw.merge_rank.find(pair_key);
            if (it != tw.merge_rank.end()) {
                if (best_rank < 0 || it->second < best_rank) {
                    best_rank = it->second;
                    best_idx = static_cast<int>(j);
                }
            }
        }
        if (best_idx < 0) break;

        std::vector<std::string> new_tokens;
        new_tokens.reserve(tokens.size() - 1);
        for (size_t k = 0; k < tokens.size(); ) {
            if (static_cast<int>(k) == best_idx) {
                new_tokens.push_back(tokens[k] + tokens[k + 1]);
                k += 2;
            } else {
                new_tokens.push_back(tokens[k]);
                k += 1;
            }
        }
        tokens = std::move(new_tokens);
    }

    // Map tokens to IDs with byte fallback
    std::vector<int> ids;
    for (const auto& tok : tokens) {
        auto it = tw.vocab.find(tok);
        if (it != tw.vocab.end()) {
            ids.push_back(it->second);
        } else if (tw.byte_fallback_enabled) {
            for (unsigned char byte : tok) {
                auto byte_it = tw.byte_to_token_id.find(byte);
                if (byte_it != tw.byte_to_token_id.end()) {
                    ids.push_back(byte_it->second);
                } else if (tw.unk_token_id >= 0) {
                    ids.push_back(tw.unk_token_id);
                }
            }
        } else if (tw.unk_token_id >= 0) {
            ids.push_back(tw.unk_token_id);
        }
    }
    return ids;
}

// Full encode: split on special tokens, normalize + BPE each text segment
static std::vector<int> tokenizer_encode(const TokenizerWrapper& tw,
                                          const std::string& text) {
    std::vector<int> all_ids;

    // Split text on added/special tokens (greedy, longest match first)
    struct Segment { bool is_special; std::string text; };
    std::vector<Segment> segments;
    size_t pos = 0;
    while (pos < text.size()) {
        bool found = false;
        for (const auto& at : tw.added_tokens_sorted) {
            if (pos + at.content.size() <= text.size() &&
                text.compare(pos, at.content.size(), at.content) == 0) {
                segments.push_back({true, at.content});
                pos += at.content.size();
                found = true;
                break;
            }
        }
        if (!found) {
            if (segments.empty() || segments.back().is_special) {
                segments.push_back({false, ""});
            }
            // Advance one UTF-8 code point
            unsigned char c = text[pos];
            size_t len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (pos + len > text.size()) len = 1;
            segments.back().text.append(text, pos, len);
            pos += len;
        }
    }

    // Process each segment
    for (const auto& seg : segments) {
        if (seg.is_special) {
            // Look up special token ID
            for (const auto& at : tw.added_tokens) {
                if (at.content == seg.text) {
                    all_ids.push_back(at.id);
                    break;
                }
            }
        } else {
            // Apply normalizer, then BPE
            std::string normalized = applyNormalizer(tw, seg.text);

            // For ByteLevel pre-tokenizer: map bytes to GPT-2 unicode chars
            if (tw.pre_tokenizer_type == TokenizerWrapper::PreTokenizerType::ByteLevel) {
                std::string mapped;
                for (unsigned char byte : normalized) {
                    auto it = tw.byte_to_unicode.find(byte);
                    if (it != tw.byte_to_unicode.end())
                        mapped += it->second;
                }
                normalized = mapped;
            }

            auto ids = bpe_encode_segment(tw, normalized);
            all_ids.insert(all_ids.end(), ids.begin(), ids.end());
        }
    }

    return all_ids;
}

// Full decode: id→token, apply decoder pipeline
static std::string tokenizer_decode(const TokenizerWrapper& tw,
                                     const std::vector<int>& ids) {
    // Step 1: Map IDs to token strings, skipping special tokens
    std::vector<std::string> tokens;
    tokens.reserve(ids.size());
    for (int id : ids) {
        // Skip special tokens during decode
        bool is_special = false;
        for (const auto& at : tw.added_tokens) {
            if (at.id == id && at.special) {
                is_special = true;
                break;
            }
        }
        if (is_special) continue;

        auto it = tw.id_to_token.find(id);
        if (it != tw.id_to_token.end()) {
            tokens.push_back(it->second);
        }
    }

    // Step 2: Apply decoder pipeline
    static const std::string METASPACE = "\xe2\x96\x81"; // U+2581 "▁"
    for (const auto& step : tw.decoder_steps) {
        switch (step.type) {
            case TokenizerWrapper::DecoderStep::ByteFallback: {
                for (auto& tok : tokens) {
                    auto it = tw.byte_token_to_byte.find(tok);
                    if (it != tw.byte_token_to_byte.end()) {
                        tok = std::string(1, static_cast<char>(it->second));
                    }
                }
                break;
            }
            case TokenizerWrapper::DecoderStep::Replace: {
                for (auto& tok : tokens) {
                    size_t p = 0;
                    while ((p = tok.find(step.arg1, p)) != std::string::npos) {
                        tok.replace(p, step.arg1.size(), step.arg2);
                        p += step.arg2.size();
                    }
                }
                break;
            }
            case TokenizerWrapper::DecoderStep::Fuse: {
                std::string fused;
                for (const auto& tok : tokens) fused += tok;
                tokens.clear();
                tokens.push_back(std::move(fused));
                break;
            }
            case TokenizerWrapper::DecoderStep::Strip: {
                if (!tokens.empty()) {
                    auto& first = tokens.front();
                    if (!first.empty() && first[0] == ' ')
                        first = first.substr(1);
                }
                break;
            }
            case TokenizerWrapper::DecoderStep::Metaspace: {
                for (auto& tok : tokens) {
                    size_t p = 0;
                    while ((p = tok.find(METASPACE, p)) != std::string::npos) {
                        tok.replace(p, METASPACE.size(), " ");
                        p += 1;
                    }
                }
                if (!tokens.empty()) {
                    auto& first = tokens.front();
                    if (!first.empty() && first[0] == ' ')
                        first = first.substr(1);
                }
                break;
            }
            case TokenizerWrapper::DecoderStep::ByteLevel: {
                for (auto& tok : tokens) {
                    auto chars = utf8_chars(tok);
                    std::string decoded;
                    for (const auto& ch : chars) {
                        auto it = tw.unicode_to_byte.find(ch);
                        if (it != tw.unicode_to_byte.end())
                            decoded.push_back(static_cast<char>(it->second));
                        else
                            decoded += ch;
                    }
                    tok = decoded;
                }
                break;
            }
        }
    }

    // Concatenate
    std::string result;
    for (const auto& tok : tokens) result += tok;
    return result;
}

// ============================================================
// nn_tokenizer_init_builtin — Tokenizer.init(path)
// ============================================================

Value ModuleNN::nn_tokenizer_init_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Tokenizer.init expects receiver");
    if (!isString(args[1]))
        throw std::invalid_argument("Tokenizer.init expects a path string");

    ObjectInstance* inst = asObjectInstance(args[0]);
    std::string path = toUTF8StdString(asStringObj(args[1])->s);

    // Read file
    std::ifstream ifs(path);
    if (!ifs.is_open())
        throw std::invalid_argument("ai.nn.load_tokenizer: cannot open '" + path + "'");
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string contents = buf.str();

    // Parse JSON
    std::string err;
    json11::Json root = json11::Json::parse(contents, err);
    if (!err.empty())
        throw std::invalid_argument("ai.nn.load_tokenizer: invalid JSON: " + err);

    auto wrapper = std::make_shared<TokenizerWrapper>();

    // --- Parse model ---
    const auto& model = root["model"];
    std::string model_type = model["type"].string_value();
    if (model_type != "BPE")
        throw std::invalid_argument("ai.nn.load_tokenizer: unsupported model type '" + model_type + "' (only BPE supported)");

    // Vocab
    const auto& vocab_obj = model["vocab"];
    if (vocab_obj.is_object()) {
        for (const auto& kv : vocab_obj.object_items()) {
            int id = kv.second.int_value();
            wrapper->vocab[kv.first] = id;
            wrapper->id_to_token[id] = kv.first;
        }
    }
    wrapper->vocab_size_count = static_cast<int>(wrapper->vocab.size());

    // Merges
    const auto& merges_arr = model["merges"];
    if (merges_arr.is_array()) {
        int rank = 0;
        for (const auto& m : merges_arr.array_items()) {
            wrapper->merge_rank[m.string_value()] = rank++;
        }
    }

    // Byte fallback
    wrapper->byte_fallback_enabled = model["byte_fallback"].bool_value();
    if (wrapper->byte_fallback_enabled) {
        const char* hex = "0123456789ABCDEF";
        for (int b = 0; b < 256; ++b) {
            std::string hex_token = "<0x";
            hex_token += hex[b >> 4];
            hex_token += hex[b & 0xF];
            hex_token += ">";
            auto it = wrapper->vocab.find(hex_token);
            if (it != wrapper->vocab.end()) {
                wrapper->byte_to_token_id[static_cast<uint8_t>(b)] = it->second;
                wrapper->byte_token_to_byte[hex_token] = static_cast<uint8_t>(b);
            }
        }
    }

    // Unk token
    if (!model["unk_token"].is_null()) {
        wrapper->unk_token = model["unk_token"].string_value();
        auto it = wrapper->vocab.find(wrapper->unk_token);
        if (it != wrapper->vocab.end())
            wrapper->unk_token_id = it->second;
    }

    // --- Parse added_tokens ---
    const auto& added = root["added_tokens"];
    if (added.is_array()) {
        for (const auto& at : added.array_items()) {
            TokenizerWrapper::AddedToken tok;
            tok.id = at["id"].int_value();
            tok.content = at["content"].string_value();
            tok.special = at["special"].bool_value();
            tok.lstrip = at["lstrip"].bool_value();
            tok.rstrip = at["rstrip"].bool_value();
            wrapper->added_tokens.push_back(tok);
            // Ensure added tokens are in vocab/id_to_token
            wrapper->vocab[tok.content] = tok.id;
            wrapper->id_to_token[tok.id] = tok.content;
        }
        // Sort by content length descending for greedy matching
        wrapper->added_tokens_sorted = wrapper->added_tokens;
        std::sort(wrapper->added_tokens_sorted.begin(),
                  wrapper->added_tokens_sorted.end(),
                  [](const TokenizerWrapper::AddedToken& a,
                     const TokenizerWrapper::AddedToken& b) {
                      return a.content.size() > b.content.size();
                  });
    }

    // Update vocab size to include added tokens
    int max_id = wrapper->vocab_size_count;
    for (const auto& at : wrapper->added_tokens)
        if (at.id >= max_id) max_id = at.id + 1;
    wrapper->vocab_size_count = max_id;

    // --- Parse normalizer ---
    const auto& normalizer = root["normalizer"];
    if (!normalizer.is_null()) {
        auto parseNormStep = [](const json11::Json& norm) -> std::optional<TokenizerWrapper::NormalizerStep> {
            std::string ntype = norm["type"].string_value();
            TokenizerWrapper::NormalizerStep step;
            if (ntype == "Prepend") {
                step.type = TokenizerWrapper::NormalizerStep::Prepend;
                step.arg1 = norm["prepend"].string_value();
            } else if (ntype == "Replace") {
                step.type = TokenizerWrapper::NormalizerStep::Replace;
                const auto& pat = norm["pattern"];
                if (pat.is_object() && !pat["String"].is_null())
                    step.arg1 = pat["String"].string_value();
                else
                    return std::nullopt; // skip regex patterns
                step.arg2 = norm["content"].string_value();
            } else if (ntype == "Lowercase") {
                step.type = TokenizerWrapper::NormalizerStep::Lowercase;
            } else if (ntype == "Strip") {
                step.type = TokenizerWrapper::NormalizerStep::Strip;
            } else {
                return std::nullopt;
            }
            return step;
        };

        if (normalizer["type"].string_value() == "Sequence") {
            for (const auto& item : normalizer["normalizers"].array_items()) {
                auto step = parseNormStep(item);
                if (step) wrapper->normalizer_steps.push_back(*step);
            }
        } else {
            auto step = parseNormStep(normalizer);
            if (step) wrapper->normalizer_steps.push_back(*step);
        }
    }

    // --- Parse pre_tokenizer ---
    const auto& pre_tok = root["pre_tokenizer"];
    if (!pre_tok.is_null()) {
        std::string pttype = pre_tok["type"].string_value();
        if (pttype == "ByteLevel") {
            wrapper->pre_tokenizer_type = TokenizerWrapper::PreTokenizerType::ByteLevel;
            initByteToUnicode(*wrapper);
        } else if (pttype == "Metaspace") {
            wrapper->pre_tokenizer_type = TokenizerWrapper::PreTokenizerType::Metaspace;
        } else if (pttype == "Whitespace") {
            wrapper->pre_tokenizer_type = TokenizerWrapper::PreTokenizerType::Whitespace;
        } else if (pttype == "Sequence") {
            for (const auto& item : pre_tok["pretokenizers"].array_items()) {
                if (item["type"].string_value() == "ByteLevel") {
                    wrapper->pre_tokenizer_type = TokenizerWrapper::PreTokenizerType::ByteLevel;
                    initByteToUnicode(*wrapper);
                    break;
                }
            }
        }
    }

    // --- Parse decoder ---
    const auto& decoder = root["decoder"];
    if (!decoder.is_null()) {
        auto parseDecStep = [](const json11::Json& dec) -> std::optional<TokenizerWrapper::DecoderStep> {
            std::string dtype = dec["type"].string_value();
            TokenizerWrapper::DecoderStep step;
            if (dtype == "Replace") {
                step.type = TokenizerWrapper::DecoderStep::Replace;
                const auto& pat = dec["pattern"];
                if (pat.is_object() && !pat["String"].is_null())
                    step.arg1 = pat["String"].string_value();
                step.arg2 = dec["content"].string_value();
            } else if (dtype == "ByteFallback") {
                step.type = TokenizerWrapper::DecoderStep::ByteFallback;
            } else if (dtype == "Fuse") {
                step.type = TokenizerWrapper::DecoderStep::Fuse;
            } else if (dtype == "Strip") {
                step.type = TokenizerWrapper::DecoderStep::Strip;
            } else if (dtype == "Metaspace") {
                step.type = TokenizerWrapper::DecoderStep::Metaspace;
            } else if (dtype == "ByteLevel") {
                step.type = TokenizerWrapper::DecoderStep::ByteLevel;
            } else {
                return std::nullopt;
            }
            return step;
        };

        if (decoder["type"].string_value() == "Sequence") {
            for (const auto& item : decoder["decoders"].array_items()) {
                auto step = parseDecStep(item);
                if (step) wrapper->decoder_steps.push_back(*step);
            }
        } else {
            auto step = parseDecStep(decoder);
            if (step) wrapper->decoder_steps.push_back(*step);
        }
    }

    // Synthesize default decoder if none was specified
    if (wrapper->decoder_steps.empty() && !wrapper->normalizer_steps.empty()) {
        bool has_prepend = false;
        for (const auto& s : wrapper->normalizer_steps)
            if (s.type == TokenizerWrapper::NormalizerStep::Prepend) has_prepend = true;
        if (has_prepend && wrapper->byte_fallback_enabled) {
            wrapper->decoder_steps.push_back({TokenizerWrapper::DecoderStep::Replace,
                                              "\xe2\x96\x81", " "});
            wrapper->decoder_steps.push_back({TokenizerWrapper::DecoderStep::ByteFallback, "", ""});
            wrapper->decoder_steps.push_back({TokenizerWrapper::DecoderStep::Fuse, "", ""});
            wrapper->decoder_steps.push_back({TokenizerWrapper::DecoderStep::Strip, "", ""});
        } else if (has_prepend) {
            wrapper->decoder_steps.push_back({TokenizerWrapper::DecoderStep::Metaspace, "", ""});
        }
    }

    // Store wrapper as ForeignPtr on the instance
    auto* sharedWrapper = new std::shared_ptr<TokenizerWrapper>(wrapper);
    Value fp = Value::foreignPtrVal(sharedWrapper);
    asForeignPtr(fp)->registerCleanup([](void* p) {
        delete static_cast<std::shared_ptr<TokenizerWrapper>*>(p);
    });
    inst->setProperty("_this", fp);

    return Value::nilVal();
}

// ============================================================
// Tokenizer method implementations
// ============================================================

Value ModuleNN::nn_tokenizer_encode_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]) || !isString(args[1]))
        throw std::invalid_argument("Tokenizer.encode expects a string argument");

    TokenizerWrapper* tw = getTokenizerWrapper(asObjectInstance(args[0]));
    if (tw->closed)
        throw std::invalid_argument("ai.nn: Tokenizer is closed");

    std::string text = toUTF8StdString(asStringObj(args[1])->s);
    auto ids = tokenizer_encode(*tw, text);

    Value list = Value::objVal(newListObj());
    for (int id : ids)
        asList(list)->append(Value::intVal(id));
    return list;
}

Value ModuleNN::nn_tokenizer_decode_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]) || !isList(args[1]))
        throw std::invalid_argument("Tokenizer.decode expects a list argument");

    TokenizerWrapper* tw = getTokenizerWrapper(asObjectInstance(args[0]));
    if (tw->closed)
        throw std::invalid_argument("ai.nn: Tokenizer is closed");

    ObjList* idList = asList(args[1]);
    std::vector<int> ids;
    ids.reserve(idList->length());
    for (int i = 0; i < idList->length(); ++i)
        ids.push_back(static_cast<int>(idList->getElement(i).asInt()));

    std::string result = tokenizer_decode(*tw, ids);
    return Value::stringVal(toUnicodeString(result));
}

Value ModuleNN::nn_tokenizer_vocab_size_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Tokenizer.vocab_size expects receiver");
    TokenizerWrapper* tw = getTokenizerWrapper(asObjectInstance(args[0]));
    return Value::intVal(tw->vocab_size_count);
}

Value ModuleNN::nn_tokenizer_special_tokens_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Tokenizer.special_tokens expects receiver");
    TokenizerWrapper* tw = getTokenizerWrapper(asObjectInstance(args[0]));

    Value dict = Value::objVal(newDictObj());
    for (const auto& at : tw->added_tokens) {
        if (at.special) {
            asDict(dict)->store(
                Value::stringVal(toUnicodeString(at.content)),
                Value::intVal(at.id));
        }
    }
    return dict;
}

Value ModuleNN::nn_tokenizer_close_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Tokenizer.close expects receiver");
    TokenizerWrapper* tw = getTokenizerWrapper(asObjectInstance(args[0]));
    tw->closed = true;
    tw->vocab.clear();
    tw->id_to_token.clear();
    tw->merge_rank.clear();
    tw->added_tokens.clear();
    tw->added_tokens_sorted.clear();
    tw->byte_to_token_id.clear();
    tw->byte_token_to_byte.clear();
    return Value::nilVal();
}
