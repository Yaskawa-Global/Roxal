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

    // True once a quit (window close / Qt.quit() / engine.quit()) has been
    // requested for the current run. The signal hub reads this to decide whether
    // a synchronous callback that temporarily un-parked run() should re-park.
    static bool quitRequested();

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
    Value engine_set_context_property_builtin(ArgsView args);

    // ListModel methods (args[0] = receiver ListModel instance). A QAbstractListModel
    // shim backed by a Roxal list of row objects; roles = the row type's properties.
    Value listmodel_init_builtin(ArgsView args);
    Value listmodel_count_builtin(ArgsView args);
    Value listmodel_row_builtin(ArgsView args);
    Value listmodel_append_builtin(ArgsView args);
    Value listmodel_insert_builtin(ArgsView args);
    Value listmodel_remove_builtin(ArgsView args);
    Value listmodel_move_builtin(ArgsView args);
    Value listmodel_clear_builtin(ArgsView args);
    Value listmodel_set_rows_builtin(ArgsView args);
    Value listmodel_begin_reset_builtin(ArgsView args);
    Value listmodel_end_reset_builtin(ArgsView args);
    Value listmodel_row_changed_builtin(ArgsView args);
    Value listmodel_cell_changed_builtin(ArgsView args);
    Value listmodel_set_builtin(ArgsView args);

    // Module-level escape hatches for non-identifier / fully-dynamic names.
    Value qt_get_builtin(ArgsView args);
    Value qt_set_builtin(ArgsView args);
    Value qt_call_builtin(ArgsView args);

    // Signal connections (dynamic / non-identifier signal names).
    Value qt_connect_builtin(ArgsView args);
    Value qt_on_builtin(ArgsView args);
    Value qt_disconnect_builtin(ArgsView args);

    // Force-push a bindable object's property to QML (for in-place collection mutation).
    Value qt_notify_builtin(ArgsView args);

    // Qt/QML diagnostic-message handling (keep Qt warnings out of the print() stream).
    Value qt_log_to_file_builtin(ArgsView args);
    Value qt_log_silence_builtin(ArgsView args);
    Value qt_log_to_stderr_builtin(ArgsView args);
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
