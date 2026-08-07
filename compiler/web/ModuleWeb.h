#pragma once

#ifdef __EMSCRIPTEN__

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

// The `web` module: expose Roxal state to a UI framework.
//
// Where `dom` hands a script the page directly, `web` inverts it — Roxal publishes
// state and the view layer subscribes. The heavy lifting is in RoxalStore; this is
// the Roxal-facing surface plus the serve/stop lifecycle.
class ModuleWeb : public BuiltinModule {
public:
    ModuleWeb();
    ~ModuleWeb() override;

    void registerBuiltins(VM& vm) override;
    void onModuleLoaded(VM& vm) override;
    void onScriptComplete(VM& vm) override;
    void onModuleUnloading(VM& vm) override;

    Value moduleType() const override { return moduleTypeValue; }

private:
    Value moduleTypeValue;   // ObjModuleType*

    Value expose_builtin(ArgsView args);
    Value notify_builtin(ArgsView args);
    Value serve_builtin(ArgsView args);
    Value stop_builtin(ArgsView args);
};

} // namespace roxal

#endif // __EMSCRIPTEN__
