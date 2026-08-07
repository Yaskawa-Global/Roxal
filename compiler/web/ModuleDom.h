#pragma once

#ifdef __EMSCRIPTEN__

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

// The `dom` module: JavaScript/DOM access for the WebAssembly build.
//
// Most use goes through native syntax on the values this module hands out
// (el.text_content = "hi"), which ObjJsValue implements. The functions here are
// the escape hatches -- exact-name access, event listeners, explicit flush --
// plus the module vars `document` and `window`, populated at import.
class ModuleDom : public BuiltinModule {
public:
    ModuleDom();
    ~ModuleDom() override;

    void registerBuiltins(VM& vm) override;
    void onModuleLoaded(VM& vm) override;
    void onScriptComplete(VM& vm) override;
    void onModuleUnloading(VM& vm) override;

    Value moduleType() const override { return moduleTypeValue; }

private:
    Value moduleTypeValue;   // ObjModuleType*

    Value init_builtin(ArgsView args);
    Value global_builtin(ArgsView args);
    Value get_builtin(ArgsView args);
    Value set_builtin(ArgsView args);
    Value call_builtin(ArgsView args);
    Value on_builtin(ArgsView args);
    Value run_builtin(ArgsView args);
    Value stop_builtin(ArgsView args);
    Value off_builtin(ArgsView args);
    Value flush_builtin(ArgsView args);
};

} // namespace roxal

#endif // __EMSCRIPTEN__
