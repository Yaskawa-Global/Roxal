#include "ModuleNN.h"
#include "VM.h"
#include "Object.h"
#include "core/json11.h"
#include <stdexcept>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <future>
#include <functional>
#include <fstream>
#include <sstream>
#include <algorithm>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#ifdef ROXAL_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#ifdef ROXAL_ENABLE_ONNX
#include "CudaRuntime.h"
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

    Ort::Env& env() { return env_; }

    Ort::SessionOptions createSessionOptions(bool requestGpu = true) {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        if (requestGpu && cudaAvailable_) {
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

    bool cudaAvailable() const { return cudaAvailable_; }

private:
    OnnxEnvironment() : env_(ORT_LOGGING_LEVEL_WARNING, "roxal-nn") {
        // Probe for CUDA by trying to create a session options with CUDA provider
        try {
            Ort::SessionOptions probe;
            OrtCUDAProviderOptions cuda_opts{};
            cuda_opts.device_id = 0;
            probe.AppendExecutionProvider_CUDA(cuda_opts);
            cudaAvailable_ = true;
        } catch (...) {
            cudaAvailable_ = false;
        }
    }
    Ort::Env env_;
    bool cudaAvailable_ = false;
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

class InferenceWorker {
    std::thread workerThread;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::atomic<bool> running{false};

    struct Job {
        std::function<Value()> work;
        ptr<std::promise<Value>> promise;
        std::vector<Value> heldValues;  // prevent GC of input tensors
    };
    std::queue<Job> pendingJobs;

    void workerLoop() {
        while (running) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCV.wait(lock, [&]{ return !pendingJobs.empty() || !running; });
                if (!running && pendingJobs.empty()) break;
                job = std::move(pendingJobs.front());
                pendingJobs.pop();
            }
            try {
                Value result = job.work();
                job.promise->set_value(result);
            } catch (const std::exception& e) {
                std::cerr << "ai.nn InferenceWorker error: " << e.what() << std::endl;
                job.promise->set_value(Value::nilVal());
            } catch (...) {
                std::cerr << "ai.nn InferenceWorker: unknown error" << std::endl;
                job.promise->set_value(Value::nilVal());
            }
        }
    }

public:
    Value submit(std::function<Value()> work, std::vector<Value> heldValues = {}) {
        auto promise = make_ptr<std::promise<Value>>();
        auto future = promise->get_future().share();
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            Job job;
            job.work = std::move(work);
            job.promise = std::move(promise);
            job.heldValues = std::move(heldValues);
            pendingJobs.push(std::move(job));
        }
        queueCV.notify_one();
        return Value::futureVal(future);
    }

    void start() {
        running = true;
        workerThread = std::thread(&InferenceWorker::workerLoop, this);
    }

    void stop() {
        running = false;
        queueCV.notify_all();
        if (workerThread.joinable())
            workerThread.join();
    }

    ~InferenceWorker() {
        stop();
    }
};

// ============================================================
// ModelWrapper — per-model native state
// ============================================================

struct ModelWrapper {
#ifdef ROXAL_ENABLE_ONNX
    std::unique_ptr<Ort::Session> session;
#endif
    std::vector<std::string> inputNames;
    std::vector<std::vector<int64_t>> inputShapes;
    std::vector<std::string> inputDtypes;

    std::vector<std::string> outputNames;
    std::vector<std::vector<int64_t>> outputShapes;
    std::vector<std::string> outputDtypes;

    std::string device = "cpu";
    bool closed = false;

    std::unique_ptr<InferenceWorker> inferenceWorker;
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

// Core inference with multiple named inputs.
// Each entry in inputTensors is (inputName, ObjTensor*).
static Value runInferenceMulti(ModelWrapper* wrapper,
                               const std::vector<std::pair<std::string, ObjTensor*>>& inputTensors) {
#ifdef ROXAL_ENABLE_ONNX
    if (wrapper->closed)
        throw std::invalid_argument("ai.nn: Model session is closed");

    // Validate each input
    for (auto& [name, tensor] : inputTensors) {
        if (!tensor->isOrtBacked())
            throw std::runtime_error("ai.nn: input tensor '" + name + "' is not ORT-backed");

        // Find the expected shape for this input name
        for (size_t i = 0; i < wrapper->inputNames.size(); ++i) {
            if (wrapper->inputNames[i] == name) {
                if (!isShapeCompatible(tensor->shape(), wrapper->inputShapes[i])) {
                    throw std::invalid_argument(
                        "ai.nn: shape mismatch for input '" + name +
                        "': model expects " + shapeToString(wrapper->inputShapes[i]) +
                        " but got " + shapeToString(tensor->shape()));
                }
                break;
            }
        }
    }

    // Use IoBinding for both CPU and CUDA: it allows binding each input
    // individually by name (ORT's Session::Run requires a contiguous array
    // of Ort::Value which we can't form from separate ObjTensor objects).
    Ort::IoBinding binding(*wrapper->session);

    for (auto& [name, tensor] : inputTensors)
        binding.BindInput(name.c_str(), tensor->ortValue());

    std::vector<Ort::Value> outputs;

    if (wrapper->device == "cuda") {
        Ort::MemoryInfo cudaMemInfo("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        for (auto& name : wrapper->outputNames)
            binding.BindOutput(name.c_str(), cudaMemInfo);
    } else {
        Ort::MemoryInfo cpuMemInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        for (auto& name : wrapper->outputNames)
            binding.BindOutput(name.c_str(), cpuMemInfo);
    }

    wrapper->session->Run(Ort::RunOptions{nullptr}, binding);
    outputs = binding.GetOutputValues();

    if (outputs.size() == 1) {
        return Value::objVal(newTensorObj(std::move(outputs[0])));
    }

    // Multiple outputs: return list of tensors
    Value list = Value::objVal(newListObj());
    for (auto& out : outputs)
        asList(list)->elts.push_back(Value::objVal(newTensorObj(std::move(out))));
    return list;
#else
    (void)wrapper; (void)inputTensors;
    throw std::runtime_error("ai.nn requires ONNX Runtime (build with -DROXAL_ENABLE_ONNX=ON)");
#endif
}

// Convenience: single-tensor input (most common case)
static Value runInference(ModelWrapper* wrapper, ObjTensor* inputTensor) {
    if (wrapper->inputNames.empty())
        throw std::runtime_error("ai.nn: model has no inputs");
    return runInferenceMulti(wrapper, {{wrapper->inputNames[0], inputTensor}});
}

// Wrapper that catches std::invalid_argument from runInference and converts
// it into a Roxal exception catchable by try/except.
static Value safeRunInference(VM& vm, ModelWrapper* wrapper, ObjTensor* inputTensor) {
    try {
        return runInference(wrapper, inputTensor);
    } catch (const std::invalid_argument& e) {
        Value msg = Value::stringVal(toUnicodeString(e.what()));
        vm.raiseException(Value::exceptionVal(msg));
        return Value::nilVal();
    }
}

// Multi-input wrapper: accepts a dict {name: tensor} or list [tensor, ...].
static Value safeRunInferenceFromValue(VM& vm, ModelWrapper* wrapper, const Value& arg) {
    try {
        if (isTensor(arg)) {
            return runInference(wrapper, asTensor(arg));
        }
        else if (isDict(arg)) {
            ObjDict* dict = asDict(arg);
            auto items = dict->items();
            std::vector<std::pair<std::string, ObjTensor*>> inputs;
            inputs.reserve(items.size());
            for (auto& [key, val] : items) {
                if (!isString(key))
                    throw std::invalid_argument("ai.nn: predict dict keys must be strings");
                if (!isTensor(val))
                    throw std::invalid_argument(
                        "ai.nn: predict dict value for '" +
                        toUTF8StdString(asStringObj(key)->s) + "' is not a tensor");
                inputs.emplace_back(toUTF8StdString(asStringObj(key)->s), asTensor(val));
            }
            return runInferenceMulti(wrapper, inputs);
        }
        else if (isList(arg)) {
            ObjList* list = asList(arg);
            if (static_cast<size_t>(list->length()) != wrapper->inputNames.size()) {
                throw std::invalid_argument(
                    "ai.nn: predict list has " + std::to_string(list->length()) +
                    " elements but model expects " + std::to_string(wrapper->inputNames.size()) +
                    " inputs");
            }
            std::vector<std::pair<std::string, ObjTensor*>> inputs;
            inputs.reserve(list->length());
            for (size_t i = 0; i < static_cast<size_t>(list->length()); ++i) {
                Value elem = list->elts.at(i);
                if (!isTensor(elem))
                    throw std::invalid_argument(
                        "ai.nn: predict list element " + std::to_string(i) + " is not a tensor");
                inputs.emplace_back(wrapper->inputNames[i], asTensor(elem));
            }
            return runInferenceMulti(wrapper, inputs);
        }
        else {
            throw std::invalid_argument(
                "ai.nn: predict expects a tensor, dict of tensors, or list of tensors");
        }
    } catch (const std::invalid_argument& e) {
        Value msg = Value::stringVal(toUnicodeString(e.what()));
        vm.raiseException(Value::exceptionVal(msg));
        return Value::nilVal();
    }
}

// Async multi-input predict: validates synchronously, submits inference to worker thread.
// Input validation (type, shape) happens on the calling thread so errors are catchable
// by Roxal try/except. The actual ORT Session::Run happens on the worker thread.
static Value safeRunInferenceFromValueAsync(VM& vm,
                                            const std::shared_ptr<ModelWrapper>& wrapper,
                                            const Value& arg) {
    try {
        if (wrapper->closed)
            throw std::invalid_argument("ai.nn: Model session is closed");
        if (!wrapper->inferenceWorker)
            throw std::runtime_error("ai.nn: Model inference worker not initialized");

        // Build inputs vector (type validation — synchronous)
        std::vector<std::pair<std::string, ObjTensor*>> inputs;
        std::vector<Value> heldValues;
        heldValues.push_back(arg);  // Keep arg alive during async inference

        if (isTensor(arg)) {
            if (wrapper->inputNames.empty())
                throw std::runtime_error("ai.nn: model has no inputs");
            inputs.emplace_back(wrapper->inputNames[0], asTensor(arg));
        }
        else if (isDict(arg)) {
            ObjDict* dict = asDict(arg);
            auto items = dict->items();
            inputs.reserve(items.size());
            for (auto& [key, val] : items) {
                if (!isString(key))
                    throw std::invalid_argument("ai.nn: predict dict keys must be strings");
                if (!isTensor(val))
                    throw std::invalid_argument(
                        "ai.nn: predict dict value for '" +
                        toUTF8StdString(asStringObj(key)->s) + "' is not a tensor");
                inputs.emplace_back(toUTF8StdString(asStringObj(key)->s), asTensor(val));
                heldValues.push_back(val);
            }
        }
        else if (isList(arg)) {
            ObjList* list = asList(arg);
            if (static_cast<size_t>(list->length()) != wrapper->inputNames.size()) {
                throw std::invalid_argument(
                    "ai.nn: predict list has " + std::to_string(list->length()) +
                    " elements but model expects " + std::to_string(wrapper->inputNames.size()) +
                    " inputs");
            }
            inputs.reserve(list->length());
            for (size_t i = 0; i < static_cast<size_t>(list->length()); ++i) {
                Value elem = list->elts.at(i);
                if (!isTensor(elem))
                    throw std::invalid_argument(
                        "ai.nn: predict list element " + std::to_string(i) + " is not a tensor");
                inputs.emplace_back(wrapper->inputNames[i], asTensor(elem));
                heldValues.push_back(elem);
            }
        }
        else {
            throw std::invalid_argument(
                "ai.nn: predict expects a tensor, dict of tensors, or list of tensors");
        }

        // Shape validation (synchronous — so errors are catchable by try/except)
        for (auto& [name, tensor] : inputs) {
            if (!tensor->isOrtBacked())
                throw std::runtime_error("ai.nn: input tensor '" + name + "' is not ORT-backed");
            for (size_t i = 0; i < wrapper->inputNames.size(); ++i) {
                if (wrapper->inputNames[i] == name) {
                    if (!isShapeCompatible(tensor->shape(), wrapper->inputShapes[i])) {
                        throw std::invalid_argument(
                            "ai.nn: shape mismatch for input '" + name +
                            "': model expects " + shapeToString(wrapper->inputShapes[i]) +
                            " but got " + shapeToString(tensor->shape()));
                    }
                    break;
                }
            }
        }

        // Submit to worker thread (returns future immediately).
        // Capture shared_ptr to keep model alive during inference.
        auto work = [wrapper, inputs]() -> Value {
            return runInferenceMulti(wrapper.get(), inputs);
        };
        return wrapper->inferenceWorker->submit(std::move(work), std::move(heldValues));

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

void ModuleNN::registerBuiltins(VM& vm)
{
    setVM(vm);

    // Module-level functions
    link("tensor_device", [this](VM&, ArgsView a) { return nn_tensor_device_builtin(a); },
         /*defaults=*/{}, /*resolveArgMask=*/0x1);
    link("memory_info", [this](VM&, ArgsView a) { return nn_memory_info_builtin(a); });

    // Model object methods
    linkMethod("Model", "init", [this](VM&, ArgsView a) { return nn_model_init_builtin(a); });
    // predict is declared here for help()/docstring visibility; the instance property
    // closure (set in Model.init) shadows this for actual calls and signal integration.
    linkMethod("Model", "predict", [](VM& vm, ArgsView a) -> Value {
        if (a.size() < 2 || !isObjectInstance(a[0]))
            throw std::invalid_argument("predict expects an input argument");
        return safeRunInferenceFromValueAsync(vm, getModelWrapperShared(asObjectInstance(a[0])), a[1]);
    });
    linkMethod("Model", "inputs",  [this](VM&, ArgsView a) { return nn_model_inputs_builtin(a); });
    linkMethod("Model", "outputs", [this](VM&, ArgsView a) { return nn_model_outputs_builtin(a); });
    linkMethod("Model", "device",  [this](VM&, ArgsView a) { return nn_model_device_builtin(a); });
    linkMethod("Model", "close",   [this](VM&, ArgsView a) { return nn_model_close_builtin(a); });

    // Tokenizer object methods
    linkMethod("Tokenizer", "init",           [this](VM&, ArgsView a) { return nn_tokenizer_init_builtin(a); });
    linkMethod("Tokenizer", "encode",         [this](VM&, ArgsView a) { return nn_tokenizer_encode_builtin(a); });
    linkMethod("Tokenizer", "decode",         [this](VM&, ArgsView a) { return nn_tokenizer_decode_builtin(a); });
    linkMethod("Tokenizer", "vocab_size",     [this](VM&, ArgsView a) { return nn_tokenizer_vocab_size_builtin(a); });
    linkMethod("Tokenizer", "special_tokens", [this](VM&, ArgsView a) { return nn_tokenizer_special_tokens_builtin(a); });
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

    bool requestGpu;
    if (deviceArg == "auto")
        requestGpu = true;
    else if (deviceArg == "cuda")
        requestGpu = true;
    else if (deviceArg == "cpu")
        requestGpu = false;
    else
        throw std::invalid_argument("Model.init: device must be 'auto', 'cpu', or 'cuda'");

#ifdef ROXAL_ENABLE_ONNX
    auto& ortEnv = OnnxEnvironment::instance();
    auto opts = ortEnv.createSessionOptions(requestGpu);

    auto wrapper = std::make_shared<ModelWrapper>();
    try {
        wrapper->session = std::make_unique<Ort::Session>(ortEnv.env(), path.c_str(), opts);
    } catch (const Ort::Exception& e) {
        throw std::invalid_argument(std::string("Model.init: failed to load '") + path + "': " + e.what());
    }
    wrapper->device = (requestGpu && ortEnv.cudaAvailable()) ? "cuda" : "cpu";

    Ort::AllocatorWithDefaultOptions allocator;

    // Extract input metadata
    size_t numInputs = wrapper->session->GetInputCount();
    for (size_t i = 0; i < numInputs; ++i) {
        auto nameAlloc = wrapper->session->GetInputNameAllocated(i, allocator);
        wrapper->inputNames.push_back(nameAlloc.get());

        auto typeInfo = wrapper->session->GetInputTypeInfo(i);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        wrapper->inputShapes.push_back(tensorInfo.GetShape());
        wrapper->inputDtypes.push_back(ortDtypeToString(tensorInfo.GetElementType()));
    }

    // Extract output metadata
    size_t numOutputs = wrapper->session->GetOutputCount();
    for (size_t i = 0; i < numOutputs; ++i) {
        auto nameAlloc = wrapper->session->GetOutputNameAllocated(i, allocator);
        wrapper->outputNames.push_back(nameAlloc.get());

        auto typeInfo = wrapper->session->GetOutputTypeInfo(i);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        wrapper->outputShapes.push_back(tensorInfo.GetShape());
        wrapper->outputDtypes.push_back(ortDtypeToString(tensorInfo.GetElementType()));
    }

    // Start the inference worker thread (must be before warmup since warmup
    // is synchronous and doesn't use the worker).
    wrapper->inferenceWorker = std::make_unique<InferenceWorker>();
    wrapper->inferenceWorker->start();

    // Auto-warmup: run one inference with zero tensors to initialize the CUDA
    // context and trigger ORT graph optimization.  This makes the first real
    // predict() call fast and consistent.
    if (doWarmup && !wrapper->inputShapes.empty()) {
        std::vector<Value> warmupTensors;
        std::vector<std::pair<std::string, ObjTensor*>> warmupInputs;
        warmupTensors.reserve(wrapper->inputShapes.size());
        warmupInputs.reserve(wrapper->inputShapes.size());
        for (size_t i = 0; i < wrapper->inputShapes.size(); ++i) {
            auto shape = wrapper->inputShapes[i];
            // Replace dynamic dimensions (-1) with reasonable defaults:
            //   batch dimension (first): 1
            //   spatial/sequence dimensions: 64 (minimum safe size for most models)
            for (size_t di = 0; di < shape.size(); ++di)
                if (shape[di] < 0) shape[di] = (di == 0) ? 1 : 64;
            warmupTensors.push_back(
                Value::tensorVal(shape, tensorDTypeFromString(wrapper->inputDtypes[i])));
            warmupInputs.emplace_back(wrapper->inputNames[i], asTensor(warmupTensors.back()));
        }
        try {
            runInferenceMulti(wrapper.get(), warmupInputs);
        } catch (const std::exception& e) {
            // Warmup failure is non-fatal — the model is still usable.
            // This can happen when synthetic input dimensions are incompatible
            // with model constraints.
        }
    }

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
        "Also works with signals for reactive dataflow.");
    func->funcType = makeFuncType({{"input", std::nullopt}});

    Value funcVal = Value::objVal(std::move(funcObj));
    Value closureVal = Value::objVal(newClosureObj(funcVal));
    inst->setProperty("predict", closureVal);

    return Value::nilVal();
#else
    (void)inst; (void)path;
    throw std::runtime_error("Model.init requires ONNX Runtime (build with -DROXAL_ENABLE_ONNX=ON)");
#endif
}

// ============================================================
// Model.inputs() → list of {name, shape, dtype}
// ============================================================

static Value buildIODescriptorList(const std::vector<std::string>& names,
                                   const std::vector<std::vector<int64_t>>& shapes,
                                   const std::vector<std::string>& dtypes)
{
    Value list = Value::objVal(newListObj());
    for (size_t i = 0; i < names.size(); ++i) {
        Value dict = Value::objVal(newDictObj());
        asDict(dict)->store(
            Value::stringVal(toUnicodeString("name")),
            Value::stringVal(toUnicodeString(names[i]))
        );

        Value shapeList = Value::objVal(newListObj());
        for (auto dim : shapes[i])
            asList(shapeList)->elts.push_back(Value::intVal(dim));
        asDict(dict)->store(
            Value::stringVal(toUnicodeString("shape")),
            shapeList
        );

        asDict(dict)->store(
            Value::stringVal(toUnicodeString("dtype")),
            Value::stringVal(toUnicodeString(dtypes[i]))
        );

        asList(list)->elts.push_back(dict);
    }
    return list;
}

Value ModuleNN::nn_model_inputs_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.inputs expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    return buildIODescriptorList(wrapper->inputNames, wrapper->inputShapes, wrapper->inputDtypes);
}

// ============================================================
// Model.outputs() → list of {name, shape, dtype}
// ============================================================

Value ModuleNN::nn_model_outputs_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.outputs expects receiver");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    return buildIODescriptorList(wrapper->outputNames, wrapper->outputShapes, wrapper->outputDtypes);
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
        // Stop worker thread first (waits for in-flight inference to complete)
        if (wrapper->inferenceWorker)
            wrapper->inferenceWorker->stop();
        wrapper->closed = true;
#ifdef ROXAL_ENABLE_ONNX
        wrapper->session.reset();
#endif
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
    icu::UnicodeString us = toUnicodeString(s);
    std::vector<std::string> chars;
    int32_t i = 0;
    while (i < us.length()) {
        UChar32 cp = us.char32At(i);
        int32_t next = i + U16_LENGTH(cp);
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
        asList(list)->elts.push_back(Value::intVal(id));
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
        ids.push_back(static_cast<int>(idList->elts.at(i).asInt()));

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
