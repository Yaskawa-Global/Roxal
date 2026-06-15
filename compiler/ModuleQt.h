#pragma once

#ifdef ROXAL_ENABLE_QT

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

#include <memory>

namespace roxal {

// Native backing for the `qt` module: owns a process-wide QGuiApplication and
// the QQmlApplicationEngine(s) the script creates, and integrates Qt's event
// loop with the VM cooperatively (no Qt exec()).
//
// All Qt state lives in a pimpl (Impl, defined in ModuleQt.cpp) so that no Qt
// headers leak through this header — important because VM.h includes it widely
// and Qt's `emit`/`signals`/`slots` macros would otherwise clash with Roxal's
// own identifiers. Qt object references are held via QPointer<> (auto-nulling,
// non-owning) inside Impl.
class ModuleQt : public BuiltinModule {
public:
    ModuleQt();
    virtual ~ModuleQt();

    void registerBuiltins(VM& vm) override;
    inline Value moduleType() const override { return moduleTypeValue; }

    // Create the QGuiApplication + install the host-loop hook (before any QML
    // loads); tear them down deterministically when the script run completes.
    void onModuleLoaded(VM& vm) override;
    void onScriptComplete(VM& vm) override;
    void onModuleUnloading(VM& vm) override;

private:
    Value moduleTypeValue; // ObjModuleType*

    struct Impl;                    // Qt state; defined in ModuleQt.cpp
    std::unique_ptr<Impl> impl_;

    // Ask run() to unblock: clears the current thread's sleep so the dispatch
    // loop resumes after run(). Called from Qt slots (window-close / Qt.quit())
    // and from Engine.quit(); all on the single main/UI thread.
    void requestQuit();

    // Detach the host-loop hook and destroy the engines + application. Safe only
    // while the VM/Qt runtime is alive (i.e. from onScriptComplete, not atexit).
    void teardownQt(VM& vm);

    // Engine methods (args[0] = receiver Engine instance). Signatures use only
    // Roxal types, keeping Qt out of the header.
    Value engine_init_builtin(ArgsView args);
    Value engine_load_builtin(ArgsView args);
    Value engine_load_string_builtin(ArgsView args);
    Value engine_run_builtin(ArgsView args);
    Value engine_quit_builtin(ArgsView args);
    Value engine_find_builtin(ArgsView args);
    Value engine_root_builtin(ArgsView args);

    // Module-level escape hatches for non-identifier / fully-dynamic names.
    Value qt_get_builtin(ArgsView args);
    Value qt_set_builtin(ArgsView args);
    Value qt_call_builtin(ArgsView args);
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
