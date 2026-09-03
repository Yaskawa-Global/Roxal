#pragma once

// The `debug` module: the script-facing debugger control surface backing
// modules/debug.rox.  Its callers are debugger SERVICE contexts -- the web
// Debug actor, an embedding's control script -- running on debug-excluded
// threads (the in-VM controller precedent): the natives drive the stop
// coordinator directly and read through DebugInspector's GC-covered entry
// points.

#include "BuiltinModule.h"
#include "Value.h"

namespace roxal {

class ModuleDebug : public BuiltinModule {
public:
    ModuleDebug();
    ~ModuleDebug() override;

    void registerBuiltins(VM& vm) override;
    void onScriptComplete(VM& vm) override;
    Value moduleType() const override { return moduleTypeValue; }

    // Armed by the embedding (wasm: roxal_debug_session) BEFORE a debug
    // run: modules/web.rox's import-time hook then exposes the debugger
    // services, so a batch script is debuggable with only `import web`.
    static void setSessionPending(bool on);
    static bool sessionPending();

    // Host-constructed control capability: the embedding registers the
    // actor instance it built and exposed as the "debug" store; only that
    // actor's thread may drive the control natives.  Reset at each script
    // completion.
    static void registerControlActor(const Value& actorInstance);

    // Is a control actor live, i.e. did the RUNNING program start under an
    // armed session?  An embedding whose only controller is that actor
    // installs this as the stop coordinator's controller-presence gate, so a
    // breakpoint left armed from an earlier session cannot freeze a program
    // that has no debugger to release it.
    static bool controlActorPresent();

private:
    Value moduleTypeValue;   // ObjModuleType*

    Value debug_builtin(const std::string& op, ArgsView args);
};

} // namespace roxal
