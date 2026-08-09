#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"
#include <memory>

struct ModelWrapper;      // opaque native state for Model instances
struct TokenizerWrapper;  // opaque native state for Tokenizer instances

namespace roxal {

// GC root hook: pins the input tensors held by queued and in-flight
// InferenceWorker jobs. Called from SimpleMarkSweepGC::visitRoots.
void nnTraceWorkers(ValueVisitor& visitor);

class ModuleNN : public BuiltinModule {
public:
    ModuleNN();
    virtual ~ModuleNN();

    void registerBuiltins(VM& vm) override;

#ifdef __EMSCRIPTEN__
    // The provider's replies arrive via the host event loop's inbound drain,
    // so the wasm backend needs the loop installed even when the script never
    // imports web/dom (mirrors ModuleWeb/ModuleDom).
    void onModuleLoaded(VM& vm) override;
#endif

    inline Value moduleType() const override { return moduleTypeValue; }

    // Module-level functions
    Value nn_tensor_device_builtin(ArgsView args);
    Value nn_memory_info_builtin(ArgsView args);

    // Model object methods
    Value nn_model_init_builtin(ArgsView args);
    Value nn_model_inputs_builtin(ArgsView args);
    Value nn_model_outputs_builtin(ArgsView args);
    Value nn_model_device_builtin(ArgsView args);
    Value nn_model_close_builtin(ArgsView args);

    // Tokenizer object methods
    Value nn_tokenizer_init_builtin(ArgsView args);
    Value nn_tokenizer_encode_builtin(ArgsView args);
    Value nn_tokenizer_decode_builtin(ArgsView args);
    Value nn_tokenizer_vocab_size_builtin(ArgsView args);
    Value nn_tokenizer_special_tokens_builtin(ArgsView args);
    Value nn_tokenizer_close_builtin(ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*

};

} // namespace roxal
