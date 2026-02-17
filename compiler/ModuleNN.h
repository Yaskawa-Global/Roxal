#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"
#include <memory>

struct ModelWrapper;      // opaque native state for Model instances
struct TokenizerWrapper;  // opaque native state for Tokenizer instances

namespace roxal {

class ModuleNN : public BuiltinModule {
public:
    ModuleNN();
    virtual ~ModuleNN();

    void registerBuiltins(VM& vm) override;

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
