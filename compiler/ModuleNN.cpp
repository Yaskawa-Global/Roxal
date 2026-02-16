#include "ModuleNN.h"
#include "VM.h"
#include "Object.h"
#include <stdexcept>
#include <memory>

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

static Value runInference(ModelWrapper* wrapper, ObjTensor* inputTensor) {
#ifdef ROXAL_ENABLE_ONNX
    if (wrapper->closed)
        throw std::runtime_error("ai.nn: Model session is closed");

    if (!inputTensor->isOrtBacked())
        throw std::runtime_error("ai.nn: input tensor is not ORT-backed");

    // Input ORT value
    const Ort::Value& inputOrt = inputTensor->ortValue();

    std::vector<Ort::Value> outputs;

    if (wrapper->device == "cuda") {
        // IoBinding path: keeps outputs on GPU for zero-copy model chaining.
        // Input tensors (CPU or GPU) are handled transparently by ORT.
        Ort::IoBinding binding(*wrapper->session);

        // Bind input(s)
        for (auto& name : wrapper->inputNames)
            binding.BindInput(name.c_str(), inputOrt);

        // Bind outputs to CUDA memory so they stay on GPU
        Ort::MemoryInfo cudaMemInfo("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        for (auto& name : wrapper->outputNames)
            binding.BindOutput(name.c_str(), cudaMemInfo);

        wrapper->session->Run(Ort::RunOptions{nullptr}, binding);
        outputs = binding.GetOutputValues();
    } else {
        // CPU path: standard Session::Run()
        std::vector<const char*> inputNamePtrs;
        inputNamePtrs.reserve(wrapper->inputNames.size());
        for (auto& name : wrapper->inputNames)
            inputNamePtrs.push_back(name.c_str());

        std::vector<const char*> outputNamePtrs;
        outputNamePtrs.reserve(wrapper->outputNames.size());
        for (auto& name : wrapper->outputNames)
            outputNamePtrs.push_back(name.c_str());

        outputs = wrapper->session->Run(
            Ort::RunOptions{nullptr},
            inputNamePtrs.data(), &inputOrt, 1,
            outputNamePtrs.data(), outputNamePtrs.size()
        );
    }

    if (outputs.size() == 1) {
        return Value::objVal(newTensorObj(std::move(outputs[0])));
    }

    // Multiple outputs: return list of tensors
    Value list = Value::objVal(newListObj());
    for (auto& out : outputs)
        asList(list)->elts.push_back(Value::objVal(newTensorObj(std::move(out))));
    return list;
#else
    (void)wrapper; (void)inputTensor;
    throw std::runtime_error("ai.nn requires ONNX Runtime (build with -DROXAL_ENABLE_ONNX=ON)");
#endif
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
    link("tensor_device", [this](VM&, ArgsView a) { return nn_tensor_device_builtin(a); });

    // Model object methods
    linkMethod("Model", "run",     [this](VM&, ArgsView a) { return nn_model_run_builtin(a); });
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
    std::shared_ptr<ModelWrapper> capturedWrapper = wrapper;
    NativeFn predictFn = [capturedWrapper](VM&, ArgsView args) -> Value {
        if (args.size() < 1 || !isTensor(args[0]))
            throw std::invalid_argument("predict expects a tensor argument");
        return runInference(capturedWrapper.get(), asTensor(args[0]));
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
    func->builtinInfo = make_ptr<BuiltinFuncInfo>(predictFn);
    // funcType is required for signal/dataflow integration: the VM uses it
    // to map signal arguments to FuncNode inputs when predict is called with a signal.
    func->funcType = makeFuncType({{"input", type::BuiltinType::Tensor}});

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

    return createModelObject(wrapper);
#else
    (void)path;
    throw std::runtime_error("ai.nn.load requires ONNX Runtime (build with -DROXAL_ENABLE_ONNX=ON)");
#endif
}

// ============================================================
// Model.run(input_tensor) → tensor | list
// ============================================================

Value ModuleNN::nn_model_run_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Model.run expects a tensor argument");
    if (!isTensor(args[1]))
        throw std::invalid_argument("Model.run expects a tensor argument");

    ModelWrapper* wrapper = getModelWrapper(asObjectInstance(args[0]));
    return runInference(wrapper, asTensor(args[1]));
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
        wrapper->closed = true;
#ifdef ROXAL_ENABLE_ONNX
        wrapper->session.reset();
#endif
    }
    return Value::nilVal();
}
