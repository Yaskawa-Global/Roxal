#pragma once

#include "../compiler/BuiltinModule.h"
#include "../compiler/Value.h"
#include "../compiler/Object.h"

namespace roxal {

class ModuleUI : public BuiltinModule {
public:
ModuleUI();
    virtual ~ModuleUI();

    // non-copyable/movable
    ModuleUI(const ModuleUI&) = delete;
    ModuleUI& operator=(const ModuleUI&) = delete;
    ModuleUI(ModuleUI&&) = delete;
    ModuleUI& operator=(ModuleUI&&) = delete;

    void registerBuiltins(VM& vm) override;

    virtual void initialize();

    inline Value moduleType() const { return moduleTypeValue; }

    Value uiType(const std::string& typeName);

    // create a default constructed instance of the named ui module type
    //  e.g. newUIObj("Display") -> ui.Display instance
    Value newUIObj(const std::string& typeName);
    Value newUIObj(const Value& typeObj);

    // builtin function implementations

    Value display_create_window(ArgsView args);

    void window_close(ArgsView args);
    void window_open(ArgsView args);
    void window_when_title_changes(ArgsView args);
    void window_when_position_changes(ArgsView args);
    void window_when_size_changes(ArgsView args);

protected:
    Value displayType;
    Value windowType;

private:
    Value moduleTypeValue; // ObjModuleType*

    // raise a UIException with the given message within the VM
    void raiseException(const icu::UnicodeString& message);

    // Helper to call init() constructor on a newly created instance (if it exists)
    void callInit(const Value& instance, const Value& typeObj);

    std::once_flag glfwInitializedFlag;
    std::atomic<bool> glfwInitialized{false};
    std::atomic<bool> lvglInitialized{false};
    bool ensureGlfwInitialized();
};

}
