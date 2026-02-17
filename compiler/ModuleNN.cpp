#include "ModuleNN.h"
#include "VM.h"
#include "Object.h"
#include <stdexcept>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <future>
#include <functional>

#ifdef ROXAL_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
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
    link("load", [this](VM&, ArgsView a) { return nn_load_builtin(a); });
    link("tensor_device", [this](VM&, ArgsView a) { return nn_tensor_device_builtin(a); },
         /*defaults=*/{}, /*resolveArgMask=*/0x1);

    // Model object methods
    // predict is declared here for help()/docstring visibility; the instance property
    // closure (set in createModelObject) shadows this for actual calls and signal integration.
    linkMethod("Model", "predict", [](VM& vm, ArgsView a) -> Value {
        if (a.size() < 2 || !isObjectInstance(a[0]))
            throw std::invalid_argument("predict expects an input argument");
        return safeRunInferenceFromValueAsync(vm, getModelWrapperShared(asObjectInstance(a[0])), a[1]);
    });
    linkMethod("Model", "inputs",  [this](VM&, ArgsView a) { return nn_model_inputs_builtin(a); });
    linkMethod("Model", "outputs", [this](VM&, ArgsView a) { return nn_model_outputs_builtin(a); });
    linkMethod("Model", "device",  [this](VM&, ArgsView a) { return nn_model_device_builtin(a); });
    linkMethod("Model", "close",   [this](VM&, ArgsView a) { return nn_model_close_builtin(a); });
}

// ============================================================
// createModelObject — wraps a ModelWrapper in a Roxal Model instance
// ============================================================

Value ModuleNN::createModelObject(const std::shared_ptr<ModelWrapper>& wrapper)
{
    // Look up the Model type declared in nn.rox
    auto typeVal = asModuleType(moduleType())->vars.load(toUnicodeString("Model"));
    if (!typeVal.has_value() || !isObjectType(typeVal.value()))
        throw std::runtime_error("Model type not found in ai.nn module");

    // Create object instance
    Value instance = Value::objVal(newObjectInstance(typeVal.value()));
    ObjectInstance* inst = asObjectInstance(instance);

    // Store wrapper as foreignPtr with shared_ptr for safe sharing with predict closure
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
    NativeFn predictFn = [capturedWrapper](VM& vm, ArgsView args) -> Value {
        if (args.size() < 1)
            throw std::invalid_argument("predict expects an input argument");
        return safeRunInferenceFromValueAsync(vm, capturedWrapper, args[0]);
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
    // resolveArgMask = 0x1: resolve future arg 0 before calling NativeFn.
    // This enables chained predict: model_b.predict(model_a.predict(t))
    // — the inner future is resolved before model_b's predict runs.
    func->builtinInfo = make_ptr<BuiltinFuncInfo>(predictFn, std::vector<Value>{}, 0x1);
    func->doc = toUnicodeString(
        "Run inference. Input may be a tensor, a dict {name: tensor}, or a list "
        "of tensors. Returns output tensor (or list if multiple outputs). "
        "Also works with signals for reactive dataflow.");
    // funcType is required for signal/dataflow integration: the VM uses it
    // to map signal arguments to FuncNode inputs when predict is called with a signal.
    // Parameter is untyped (nullopt) so the VM doesn't coerce dict/list to tensor.
    func->funcType = makeFuncType({{"input", std::nullopt}});

    Value funcVal = Value::objVal(std::move(funcObj));
    Value closureVal = Value::objVal(newClosureObj(funcVal));
    inst->setProperty("predict", closureVal);

    return instance;
}

// ============================================================
// ai.nn.load(path) → Model
// ============================================================

Value ModuleNN::nn_load_builtin(ArgsView args)
{
    if (args.size() < 1)
        throw std::invalid_argument("ai.nn.load expects a model path string");
    if (!isString(args[0]))
        throw std::invalid_argument("ai.nn.load expects a model path string");

    std::string path = toUTF8StdString(asStringObj(args[0])->s);
    bool doWarmup = (args.size() < 2) || args[1].asBool();

#ifdef ROXAL_ENABLE_ONNX
    auto& ortEnv = OnnxEnvironment::instance();
    auto opts = ortEnv.createSessionOptions();

    auto wrapper = std::make_shared<ModelWrapper>();
    wrapper->session = std::make_unique<Ort::Session>(ortEnv.env(), path.c_str(), opts);
    wrapper->device = ortEnv.cudaAvailable() ? "cuda" : "cpu";

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
            for (auto& d : shape)
                if (d < 0) d = 1;  // replace dynamic dimensions with 1
            warmupTensors.push_back(
                Value::tensorVal(shape, tensorDTypeFromString(wrapper->inputDtypes[i])));
            warmupInputs.emplace_back(wrapper->inputNames[i], asTensor(warmupTensors.back()));
        }
        runInferenceMulti(wrapper.get(), warmupInputs);
    }

    return createModelObject(wrapper);
#else
    (void)path;
    throw std::runtime_error("ai.nn.load requires ONNX Runtime (build with -DROXAL_ENABLE_ONNX=ON)");
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
