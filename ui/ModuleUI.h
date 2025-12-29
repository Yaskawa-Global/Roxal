#pragma once

#include "../compiler/BuiltinModule.h"
#include "../compiler/Value.h"
#include "../compiler/Object.h"

#include <functional>

namespace roxal {

// Forward declarations
class ModuleUI;


// Widget registry - maps between Roxal widgets and LVGL objects
class WidgetRegistry {
public:
    // Register a mapping between Roxal widget and LVGL object
    void registerWidget(Value roxalWidget, void* lvObj);

    // Unregister a widget (called when Roxal widget is destroyed)
    void unregisterWidget(void* lvObj);

    // Lookup LVGL object from Roxal widget
    void* getNative(Value widget);

    // Lookup Roxal widget from LVGL object (returns strong ref or nil if dead)
    Value getRoxal(void* lvObj);

    // Check if a native object is registered
    bool hasNative(void* lvObj);

private:
    std::mutex mutex;
    // lv_obj_t* -> Roxal Widget (weak ref to avoid preventing GC)
    std::unordered_map<void*, Value> toRoxal;
};


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
    void display_show_perf_monitor(ArgsView args);
    void display_hide_perf_monitor(ArgsView args);
    void display_show_mem_monitor(ArgsView args);
    void display_hide_mem_monitor(ArgsView args);

    void window_close(ArgsView args);
    void window_open(ArgsView args);
    void window_when_title_changes(ArgsView args);
    void window_when_position_changes(ArgsView args);
    void window_when_size_changes(ArgsView args);
    void window_set_root(ArgsView args);
    Value window_capture(ArgsView args);

    // Window simulation methods (for UI testing)
    void window_simulate_click(ArgsView args);
    void window_simulate_key(ArgsView args);
    void window_simulate_text(ArgsView args);
    void window_simulate_close(ArgsView args);

    // Snapshot methods
    void snapshot_save(ArgsView args);
    void snapshot_release(ArgsView args);

    // Widget methods
    void widget_register(ArgsView args);
    void widget_update_position(ArgsView args);
    void widget_update_size(ArgsView args);
    void widget_update_visible(ArgsView args);
    void widget_update_enabled(ArgsView args);
    void widget_update_scrollbar(ArgsView args);
    void widget_update_scroll_dir(ArgsView args);
    void widget_simulate_click(ArgsView args);

    // Label methods
    Value label_create(ArgsView args);
    void label_update_text(ArgsView args);

    // Image methods
    Value image_create(ArgsView args);
    void image_update_src(ArgsView args);
    void image_update_align(ArgsView args);

    // Button methods
    Value button_create(ArgsView args);
    void button_update_label(ArgsView args);

    // Checkbox methods
    Value checkbox_create(ArgsView args);
    void checkbox_update_text(ArgsView args);
    void checkbox_update_checked(ArgsView args);

    // Switch methods
    Value switch_create(ArgsView args);
    void switch_update_state(ArgsView args);

    // TextArea methods
    Value textarea_create(ArgsView args);
    void textarea_update_text(ArgsView args);
    void textarea_update_placeholder(ArgsView args);
    void textarea_update_one_line(ArgsView args);
    void textarea_update_password(ArgsView args);
    void textarea_update_max_length(ArgsView args);

    // Slider methods
    Value slider_create(ArgsView args);
    void slider_update_value(ArgsView args);
    void slider_update_range(ArgsView args);

    // Layout methods
    Value layout_create(ArgsView args);
    void layout_add(ArgsView args);
    void layout_remove(ArgsView args);
    void layout_update_mode(ArgsView args);
    void layout_update_padding(ArgsView args);
    void layout_update_gap(ArgsView args);
    void layout_update_border_width(ArgsView args);

    // Access to widget registry
    WidgetRegistry& getWidgetRegistry() { return widgetRegistry; }

    // Emit a UI event from native code
    void emitUIEvent(const std::string& eventTypeName, Value widget,
                     const std::map<std::string, Value>& payload = {});

protected:
    Value displayType;
    Value windowType;
    Value snapshotType;
    Value widgetType;
    Value labelType;
    Value imageType;
    Value buttonType;
    Value checkboxType;
    Value switchType;
    Value textareaType;
    Value sliderType;
    Value layoutType;

private:
    Value moduleTypeValue; // ObjModuleType*

    // Widget registry for Roxal <-> LVGL mapping
    WidgetRegistry widgetRegistry;

    // raise a UIException with the given message within the VM
    void raiseException(const icu::UnicodeString& message);

    // Helper to call init() constructor on a newly created instance (if it exists)
    void callInit(const Value& instance, const Value& typeObj);

    // LVGL event callback (static so it can be used as C callback)
    static void lvglEventCallback(void* e);

    std::once_flag glfwInitializedFlag;
    std::atomic<bool> glfwInitialized{false};
    std::atomic<bool> lvglInitialized{false};
    bool ensureGlfwInitialized();
};

}
