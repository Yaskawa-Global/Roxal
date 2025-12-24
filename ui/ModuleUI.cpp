#include "ModuleUI.h"
#include "../compiler/VM.h"
#include "../compiler/Object.h"

#include <GLFW/glfw3.h>
#include <lvgl.h>

using namespace roxal;

// Target frames per second for UI event polling (matches FPS in ui.rox)
static constexpr int FPS = 100;

ModuleUI::ModuleUI()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("ui")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleUI::~ModuleUI()
{
    // Note: Poll callback cleanup is handled by VM's ModulePoller

    if (lv_is_initialized()) {
        lv_deinit();
        // NOTE: We intentionally do NOT call glfwTerminate() here because:
        // 1. We may have hidden windows (LVGL bug workaround)
        // 2. GLFW will be terminated by the OS on process exit anyway
        // 3. Calling glfwTerminate() with allocated windows causes SEGFAULT
        //
        // if (glfwInitialized)
        //     glfwTerminate();
    }

    destroyModuleType(moduleTypeValue);
}

void ModuleUI::registerBuiltins(VM& vm)
{
    setVM(vm);

    link("_init", [this](VM& vm, ArgsView a){
        initialize();
        return Value::nilVal();
    });

    // Display methods
    linkMethod("Display", "create_window", [this](VM& vm, ArgsView a){ return display_create_window(a); });

    // Window methods
    linkMethod("Window", "close", [this](VM& vm, ArgsView a){ window_close(a); return Value::nilVal(); });
    linkMethod("Window", "open", [this](VM& vm, ArgsView a){ window_open(a); return Value::nilVal(); });
    linkMethod("Window", "when_title_changes", [this](VM& vm, ArgsView a){ window_when_title_changes(a); return Value::nilVal(); });
    linkMethod("Window", "when_position_changes", [this](VM& vm, ArgsView a){ window_when_position_changes(a); return Value::nilVal(); });
    linkMethod("Window", "when_size_changes", [this](VM& vm, ArgsView a){ window_when_size_changes(a); return Value::nilVal(); });
    linkMethod("Window", "set_root", [this](VM& vm, ArgsView a){ window_set_root(a); return Value::nilVal(); });

    // Widget base methods
    link("_widget_register", [this](VM& vm, ArgsView a){ widget_register(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_position", [this](VM& vm, ArgsView a){ widget_update_position(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_size", [this](VM& vm, ArgsView a){ widget_update_size(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_visible", [this](VM& vm, ArgsView a){ widget_update_visible(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_enabled", [this](VM& vm, ArgsView a){ widget_update_enabled(a); return Value::nilVal(); });

    // Label methods
    link("_label_create", [this](VM& vm, ArgsView a){ return label_create(a); });
    linkMethod("Label", "_update_text", [this](VM& vm, ArgsView a){ label_update_text(a); return Value::nilVal(); });

    // Button methods
    link("_button_create", [this](VM& vm, ArgsView a){ return button_create(a); });
    linkMethod("Button", "_update_label", [this](VM& vm, ArgsView a){ button_update_label(a); return Value::nilVal(); });

    // Slider methods
    link("_slider_create", [this](VM& vm, ArgsView a){ return slider_create(a); });
    linkMethod("Slider", "_update_value", [this](VM& vm, ArgsView a){ slider_update_value(a); return Value::nilVal(); });
    linkMethod("Slider", "_update_range", [this](VM& vm, ArgsView a){ slider_update_range(a); return Value::nilVal(); });

    // Layout methods
    link("_layout_create", [this](VM& vm, ArgsView a){ return layout_create(a); });
    linkMethod("Layout", "_add", [this](VM& vm, ArgsView a){ layout_add(a); return Value::nilVal(); });
    linkMethod("Layout", "_remove", [this](VM& vm, ArgsView a){ layout_remove(a); return Value::nilVal(); });

    // Note: Type references are cached in initialize() after the module script is parsed
}


void ModuleUI::initialize()
{
    // Cache type references (now that the module script has been parsed)
    displayType = uiType("Display").weakRef();
    windowType = uiType("Window").weakRef();
    widgetType = uiType("Widget").weakRef();
    labelType = uiType("Label").weakRef();
    buttonType = uiType("Button").weakRef();
    sliderType = uiType("Slider").weakRef();
    layoutType = uiType("Layout").weakRef();

    if (ensureGlfwInitialized()) {
        // populate the module "displays" list
        Value display = newUIObj("Display");

        // Set properties
        ObjectInstance* displayInst = asObjectInstance(display);
        displayInst->setProperty("id", Value::intVal(0));
        displayInst->setProperty("windows", Value::dictVal());

        auto displaysOpt = asModuleType(moduleType())->vars.load(toUnicodeString("displays"));
        if (displaysOpt) {
            auto displays { displaysOpt.value() };

            auto displaysList = asList(displays);
            displaysList->append(display);
        }

        // Register poll callback for UI event processing
        // This will be called periodically by the VM during wait() and execution
        const int64_t pollIntervalMicros = 1000000 / FPS;  // FPS defined in ui.rox (100)
        vm().registerPollCallback([this]() {
            glfwPollEvents();
            lv_timer_handler();
        }, pollIntervalMicros);
    }
    else
        LV_LOG_WARN("Unable to initialize glfw");
}


void ModuleUI::raiseException(const icu::UnicodeString& message)
{
    Value exType = vm().loadGlobal(toUnicodeString("UIException")).value();
    Value msg = Value::stringVal(message);
    Value exc = Value::exceptionVal(msg, exType);
    vm().raiseException(exc);
}


bool ModuleUI::ensureGlfwInitialized()
{
    try {
        std::call_once(glfwInitializedFlag, [this] {

            if (!glfwInit())
                throw std::runtime_error("Unable to initialize glfw UI back-end");
            glfwInitialized = true;

            // Hint: request an OpenGL ES context
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE); // TODO: can use true?

            lv_init();
            lvglInitialized = true;
        });
   } catch (std::exception& e) {
       raiseException(e.what());
       LV_LOG_ERROR("GLFW & lvgl Initialization failed: %s", e.what());
       return false;
   }
   return true;
}


Value ModuleUI::uiType(const std::string& typeName)
{
    // lookup type in module
    auto typeOpt = asModuleType(moduleType())->vars.load(toUnicodeString(typeName));
    if (!typeOpt.has_value())
        throw std::runtime_error(typeName+" type not found in ui module");

    return typeOpt.value();
}


Value ModuleUI::newUIObj(const std::string& typeName)
{
    Value typeObj { uiType(typeName) };
    // Verify it's an object type
    if (!isObjectType(typeObj))
        throw std::runtime_error(typeName+" is not an object type");

    return Value::objectInstanceVal(typeObj);
}


Value ModuleUI::newUIObj(const Value& typeObj)
{
    // Verify it's an object type
    if (!isObjectType(typeObj))
        throw std::runtime_error(typeObj.typeName()+" is not an object type");

    return Value::objectInstanceVal(typeObj);
}


void ModuleUI::callInit(const Value& instance, const Value& typeObj)
{
    if (!isObjectType(typeObj) || !isObjectInstance(instance))
        return;

    ObjObjectType* type = asObjectType(typeObj);

    // Look up the init method (searching up the inheritance hierarchy)
    ObjObjectType* tInit = type;
    const ObjObjectType::Method* initMethod = nullptr;
    ObjString* initString = asStringObj(Value::stringVal(UnicodeString("init")));

    while (tInit != nullptr && initMethod == nullptr) {
        auto it = tInit->methods.find(initString->hash);
        if (it != tInit->methods.end())
            initMethod = &it->second;
        else
            tInit = tInit->superType.isNil() ? nullptr : asObjectType(tInit->superType);
    }

    // If init() exists, call it
    if (initMethod != nullptr) {
        auto initClosureObj = asClosure(initMethod->closure);

        // For methods, we need to push the instance at the callee position (not as an argument)
        // so that slots[0] in the call frame will be 'this'.
        // We can't use callAndExec directly because it pushes the closure at the callee position.

        // Push the instance at the callee position (where the closure normally goes)
        vm().thread->push(instance);

        // Call the closure with 0 arguments (init takes no explicit parameters)
        CallSpec callSpec(0);
        if (!vm().call(initClosureObj, callSpec)) {
            // Call setup failed - clean up the stack
            vm().thread->pop();
            return;
        }

        // Execute the init method
        auto result = vm().execute();

        // Check for errors
        if (result.first != InterpretResult::OK) {
            // An error occurred during init execution
            // The VM will have already handled the error reporting
            return;
        }
    }
}


Value ModuleUI::display_create_window(ArgsView args)
{
    if (!ensureGlfwInitialized()) return Value::nilVal();

    debug_assert_msg(instanceOf(args[0], displayType), "instance is Display");
    Value& display { args[0] };
    auto displayInst { asObjectInstance(display) };

    Value window { newUIObj(windowType) };
    auto windowInst = asObjectInstance(window);

    // create lvgl window
    Value width { args[1] };
    Value height { args[2] };
    Value title { args[3] };
    Value open { args[4] };
    if (!width.isNumber() || !height.isNumber())
        throw std::runtime_error("width & height must be numeric");

    // Set window visibility hint before creating the window to prevent flash
    if (open.isBool() && !open.asBool()) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    lv_opengles_window_t* lv_window = lv_opengles_glfw_window_create(width.asInt(), height.asInt(), true);

    // Reset visibility hint to default for future windows
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (!lv_window) {
        LV_LOG_ERROR("Failed to create OpenGL ES window");
        raiseException("Unable to create window (lv_opengles_glfw)");
        return Value::nilVal();
    }

    // Override the window close callback to prevent automatic closing
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    glfwSetWindowCloseCallback(glfw_window, [](GLFWwindow* window) {
        // Prevent the window from closing - we'll handle it in Roxal
        glfwSetWindowShouldClose(window, GLFW_FALSE);
    });

    windowInst->setProperty("_lv_window", Value::foreignPtrVal(lv_window));

    lv_display_t* lv_display = lv_opengles_texture_create(width.asInt(), height.asInt());
    if (!lv_display) {
        LV_LOG_ERROR("Failed to create OpenGL ES texture");
        lv_opengles_window_delete(lv_window);
        raiseException("Unable to create window (lv_opengles_texture)");
        return Value::nilVal();
    }
    windowInst->setProperty("_lv_display", Value::foreignPtrVal(lv_display));

    // Store the screen for this display (auto-created by lv_display_create)
    lv_obj_t* screen = lv_screen_active();
    windowInst->setProperty("_lv_screen", Value::foreignPtrVal(screen));

    int32_t texture_id = lv_opengles_texture_get_texture_id(lv_display);
    lv_opengles_window_texture_t* window_texture = lv_opengles_window_add_texture(
        lv_window, texture_id,
        width.asInt(), height.asInt()
    );
    if (!window_texture) {
        LV_LOG_ERROR("Failed to obtain OpenGL ES texture id");
        lv_display_delete(lv_display);
        lv_opengles_window_delete(lv_window);
        raiseException("Unable to create window (lv_opengles_window_add_texture)");
        return Value::nilVal();
    }
    windowInst->setProperty("_texture_id", Value::intVal(texture_id));
    windowInst->setProperty("_window_texture", Value::foreignPtrVal(window_texture));

    // Set window title (or generate default using texture_id)
    std::string windowTitle;
    if (isString(title)) {
        asUString(title).toUTF8String(windowTitle);
        if (windowTitle.empty()) {
            windowTitle = "Roxal Window " + std::to_string(texture_id);
            windowInst->setProperty("title", Value::stringVal(toUnicodeString(windowTitle)));
        } else {
            windowInst->setProperty("title", title);
        }
    } else {
        throw std::runtime_error("title must be a string");
    }
    glfwSetWindowTitle(glfw_window, windowTitle.c_str());

    // Get and set initial window position
    int pos_x, pos_y;
    glfwGetWindowPos(glfw_window, &pos_x, &pos_y);
    windowInst->setProperty("x", Value::intVal(pos_x));
    windowInst->setProperty("y", Value::intVal(pos_y));

    // Set initial window size
    windowInst->setProperty("width", width);
    windowInst->setProperty("height", height);

    windowInst->setProperty("_display", display.weakRef()); // back ref to Display from Window

    // Add to ui.Display's windows list
    Value display_windows { displayInst->getProperty("windows") };
    debug_assert_msg(isDict(display_windows),"Display.windows is a dict");
    auto display_windowsDict = asDict(display_windows);

    if (display_windowsDict->length() == 0) // first window
        lv_display_set_default(lv_display);

    display_windowsDict->store(Value::intVal(texture_id),window);

    // Call init() constructor if it exists
    callInit(window, windowType);

    return window;
}


void ModuleUI::window_close(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    auto window_texture = (lv_opengles_window_texture_t*)(asForeignPtr(windowInst->getProperty("_window_texture"))->ptr);
    if (window_texture) {
        lv_opengles_window_texture_remove(window_texture);
        windowInst->setProperty("_window_texture",Value::nilVal());
    }

    auto lv_display = (lv_display_t*)(asForeignPtr(windowInst->getProperty("_lv_display"))->ptr);
    if (lv_display) {
        lv_display_delete(lv_display);
        windowInst->setProperty("_lv_display",Value::nilVal());
    }

    auto texture_id = windowInst->getProperty("_texture_id").asInt();

    // Check how many windows are open before deleting
    Value display = windowInst->getProperty("_display");
    size_t window_count = 0;
    if (display.isNonNil()) {
        auto displayInst { asObjectInstance(display) };
        Value display_windows { displayInst->getProperty("windows") };
        debug_assert_msg(isDict(display_windows),"Display.windows is a dict");
        auto display_windowsDict = asDict(display_windows);
        window_count = display_windowsDict->length();
    }

    auto lv_window = (lv_opengles_window_t*)(asForeignPtr(windowInst->getProperty("_lv_window"))->ptr);
    if (lv_window) {
        if (window_count > 1) {
            // Safe to delete - not the last window
            lv_opengles_window_delete(lv_window);
            windowInst->setProperty("_lv_window",Value::nilVal());
        } else {
            // WORKAROUND: Don't delete the last window to avoid LVGL bug (SEGFAULT)
            // Instead, just hide it
            GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
            glfwHideWindow(glfw_window);
            // Note: We intentionally leak lv_window to avoid the SEGFAULT
        }
    }

    // remove Window from Display's windows list
    if (display.isNonNil()) {
        auto displayInst { asObjectInstance(display) };

        Value display_windows { displayInst->getProperty("windows") };
        debug_assert_msg(isDict(display_windows),"Display.windows is a dict");
        auto display_windowsDict = asDict(display_windows);

        display_windowsDict->erase(Value::intVal(texture_id));
    }
}


void ModuleUI::window_open(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    // Get the lvgl window
    Value lv_window_val = windowInst->getProperty("_lv_window");
    if (lv_window_val.isNil())
        return; // Window has been closed

    auto lv_window = (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr);
    if (!lv_window)
        return;

    // Get the GLFW window
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    if (!glfw_window)
        return;

    // Apply the window position before showing (ensures position is set even if window manager reset it)
    Value x_val = windowInst->getProperty("x");
    Value y_val = windowInst->getProperty("y");
    if (x_val.isNumber() && y_val.isNumber()) {
        glfwSetWindowPos(glfw_window, x_val.asInt(), y_val.asInt());
    }

    // Show the window
    glfwShowWindow(glfw_window);
}


void ModuleUI::window_when_title_changes(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    Value& title { args[1] };

    auto windowInst { asObjectInstance(window) };

    // Get the lvgl window
    Value lv_window_val = windowInst->getProperty("_lv_window");
    if (lv_window_val.isNil())
        return; // Window has been closed

    auto lv_window = (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr);
    if (!lv_window)
        return;

    // Get the GLFW window
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    if (!glfw_window)
        return;

    // Convert title to std::string and set it
    std::string titleStr;
    if (isString(title)) {
        asUString(title).toUTF8String(titleStr);
        glfwSetWindowTitle(glfw_window, titleStr.c_str());
    }
}


void ModuleUI::window_when_position_changes(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    // Get the lvgl window
    Value lv_window_val = windowInst->getProperty("_lv_window");
    if (lv_window_val.isNil())
        return; // Window has been closed

    auto lv_window = (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr);
    if (!lv_window)
        return;

    // Get the GLFW window
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    if (!glfw_window)
        return;

    // Get x and y from window properties
    Value x_val = windowInst->getProperty("x");
    Value y_val = windowInst->getProperty("y");

    if (x_val.isNumber() && y_val.isNumber()) {
        glfwSetWindowPos(glfw_window, x_val.asInt(), y_val.asInt());
    }
}


void ModuleUI::window_when_size_changes(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    // Get the lvgl window
    Value lv_window_val = windowInst->getProperty("_lv_window");
    if (lv_window_val.isNil())
        return; // Window has been closed

    auto lv_window = (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr);
    if (!lv_window)
        return;

    // Get the GLFW window
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    if (!glfw_window)
        return;

    // Get width and height from window properties
    Value width_val = windowInst->getProperty("width");
    Value height_val = windowInst->getProperty("height");

    if (width_val.isNumber() && height_val.isNumber()) {
        glfwSetWindowSize(glfw_window, width_val.asInt(), height_val.asInt());
    }
}


void ModuleUI::window_set_root(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    Value& rootWidget { args[1] };
    auto windowInst { asObjectInstance(window) };

    // Get the screen for this window
    Value lv_screen_val = windowInst->getProperty("_lv_screen");
    if (lv_screen_val.isNil())
        return;

    lv_obj_t* screen = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
    if (!screen)
        return;

    // Store reference to root widget
    windowInst->setProperty("_root", rootWidget);

    // If the widget has an LVGL object, set its parent to the screen
    if (isObjectInstance(rootWidget)) {
        auto rootInst = asObjectInstance(rootWidget);
        Value lv_obj_val = rootInst->getProperty("_lv_obj");
        if (lv_obj_val.isNonNil() && isForeignPtr(lv_obj_val)) {
            lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
            if (lv_obj) {
                lv_obj_set_parent(lv_obj, screen);
            }
        }
    }
}


// ============================================================================
// WidgetRegistry Implementation
// ============================================================================

void WidgetRegistry::registerWidget(Value roxalWidget, void* lvObj)
{
    std::lock_guard<std::mutex> lock(mutex);
    // Store weak reference to the widget to avoid preventing GC
    toRoxal[lvObj] = roxalWidget.weakRef();
}

void WidgetRegistry::unregisterWidget(void* lvObj)
{
    std::lock_guard<std::mutex> lock(mutex);
    toRoxal.erase(lvObj);
}

void* WidgetRegistry::getNative(Value widget)
{
    std::lock_guard<std::mutex> lock(mutex);
    // Look through all entries to find the matching widget
    for (const auto& [lvObj, weakWidget] : toRoxal) {
        if (weakWidget.isAlive()) {
            Value strong = weakWidget.strongRef();
            if (strong == widget) {
                return lvObj;
            }
        }
    }
    return nullptr;
}

Value WidgetRegistry::getRoxal(void* lvObj)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = toRoxal.find(lvObj);
    if (it != toRoxal.end()) {
        if (it->second.isAlive()) {
            return it->second.strongRef();
        }
    }
    return Value::nilVal();
}

bool WidgetRegistry::hasNative(void* lvObj)
{
    std::lock_guard<std::mutex> lock(mutex);
    return toRoxal.find(lvObj) != toRoxal.end();
}


// ============================================================================
// LVGL Event Callback
// ============================================================================

void ModuleUI::lvglEventCallback(void* e)
{
    lv_event_t* event = (lv_event_t*)e;
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t* target = lv_event_get_target_obj(event);

    // Get the ModuleUI instance from user data
    ModuleUI* module = (ModuleUI*)lv_event_get_user_data(event);
    if (!module)
        return;

    // Look up the Roxal widget from the LVGL object
    // Note: For buttons, we might get the label child as target - try parent if needed
    Value roxalWidget = module->getWidgetRegistry().getRoxal(target);
    if (roxalWidget.isNil()) {
        // Try parent (for bubbled events from children like button labels)
        lv_obj_t* parent = lv_obj_get_parent(target);
        if (parent) {
            roxalWidget = module->getWidgetRegistry().getRoxal(parent);
        }
    }
    if (roxalWidget.isNil()) {
        return;
    }

    // Convert LVGL event to Roxal event based on event code
    switch (code) {
        case LV_EVENT_CLICKED:
            module->emitUIEvent("Clicked", roxalWidget);
            break;

        case LV_EVENT_PRESSED:
            module->emitUIEvent("Pressed", roxalWidget);
            break;

        case LV_EVENT_RELEASED:
            module->emitUIEvent("Released", roxalWidget);
            break;

        case LV_EVENT_LONG_PRESSED:
            module->emitUIEvent("LongPressed", roxalWidget);
            break;

        case LV_EVENT_VALUE_CHANGED: {
            // For sliders, include the value
            int32_t value = 0;
            if (lv_obj_check_type(target, &lv_slider_class)) {
                value = lv_slider_get_value(target);
            }
            module->emitUIEvent("ValueChanged", roxalWidget, {{"value", Value::intVal(value)}});
            break;
        }

        case LV_EVENT_FOCUSED:
            module->emitUIEvent("Focused", roxalWidget);
            break;

        case LV_EVENT_DEFOCUSED:
            module->emitUIEvent("Defocused", roxalWidget);
            break;

        default:
            // Ignore other events for now
            break;
    }
}


void ModuleUI::emitUIEvent(const std::string& eventTypeName, Value widget,
                           const std::map<std::string, Value>& extraPayload)
{
    // Look up the event type in the ui module
    auto eventTypeOpt = asModuleType(moduleType())->vars.load(toUnicodeString(eventTypeName));
    if (!eventTypeOpt.has_value()) {
        return;
    }

    Value eventType = eventTypeOpt.value();
    if (!isEventType(eventType)) {
        return;
    }

    ObjEventType* evType = asEventType(eventType);

    // Build the payload vector in the order defined by the event type
    // Base Event has: widget, timestamp
    // Subclasses add their own fields after these
    std::vector<Value> payload;

    // Add base Event fields
    payload.push_back(widget);  // widget
    payload.push_back(Value::intVal(static_cast<int64_t>(lv_tick_get())));  // timestamp

    // Add event-specific fields based on type name
    if (eventTypeName == "Clicked") {
        // Clicked has: x, y
        auto xIt = extraPayload.find("x");
        auto yIt = extraPayload.find("y");
        payload.push_back(xIt != extraPayload.end() ? xIt->second : Value::intVal(0));
        payload.push_back(yIt != extraPayload.end() ? yIt->second : Value::intVal(0));
    } else if (eventTypeName == "LongPressed") {
        // LongPressed has: duration
        auto durIt = extraPayload.find("duration");
        payload.push_back(durIt != extraPayload.end() ? durIt->second : Value::intVal(0));
    } else if (eventTypeName == "ValueChanged") {
        // ValueChanged has: value, previous
        auto valIt = extraPayload.find("value");
        auto prevIt = extraPayload.find("previous");
        payload.push_back(valIt != extraPayload.end() ? valIt->second : Value::intVal(0));
        payload.push_back(prevIt != extraPayload.end() ? prevIt->second : Value::intVal(0));
    } else if (eventTypeName == "WindowResize") {
        // WindowResize has: newWidth, newHeight
        auto wIt = extraPayload.find("newWidth");
        auto hIt = extraPayload.find("newHeight");
        payload.push_back(wIt != extraPayload.end() ? wIt->second : Value::intVal(0));
        payload.push_back(hIt != extraPayload.end() ? hIt->second : Value::intVal(0));
    }
    // Pressed, Released, Focused, Defocused, WindowClose have no extra fields

    // Create event instance with payload
    Value eventInstance = Value::eventInstanceVal(eventType, std::move(payload));

    // Schedule handlers on the main thread
    TimePoint now = TimePoint::currentTime();
    scheduleEventHandlers(eventType.weakRef(), evType, eventInstance, now);
}


// ============================================================================
// Widget Methods
// ============================================================================

void ModuleUI::widget_register(ArgsView args)
{
    Value& widget = args[0];
    if (!isObjectInstance(widget))
        return;

    auto widgetInst = asObjectInstance(widget);
    Value lv_obj_val = widgetInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_obj)
        return;

    // Register this widget in the widget registry
    widgetRegistry.registerWidget(widget, lv_obj);
}

void ModuleUI::widget_update_position(ArgsView args)
{
    // Base widget position update - subclasses may override
    Value& widget = args[0];
    if (!isObjectInstance(widget))
        return;

    auto widgetInst = asObjectInstance(widget);
    Value lv_obj_val = widgetInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_obj)
        return;

    Value x_val = widgetInst->getProperty("x");
    Value y_val = widgetInst->getProperty("y");

    if (x_val.isNumber() && y_val.isNumber()) {
        lv_obj_set_pos(lv_obj, x_val.asInt(), y_val.asInt());
    }
}

void ModuleUI::widget_update_size(ArgsView args)
{
    Value& widget = args[0];
    if (!isObjectInstance(widget))
        return;

    auto widgetInst = asObjectInstance(widget);
    Value lv_obj_val = widgetInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_obj)
        return;

    Value w_val = widgetInst->getProperty("width");
    Value h_val = widgetInst->getProperty("height");

    if (w_val.isNumber() && h_val.isNumber()) {
        lv_obj_set_size(lv_obj, w_val.asInt(), h_val.asInt());
    }
}

void ModuleUI::widget_update_visible(ArgsView args)
{
    Value& widget = args[0];
    if (!isObjectInstance(widget))
        return;

    auto widgetInst = asObjectInstance(widget);
    Value lv_obj_val = widgetInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_obj)
        return;

    Value visible_val = widgetInst->getProperty("visible");
    if (visible_val.isBool()) {
        if (visible_val.asBool()) {
            lv_obj_remove_flag(lv_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lv_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ModuleUI::widget_update_enabled(ArgsView args)
{
    Value& widget = args[0];
    if (!isObjectInstance(widget))
        return;

    auto widgetInst = asObjectInstance(widget);
    Value lv_obj_val = widgetInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_obj)
        return;

    Value enabled_val = widgetInst->getProperty("enabled");
    if (enabled_val.isBool()) {
        if (enabled_val.asBool()) {
            lv_obj_remove_state(lv_obj, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(lv_obj, LV_STATE_DISABLED);
        }
    }
}


// ============================================================================
// Label Widget
// ============================================================================

Value ModuleUI::label_create(ArgsView args)
{
    Value& parent = args[0];  // parent widget or nil for screen

    // Determine the LVGL parent
    lv_obj_t* lv_parent = nullptr;
    if (isObjectInstance(parent)) {
        auto parentInst = asObjectInstance(parent);
        Value lv_obj_val = parentInst->getProperty("_lv_obj");
        if (lv_obj_val.isNonNil() && isForeignPtr(lv_obj_val)) {
            lv_parent = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
        }
        // Also check for _lv_screen (for Window)
        if (!lv_parent) {
            Value lv_screen_val = parentInst->getProperty("_lv_screen");
            if (lv_screen_val.isNonNil() && isForeignPtr(lv_screen_val)) {
                lv_parent = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
            }
        }
    }

    if (!lv_parent) {
        lv_parent = lv_screen_active();
    }

    // Create the LVGL label
    lv_obj_t* lv_label = lv_label_create(lv_parent);
    if (!lv_label) {
        raiseException("Failed to create LVGL label");
        return Value::nilVal();
    }

    // Create Roxal Label instance
    Value label = newUIObj(labelType);
    auto labelInst = asObjectInstance(label);

    // Store the LVGL object reference
    labelInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_label));

    // Note: Widget registry registration happens in Roxal init() via _widget_register(this)

    // Add event callback
    lv_obj_add_event_cb(lv_label, (lv_event_cb_t)lvglEventCallback, LV_EVENT_ALL, this);

    return label;
}

void ModuleUI::label_update_text(ArgsView args)
{
    Value& label = args[0];
    if (!isObjectInstance(label))
        return;

    auto labelInst = asObjectInstance(label);
    Value lv_obj_val = labelInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_label = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_label)
        return;

    Value text_val = labelInst->getProperty("text");
    if (isString(text_val)) {
        std::string textStr;
        asUString(text_val).toUTF8String(textStr);
        lv_label_set_text(lv_label, textStr.c_str());
    }
}


// ============================================================================
// Button Widget
// ============================================================================

Value ModuleUI::button_create(ArgsView args)
{
    Value& parent = args[0];

    // Determine the LVGL parent
    lv_obj_t* lv_parent = nullptr;
    if (isObjectInstance(parent)) {
        auto parentInst = asObjectInstance(parent);
        Value lv_obj_val = parentInst->getProperty("_lv_obj");
        if (lv_obj_val.isNonNil() && isForeignPtr(lv_obj_val)) {
            lv_parent = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
        }
        if (!lv_parent) {
            Value lv_screen_val = parentInst->getProperty("_lv_screen");
            if (lv_screen_val.isNonNil() && isForeignPtr(lv_screen_val)) {
                lv_parent = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
            }
        }
    }

    if (!lv_parent) {
        lv_parent = lv_screen_active();
    }

    // Create the LVGL button
    lv_obj_t* lv_button = lv_button_create(lv_parent);
    if (!lv_button) {
        raiseException("Failed to create LVGL button");
        return Value::nilVal();
    }

    // Create a label inside the button
    lv_obj_t* lv_btn_label = lv_label_create(lv_button);
    lv_label_set_text(lv_btn_label, "Button");
    lv_obj_center(lv_btn_label);

    // Create Roxal Button instance
    Value button = newUIObj(buttonType);
    auto buttonInst = asObjectInstance(button);

    // Store the LVGL object references
    buttonInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_button));
    buttonInst->setProperty("_lv_label", Value::foreignPtrVal(lv_btn_label));

    // Note: Widget registry registration happens in Roxal init() via _widget_register(this)

    // Add event callbacks
    lv_obj_add_event_cb(lv_button, (lv_event_cb_t)lvglEventCallback, LV_EVENT_ALL, this);

    return button;
}

void ModuleUI::button_update_label(ArgsView args)
{
    Value& button = args[0];
    if (!isObjectInstance(button))
        return;

    auto buttonInst = asObjectInstance(button);
    Value lv_label_val = buttonInst->getProperty("_lv_label");
    if (lv_label_val.isNil() || !isForeignPtr(lv_label_val))
        return;

    lv_obj_t* lv_label = (lv_obj_t*)(asForeignPtr(lv_label_val)->ptr);
    if (!lv_label)
        return;

    Value label_text = buttonInst->getProperty("label");
    if (isString(label_text)) {
        std::string labelStr;
        asUString(label_text).toUTF8String(labelStr);
        lv_label_set_text(lv_label, labelStr.c_str());
    }
}


// ============================================================================
// Slider Widget
// ============================================================================

Value ModuleUI::slider_create(ArgsView args)
{
    Value& parent = args[0];

    // Determine the LVGL parent
    lv_obj_t* lv_parent = nullptr;
    if (isObjectInstance(parent)) {
        auto parentInst = asObjectInstance(parent);
        Value lv_obj_val = parentInst->getProperty("_lv_obj");
        if (lv_obj_val.isNonNil() && isForeignPtr(lv_obj_val)) {
            lv_parent = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
        }
        if (!lv_parent) {
            Value lv_screen_val = parentInst->getProperty("_lv_screen");
            if (lv_screen_val.isNonNil() && isForeignPtr(lv_screen_val)) {
                lv_parent = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
            }
        }
    }

    if (!lv_parent) {
        lv_parent = lv_screen_active();
    }

    // Create the LVGL slider
    lv_obj_t* lv_slider = lv_slider_create(lv_parent);
    if (!lv_slider) {
        raiseException("Failed to create LVGL slider");
        return Value::nilVal();
    }

    // Set default range
    lv_slider_set_range(lv_slider, 0, 100);
    lv_slider_set_value(lv_slider, 0, LV_ANIM_OFF);

    // Create Roxal Slider instance
    Value slider = newUIObj(sliderType);
    auto sliderInst = asObjectInstance(slider);

    // Store the LVGL object reference
    sliderInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_slider));

    // Note: Widget registry registration happens in Roxal init() via _widget_register(this)

    // Add event callbacks
    lv_obj_add_event_cb(lv_slider, (lv_event_cb_t)lvglEventCallback, LV_EVENT_ALL, this);

    return slider;
}

void ModuleUI::slider_update_value(ArgsView args)
{
    Value& slider = args[0];
    if (!isObjectInstance(slider))
        return;

    auto sliderInst = asObjectInstance(slider);
    Value lv_obj_val = sliderInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_slider = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_slider)
        return;

    Value value_val = sliderInst->getProperty("value");
    if (value_val.isNumber()) {
        lv_slider_set_value(lv_slider, value_val.asInt(), LV_ANIM_OFF);
    }
}

void ModuleUI::slider_update_range(ArgsView args)
{
    Value& slider = args[0];
    if (!isObjectInstance(slider))
        return;

    auto sliderInst = asObjectInstance(slider);
    Value lv_obj_val = sliderInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_slider = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_slider)
        return;

    Value min_val = sliderInst->getProperty("min");
    Value max_val = sliderInst->getProperty("max");

    if (min_val.isNumber() && max_val.isNumber()) {
        lv_slider_set_range(lv_slider, min_val.asInt(), max_val.asInt());
    }
}


// ============================================================================
// Layout Widget
// ============================================================================

Value ModuleUI::layout_create(ArgsView args)
{
    Value& parent = args[0];

    // Determine the LVGL parent
    lv_obj_t* lv_parent = nullptr;
    if (isObjectInstance(parent)) {
        auto parentInst = asObjectInstance(parent);
        Value lv_obj_val = parentInst->getProperty("_lv_obj");
        if (lv_obj_val.isNonNil() && isForeignPtr(lv_obj_val)) {
            lv_parent = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
        }
        if (!lv_parent) {
            Value lv_screen_val = parentInst->getProperty("_lv_screen");
            if (lv_screen_val.isNonNil() && isForeignPtr(lv_screen_val)) {
                lv_parent = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
            }
        }
    }

    if (!lv_parent) {
        lv_parent = lv_screen_active();
    }

    // Create the LVGL layout (base object)
    lv_obj_t* lv_layout = lv_obj_create(lv_parent);
    if (!lv_layout) {
        raiseException("Failed to create LVGL layout");
        return Value::nilVal();
    }

    // Create Roxal Layout instance
    Value layout = newUIObj(layoutType);
    auto layoutInst = asObjectInstance(layout);

    // Store the LVGL object reference
    layoutInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_layout));

    // Initialize children list
    layoutInst->setProperty("children", Value::listVal());

    // Note: Widget registry registration happens in Roxal init() via _widget_register(this)

    return layout;
}

void ModuleUI::layout_add(ArgsView args)
{
    Value& layout = args[0];
    Value& child = args[1];

    if (!isObjectInstance(layout) || !isObjectInstance(child))
        return;

    auto layoutInst = asObjectInstance(layout);
    auto childInst = asObjectInstance(child);

    // Get LVGL objects
    Value layout_lv_val = layoutInst->getProperty("_lv_obj");
    Value child_lv_val = childInst->getProperty("_lv_obj");

    if (layout_lv_val.isNil() || child_lv_val.isNil())
        return;

    lv_obj_t* lv_layout = (lv_obj_t*)(asForeignPtr(layout_lv_val)->ptr);
    lv_obj_t* lv_child = (lv_obj_t*)(asForeignPtr(child_lv_val)->ptr);

    if (!lv_layout || !lv_child)
        return;

    // Set parent in LVGL - the Roxal add() method handles the children list
    lv_obj_set_parent(lv_child, lv_layout);
}

void ModuleUI::layout_remove(ArgsView args)
{
    Value& layout = args[0];
    Value& child = args[1];

    if (!isObjectInstance(layout) || !isObjectInstance(child))
        return;

    auto layoutInst = asObjectInstance(layout);

    // Remove from Roxal children list
    Value children = layoutInst->getProperty("children");
    if (isList(children)) {
        auto list = asList(children);
        // Erase by value
        list->elts.erase(child);
    }

    // Note: We don't delete the LVGL object here - it may be re-parented
}
