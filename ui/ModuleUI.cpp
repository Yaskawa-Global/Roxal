#include "ModuleUI.h"
#include "../compiler/VM.h"
#include "../compiler/Object.h"
#include "../compiler/RuntimeConfig.h"

#include <GL/glew.h>  // Must be before GLFW
#include <GLFW/glfw3.h>
#include <lvgl.h>
#include <png.h>

using namespace roxal;

// ============================================================================
// Architecture: Roxal UI / GLFW / LVGL Relationship
// ============================================================================
//
// Roxal UI Layer (ui.rox):
//   - Display: Represents a physical display/monitor. Contains a dict of Windows.
//              Currently there's typically one Display (displays[0]).
//   - Window:  A rendering surface that can contain UI widgets.
//
// Native Implementation (this file):
//   Each Roxal Window creates:
//   - lv_display_t* (_lv_display): LVGL display/rendering context with draw buffers
//   - lv_opengles_window_t* (_lv_window): GLFW window wrapper
//   - lv_obj_t* (_lv_screen): The root LVGL screen object for this display
//
// All Windows use GLFW + OpenGL rendering (LVGL is compiled with LV_USE_OPENGLES=1).
// GLFW handles: window management, input events, OpenGL context
// LVGL renders to OpenGL texture, displayed via GLFW window
//
// Offscreen Windows (offscreen=true or --offscreen CLI flag):
//   Same rendering pipeline as visible windows, but the GLFW window is never shown.
//   Used for automated UI testing, screenshot comparison, CI environments.
//   For truly headless environments (no display), use Xvfb or similar.
//
// Note: Pure software rendering (no OpenGL) would require rebuilding LVGL with
// LV_USE_OPENGLES=0, LV_USE_GLFW=0 in lv_conf.h.
// ============================================================================

// Target frames per second for UI event polling (matches FPS in ui.rox)
static constexpr int FPS = 100;

// ============================================================================
// Keyboard Input Support
// ============================================================================

// Get the size of a UTF-8 encoded character from its first byte
static inline uint32_t utf8_char_size(const char* txt) {
    if (txt == nullptr || *txt == '\0') return 0;
    uint8_t c = (uint8_t)*txt;
    if ((c & 0x80) == 0) return 1;       // 0xxxxxxx - ASCII
    if ((c & 0xE0) == 0xC0) return 2;    // 110xxxxx - 2 byte
    if ((c & 0xF0) == 0xE0) return 3;    // 1110xxxx - 3 byte
    if ((c & 0xF8) == 0xF0) return 4;    // 11110xxx - 4 byte
    return 1;  // Invalid UTF-8, treat as single byte
}

static constexpr size_t KEYBOARD_BUFFER_SIZE = 64;

struct KeyboardState {
    char buf[KEYBOARD_BUFFER_SIZE] = {0};
    bool dummy_read = false;
    lv_indev_t* indev = nullptr;
    lv_display_t* disp = nullptr;
    lv_group_t* group = nullptr;  // Input group for keyboard navigation
};

// Global keyboard state (one per window would be better, but this works for now)
static KeyboardState g_keyboard;

// ============================================================================
// Window Event Support
// ============================================================================

// Queued window event - simple struct with all needed data
struct QueuedWindowEvent {
    std::string eventName;
    ModuleUI* module;
    Value window;  // Weak reference to Roxal Window object
    std::map<std::string, Value> payload;
};

// Data associated with each GLFW window for event callbacks
struct WindowUserData {
    ModuleUI* module = nullptr;
    Value window;  // Weak reference to Roxal Window object
    lv_display_t* lv_display = nullptr;  // For resize handling
    lv_opengles_window_texture_t* window_texture = nullptr;  // For resize handling
};

// Global registry mapping GLFWwindow* to WindowUserData
// NOTE: We cannot use glfwSetWindowUserPointer because LVGL uses it for its own data!
// All access is single-threaded (UI poll thread), so no mutex needed
static std::unordered_map<GLFWwindow*, ptr<WindowUserData>> g_windowUserDataMap;
static std::vector<QueuedWindowEvent> g_pendingWindowEvents;

// Pending resize operation (processed after glfwPollEvents to avoid re-entrancy)
struct PendingResize {
    lv_display_t* display;
    int32_t width;
    int32_t height;
};
static std::vector<PendingResize> g_pendingResizes;

// Look up WindowUserData for a GLFW window
static WindowUserData* getWindowUserData(GLFWwindow* window) {
    auto it = g_windowUserDataMap.find(window);
    return (it != g_windowUserDataMap.end()) ? it->second.get() : nullptr;
}

// Queue a window event (called from GLFW callbacks during glfwPollEvents)
static void queueWindowEvent(GLFWwindow* window, const std::string& eventName,
                             const std::map<std::string, Value>& payload = {}) {
    WindowUserData* userData = getWindowUserData(window);
    if (!userData || !userData->module) return;
    g_pendingWindowEvents.push_back({eventName, userData->module, userData->window, payload});
}

// Process all queued window events (called after glfwPollEvents returns)
static void processQueuedWindowEvents() {
    // Move events to local vector in case processing triggers more events
    auto events = std::move(g_pendingWindowEvents);
    g_pendingWindowEvents.clear();

    for (auto& evt : events) {
        if (!evt.module) continue;
        Value roxalWindow = evt.window.isWeak() ? evt.window.strongRef() : evt.window;
        if (roxalWindow.isNil()) continue;
        evt.module->emitUIEvent(evt.eventName, roxalWindow, evt.payload);
    }
}

// Process pending resize operations (called after glfwPollEvents, before lv_timer_handler)
static void processQueuedResizes() {
    // NOTE: LVGL's lv_opengles_texture_reshape doesn't properly update display buffers,
    // causing assertions when enlarging windows. For now, we skip the LVGL resize.
    // The WindowResize event is still emitted so Roxal code can handle it.
    // TODO: Implement proper resize by recreating display/texture or fixing LVGL driver.
    g_pendingResizes.clear();
}

// Forward declarations for GLFW callbacks
static void glfw_window_close_callback(GLFWwindow* window);
static void glfw_window_size_callback(GLFWwindow* window, int width, int height);
static void glfw_window_pos_callback(GLFWwindow* window, int xpos, int ypos);
static void glfw_window_focus_callback(GLFWwindow* window, int focused);
static void glfw_window_iconify_callback(GLFWwindow* window, int iconified);

// Convert GLFW key to LVGL control key
static uint32_t glfw_key_to_lv_key(int key) {
    switch (key) {
        case GLFW_KEY_RIGHT:      return LV_KEY_RIGHT;
        case GLFW_KEY_LEFT:       return LV_KEY_LEFT;
        case GLFW_KEY_UP:         return LV_KEY_UP;
        case GLFW_KEY_DOWN:       return LV_KEY_DOWN;
        case GLFW_KEY_ESCAPE:     return LV_KEY_ESC;
        case GLFW_KEY_BACKSPACE:  return LV_KEY_BACKSPACE;
        case GLFW_KEY_DELETE:     return LV_KEY_DEL;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:   return LV_KEY_ENTER;
        case GLFW_KEY_TAB:        return LV_KEY_NEXT;
        case GLFW_KEY_HOME:       return LV_KEY_HOME;
        case GLFW_KEY_END:        return LV_KEY_END;
        default:                  return 0;
    }
}

// Keyboard read callback for LVGL
static void keyboard_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    size_t len = strlen(g_keyboard.buf);

    // Send a release after each press
    if (g_keyboard.dummy_read) {
        g_keyboard.dummy_read = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    // Send the pressed character
    else if (len > 0) {
        g_keyboard.dummy_read = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = 0;

        // Copy the first UTF8 character from the buffer
        uint32_t utf8_len = utf8_char_size(g_keyboard.buf);
        if (utf8_len == 0) utf8_len = 1;
        memcpy(&data->key, g_keyboard.buf, utf8_len);

        // Drop the first character
        memmove(g_keyboard.buf, g_keyboard.buf + utf8_len, len - utf8_len + 1);
    }
}

// GLFW key callback - handles control keys
static void glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)window; (void)scancode; (void)mods;

    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return;

    uint32_t lv_key = glfw_key_to_lv_key(key);
    if (lv_key == 0)
        return;

    size_t len = strlen(g_keyboard.buf);
    if (len < KEYBOARD_BUFFER_SIZE - 1) {
        g_keyboard.buf[len] = (char)lv_key;
        g_keyboard.buf[len + 1] = '\0';
    }

    // Trigger LVGL to read the keyboard
    if (g_keyboard.indev) {
        lv_indev_read(g_keyboard.indev);
        lv_indev_read(g_keyboard.indev);  // Second call for dummy read
    }
}

// GLFW character callback - handles text input
static void glfw_char_callback(GLFWwindow* window, unsigned int codepoint) {
    (void)window;

    // Convert Unicode codepoint to UTF-8
    char utf8[5] = {0};
    if (codepoint < 0x80) {
        utf8[0] = (char)codepoint;
    } else if (codepoint < 0x800) {
        utf8[0] = (char)(0xC0 | (codepoint >> 6));
        utf8[1] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        utf8[0] = (char)(0xE0 | (codepoint >> 12));
        utf8[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        utf8[0] = (char)(0xF0 | (codepoint >> 18));
        utf8[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (codepoint & 0x3F));
    }

    size_t buf_len = strlen(g_keyboard.buf);
    size_t utf8_len = strlen(utf8);
    if (buf_len + utf8_len < KEYBOARD_BUFFER_SIZE - 1) {
        strcat(g_keyboard.buf, utf8);
    }

    // Trigger LVGL to read the keyboard
    if (g_keyboard.indev) {
        lv_indev_read(g_keyboard.indev);
        lv_indev_read(g_keyboard.indev);  // Second call for dummy read
    }
}

// ============================================================================
// GLFW Window Event Callbacks
// ============================================================================
// These callbacks queue events instead of emitting directly to avoid
// re-entrancy issues with LVGL during glfwPollEvents()

static void glfw_window_close_callback(GLFWwindow* window) {
    // Prevent the window from closing - Roxal code must explicitly call close()
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    queueWindowEvent(window, "WindowClose");
}

static void glfw_window_size_callback(GLFWwindow* window, int width, int height) {
    // Queue LVGL resize operation (processed after glfwPollEvents to avoid re-entrancy)
    WindowUserData* userData = getWindowUserData(window);
    if (userData && userData->lv_display && width > 0 && height > 0) {
        g_pendingResizes.push_back({userData->lv_display, width, height});
    }

    // Queue event for Roxal code to handle
    queueWindowEvent(window, "WindowResize", {
        {"newWidth", Value::intVal(width)},
        {"newHeight", Value::intVal(height)}
    });
}

static void glfw_window_pos_callback(GLFWwindow* window, int xpos, int ypos) {
    queueWindowEvent(window, "WindowMove", {
        {"newX", Value::intVal(xpos)},
        {"newY", Value::intVal(ypos)}
    });
}

static void glfw_window_focus_callback(GLFWwindow* window, int focused) {
    queueWindowEvent(window, focused ? "WindowFocus" : "WindowDefocus");
}

static void glfw_window_iconify_callback(GLFWwindow* window, int iconified) {
    queueWindowEvent(window, iconified ? "WindowMinimize" : "WindowRestore");
}

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
    linkMethod("Display", "show_perf_monitor", [this](VM& vm, ArgsView a){ display_show_perf_monitor(a); return Value::nilVal(); });
    linkMethod("Display", "hide_perf_monitor", [this](VM& vm, ArgsView a){ display_hide_perf_monitor(a); return Value::nilVal(); });
    linkMethod("Display", "show_mem_monitor", [this](VM& vm, ArgsView a){ display_show_mem_monitor(a); return Value::nilVal(); });
    linkMethod("Display", "hide_mem_monitor", [this](VM& vm, ArgsView a){ display_hide_mem_monitor(a); return Value::nilVal(); });

    // Window methods
    linkMethod("Window", "close", [this](VM& vm, ArgsView a){ window_close(a); return Value::nilVal(); });
    linkMethod("Window", "open", [this](VM& vm, ArgsView a){ window_open(a); return Value::nilVal(); });
    linkMethod("Window", "when_title_changes", [this](VM& vm, ArgsView a){ window_when_title_changes(a); return Value::nilVal(); });
    linkMethod("Window", "when_position_changes", [this](VM& vm, ArgsView a){ window_when_position_changes(a); return Value::nilVal(); });
    linkMethod("Window", "when_size_changes", [this](VM& vm, ArgsView a){ window_when_size_changes(a); return Value::nilVal(); });
    linkMethod("Window", "set_root", [this](VM& vm, ArgsView a){ window_set_root(a); return Value::nilVal(); });
    linkMethod("Window", "capture", [this](VM& vm, ArgsView a){ return window_capture(a); });

    // Window simulation methods (for UI testing)
    linkMethod("Window", "simulate_click", [this](VM& vm, ArgsView a){ window_simulate_click(a); return Value::nilVal(); });
    linkMethod("Window", "simulate_key", [this](VM& vm, ArgsView a){ window_simulate_key(a); return Value::nilVal(); });
    linkMethod("Window", "simulate_text", [this](VM& vm, ArgsView a){ window_simulate_text(a); return Value::nilVal(); });
    linkMethod("Window", "simulate_close", [this](VM& vm, ArgsView a){ window_simulate_close(a); return Value::nilVal(); });

    // Snapshot methods
    linkMethod("Snapshot", "save", [this](VM& vm, ArgsView a){ snapshot_save(a); return Value::nilVal(); });
    linkMethod("Snapshot", "release", [this](VM& vm, ArgsView a){ snapshot_release(a); return Value::nilVal(); });

    // Widget base methods
    link("_widget_register", [this](VM& vm, ArgsView a){ widget_register(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_position", [this](VM& vm, ArgsView a){ widget_update_position(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_size", [this](VM& vm, ArgsView a){ widget_update_size(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_visible", [this](VM& vm, ArgsView a){ widget_update_visible(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_enabled", [this](VM& vm, ArgsView a){ widget_update_enabled(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_scrollbar", [this](VM& vm, ArgsView a){ widget_update_scrollbar(a); return Value::nilVal(); });
    linkMethod("Widget", "_update_scroll_dir", [this](VM& vm, ArgsView a){ widget_update_scroll_dir(a); return Value::nilVal(); });
    linkMethod("Widget", "simulate_click", [this](VM& vm, ArgsView a){ widget_simulate_click(a); return Value::nilVal(); });

    // Label methods
    link("_label_create", [this](VM& vm, ArgsView a){ return label_create(a); });
    linkMethod("Label", "_update_text", [this](VM& vm, ArgsView a){ label_update_text(a); return Value::nilVal(); });

    // Image methods
    link("_image_create", [this](VM& vm, ArgsView a){ return image_create(a); });
    linkMethod("Image", "_update_src", [this](VM& vm, ArgsView a){ image_update_src(a); return Value::nilVal(); });
    linkMethod("Image", "_update_align", [this](VM& vm, ArgsView a){ image_update_align(a); return Value::nilVal(); });

    // Button methods
    link("_button_create", [this](VM& vm, ArgsView a){ return button_create(a); });
    linkMethod("Button", "_update_label", [this](VM& vm, ArgsView a){ button_update_label(a); return Value::nilVal(); });

    // Checkbox methods
    link("_checkbox_create", [this](VM& vm, ArgsView a){ return checkbox_create(a); });
    linkMethod("Checkbox", "_update_text", [this](VM& vm, ArgsView a){ checkbox_update_text(a); return Value::nilVal(); });
    linkMethod("Checkbox", "_update_checked", [this](VM& vm, ArgsView a){ checkbox_update_checked(a); return Value::nilVal(); });

    // Switch methods
    link("_switch_create", [this](VM& vm, ArgsView a){ return switch_create(a); });
    linkMethod("Switch", "_update_state", [this](VM& vm, ArgsView a){ switch_update_state(a); return Value::nilVal(); });

    // TextArea methods
    link("_textarea_create", [this](VM& vm, ArgsView a){ return textarea_create(a); });
    linkMethod("TextArea", "_update_text", [this](VM& vm, ArgsView a){ textarea_update_text(a); return Value::nilVal(); });
    linkMethod("TextArea", "_update_placeholder", [this](VM& vm, ArgsView a){ textarea_update_placeholder(a); return Value::nilVal(); });
    linkMethod("TextArea", "_update_one_line", [this](VM& vm, ArgsView a){ textarea_update_one_line(a); return Value::nilVal(); });
    linkMethod("TextArea", "_update_password", [this](VM& vm, ArgsView a){ textarea_update_password(a); return Value::nilVal(); });
    linkMethod("TextArea", "_update_max_length", [this](VM& vm, ArgsView a){ textarea_update_max_length(a); return Value::nilVal(); });

    // Slider methods
    link("_slider_create", [this](VM& vm, ArgsView a){ return slider_create(a); });
    linkMethod("Slider", "_update_value", [this](VM& vm, ArgsView a){ slider_update_value(a); return Value::nilVal(); });
    linkMethod("Slider", "_update_range", [this](VM& vm, ArgsView a){ slider_update_range(a); return Value::nilVal(); });

    // Layout methods
    link("_layout_create", [this](VM& vm, ArgsView a){ return layout_create(a); });
    linkMethod("Layout", "_add", [this](VM& vm, ArgsView a){ layout_add(a); return Value::nilVal(); });
    linkMethod("Layout", "_remove", [this](VM& vm, ArgsView a){ layout_remove(a); return Value::nilVal(); });
    linkMethod("Layout", "_update_mode", [this](VM& vm, ArgsView a){ layout_update_mode(a); return Value::nilVal(); });
    linkMethod("Layout", "_update_padding", [this](VM& vm, ArgsView a){ layout_update_padding(a); return Value::nilVal(); });
    linkMethod("Layout", "_update_gap", [this](VM& vm, ArgsView a){ layout_update_gap(a); return Value::nilVal(); });
    linkMethod("Layout", "_update_border_width", [this](VM& vm, ArgsView a){ layout_update_border_width(a); return Value::nilVal(); });

    // Note: Type references are cached in initialize() after the module script is parsed
}


void ModuleUI::initialize()
{
    // Cache type references (now that the module script has been parsed)
    displayType = uiType("Display").weakRef();
    windowType = uiType("Window").weakRef();
    snapshotType = uiType("Snapshot").weakRef();
    widgetType = uiType("Widget").weakRef();
    labelType = uiType("Label").weakRef();
    imageType = uiType("Image").weakRef();
    buttonType = uiType("Button").weakRef();
    checkboxType = uiType("Checkbox").weakRef();
    switchType = uiType("Switch").weakRef();
    textareaType = uiType("TextArea").weakRef();
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
            // Process resize operations before LVGL rendering (must happen before lv_timer_handler)
            processQueuedResizes();
            // Process any window events that were queued during glfwPollEvents
            processQueuedWindowEvents();
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

    // Get parameters
    Value width { args[1] };
    Value height { args[2] };
    Value title { args[3] };
    Value open { args[4] };
    Value resizable { args[5] };
    Value offscreenArg { args[6] };

    if (!width.isNumber() || !height.isNumber())
        throw std::runtime_error("width & height must be numeric");

    // Determine if this should be an offscreen (hidden) window:
    // 1. Global override from --offscreen CLI flag
    // 2. Per-window offscreen parameter
    bool isOffscreen = RuntimeConfig::getBool("ui.offscreen", false);
    if (!isOffscreen && offscreenArg.isBool()) {
        isOffscreen = offscreenArg.asBool();
    }

    // For offscreen windows, force them to start hidden
    bool shouldOpen = open.isBool() ? open.asBool() : false;
    if (isOffscreen) {
        shouldOpen = false;  // Never show offscreen windows
    }

    int32_t w = width.asInt();
    int32_t h = height.asInt();

    // Set window visibility hint before creating the window to prevent flash
    if (!shouldOpen) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    // Set resizable hint before creating the window
    if (resizable.isBool() && !resizable.asBool()) {
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    }

    lv_opengles_window_t* lv_window = lv_opengles_glfw_window_create(w, h, true);

    // Reset window hints to default for future windows
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    if (!lv_window) {
        LV_LOG_ERROR("Failed to create OpenGL ES window");
        raiseException("Unable to create window (lv_opengles_glfw)");
        return Value::nilVal();
    }

    // Get the underlying GLFW window
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);

    // Apply resizable attribute after window creation (more reliable than hints on some platforms)
    if (resizable.isBool() && !resizable.asBool()) {
        glfwSetWindowAttrib(glfw_window, GLFW_RESIZABLE, GLFW_FALSE);
    }

    // Create user data for GLFW callbacks (stored in map, cleaned up in window_close)
    // NOTE: We use a map instead of glfwSetWindowUserPointer because LVGL uses it!
    ptr<WindowUserData> userData = make_ptr<WindowUserData>();
    userData->module = this;
    userData->window = window.weakRef();
    g_windowUserDataMap[glfw_window] = userData;

    // Set up window event callbacks
    glfwSetWindowCloseCallback(glfw_window, glfw_window_close_callback);
    glfwSetWindowSizeCallback(glfw_window, glfw_window_size_callback);
    glfwSetWindowPosCallback(glfw_window, glfw_window_pos_callback);
    glfwSetWindowFocusCallback(glfw_window, glfw_window_focus_callback);
    glfwSetWindowIconifyCallback(glfw_window, glfw_window_iconify_callback);

    // Set up keyboard callbacks for text input
    glfwSetKeyCallback(glfw_window, glfw_key_callback);
    glfwSetCharCallback(glfw_window, glfw_char_callback);

    windowInst->setProperty("_lv_window", Value::foreignPtrVal(lv_window));
    windowInst->setProperty("offscreen", Value::boolVal(isOffscreen));
    // Note: userData is kept alive by g_windowUserDataMap, not stored here

    lv_display_t* lv_display = lv_opengles_texture_create(w, h);
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
        w, h
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

    // Update WindowUserData with LVGL pointers for resize handling
    userData->lv_display = lv_display;
    userData->window_texture = window_texture;

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

    if (display_windowsDict->length() == 0) { // first window
        lv_display_set_default(lv_display);

        // Create keyboard input device for text input
        if (g_keyboard.indev == nullptr) {
            // Create input group for keyboard navigation
            g_keyboard.group = lv_group_create();
            lv_group_set_default(g_keyboard.group);

            g_keyboard.indev = lv_indev_create();
            if (g_keyboard.indev) {
                lv_indev_set_type(g_keyboard.indev, LV_INDEV_TYPE_KEYPAD);
                lv_indev_set_read_cb(g_keyboard.indev, keyboard_read_cb);
                lv_indev_set_mode(g_keyboard.indev, LV_INDEV_MODE_EVENT);
                lv_indev_set_display(g_keyboard.indev, lv_display);
                lv_indev_set_group(g_keyboard.indev, g_keyboard.group);
                g_keyboard.disp = lv_display;
            }
        }

        // Hide auto-created monitors by default (user can show them via API)
#if LV_USE_SYSMON && LV_USE_PERF_MONITOR
        lv_sysmon_hide_performance(lv_display);
#endif
#if LV_USE_SYSMON && LV_USE_MEM_MONITOR
        lv_sysmon_hide_memory(lv_display);
#endif
    }

    display_windowsDict->store(Value::intVal(texture_id),window);

    // Call init() constructor if it exists
    callInit(window, windowType);

    return window;
}


void ModuleUI::display_show_perf_monitor(ArgsView args)
{
#if LV_USE_SYSMON
    debug_assert_msg(instanceOf(args[0], displayType), "instance is Display");
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        lv_sysmon_show_performance(disp);
    }
#endif
}

void ModuleUI::display_hide_perf_monitor(ArgsView args)
{
#if LV_USE_SYSMON
    debug_assert_msg(instanceOf(args[0], displayType), "instance is Display");
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        lv_sysmon_hide_performance(disp);
    }
#endif
}

void ModuleUI::display_show_mem_monitor(ArgsView args)
{
#if LV_USE_SYSMON
    debug_assert_msg(instanceOf(args[0], displayType), "instance is Display");
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        lv_sysmon_show_memory(disp);
    }
#endif
}

void ModuleUI::display_hide_mem_monitor(ArgsView args)
{
#if LV_USE_SYSMON
    debug_assert_msg(instanceOf(args[0], displayType), "instance is Display");
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        lv_sysmon_hide_memory(disp);
    }
#endif
}


void ModuleUI::window_close(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    // Ensure all OpenGL operations are complete before cleanup
    // This helps avoid race conditions in the WSL2 D3D12 driver
    Value lv_window_val_pre = windowInst->getProperty("_lv_window");
    if (!lv_window_val_pre.isNil() && isForeignPtr(lv_window_val_pre)) {
        lv_opengles_window_t* lv_window_pre = (lv_opengles_window_t*)(asForeignPtr(lv_window_val_pre)->ptr);
        if (lv_window_pre) {
            GLFWwindow* glfw_window_pre = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window_pre);
            if (glfw_window_pre) {
                GLFWwindow* prev_context = glfwGetCurrentContext();
                glfwMakeContextCurrent(glfw_window_pre);
                glFinish();
                glfwMakeContextCurrent(prev_context);
            }
        }
    }

    // Safe extraction of foreign pointers (may be nil if already closed)
    Value window_texture_val = windowInst->getProperty("_window_texture");
    auto window_texture = isForeignPtr(window_texture_val)
        ? (lv_opengles_window_texture_t*)(asForeignPtr(window_texture_val)->ptr)
        : nullptr;
    if (window_texture) {
        lv_opengles_window_texture_remove(window_texture);
        windowInst->setProperty("_window_texture",Value::nilVal());
    }

    Value lv_display_val = windowInst->getProperty("_lv_display");
    auto lv_display = isForeignPtr(lv_display_val)
        ? (lv_display_t*)(asForeignPtr(lv_display_val)->ptr)
        : nullptr;
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

    Value lv_window_val = windowInst->getProperty("_lv_window");
    auto lv_window = isForeignPtr(lv_window_val)
        ? (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr)
        : nullptr;
    if (lv_window) {
        // Clean up the user data for GLFW callbacks
        GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
        if (glfw_window) {
            // Remove from map (removing ptr<> releases the memory)
            g_windowUserDataMap.erase(glfw_window);
        }

        if (window_count > 1) {
            // Safe to delete - not the last window
            lv_opengles_window_delete(lv_window);
            windowInst->setProperty("_lv_window",Value::nilVal());
        } else {
            // WORKAROUND: Don't delete the last window to avoid LVGL bug (SEGFAULT)
            // Instead, just hide it
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
// Window Event Simulation (for UI testing)
// ============================================================================

// Helper function to find widget at coordinates by recursively checking children
static lv_obj_t* find_obj_at_point(lv_obj_t* parent, int32_t x, int32_t y)
{
    if (!parent) return nullptr;

    // Check children in reverse order (top-most first)
    uint32_t child_count = lv_obj_get_child_count(parent);
    for (int32_t i = child_count - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child) continue;

        // Skip hidden objects
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;

        // Check if point is within this object's bounds
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            // Recursively check this object's children
            lv_obj_t* found = find_obj_at_point(child, x, y);
            if (found) return found;
            // If no child contains the point, this object is the target
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
                return child;
            }
        }
    }

    // Check if parent itself is clickable at this point
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE)) {
        lv_area_t area;
        lv_obj_get_coords(parent, &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            return parent;
        }
    }

    return nullptr;
}

void ModuleUI::window_simulate_click(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    int x = args[1].asInt();
    int y = args[2].asInt();

    // Get the screen for this window
    Value lv_screen_val = windowInst->getProperty("_lv_screen");
    if (lv_screen_val.isNil() || !isForeignPtr(lv_screen_val))
        return;

    lv_obj_t* screen = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
    if (!screen)
        return;

    // Find the widget at the given coordinates
    lv_obj_t* target = find_obj_at_point(screen, x, y);
    if (!target) {
        target = screen;  // Click on screen if no clickable widget at coordinates
    }

    // Send click events: PRESSED -> CLICKED -> RELEASED
    lv_obj_send_event(target, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(target, LV_EVENT_RELEASED, nullptr);
}

void ModuleUI::window_simulate_key(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };
    Value keyVal { args[1] };

    if (!isString(keyVal))
        return;

    std::string keyStr;
    asUString(keyVal).toUTF8String(keyStr);

    // Map key names to LVGL key codes
    uint32_t key = 0;
    if (keyStr == "Enter" || keyStr == "Return") {
        key = LV_KEY_ENTER;
    } else if (keyStr == "Escape" || keyStr == "Esc") {
        key = LV_KEY_ESC;
    } else if (keyStr == "Tab") {
        key = LV_KEY_NEXT;
    } else if (keyStr == "Backspace") {
        key = LV_KEY_BACKSPACE;
    } else if (keyStr == "Delete") {
        key = LV_KEY_DEL;
    } else if (keyStr == "Left") {
        key = LV_KEY_LEFT;
    } else if (keyStr == "Right") {
        key = LV_KEY_RIGHT;
    } else if (keyStr == "Up") {
        key = LV_KEY_UP;
    } else if (keyStr == "Down") {
        key = LV_KEY_DOWN;
    } else if (keyStr == "Home") {
        key = LV_KEY_HOME;
    } else if (keyStr == "End") {
        key = LV_KEY_END;
    } else if (keyStr.length() == 1) {
        // Single character - treat as character input
        key = (uint32_t)keyStr[0];
    } else {
        return;  // Unknown key
    }

    // Inject the key through the keyboard buffer (same mechanism as real input)
    if (key < 128 && key >= 32) {
        // Printable character - add to keyboard buffer
        size_t len = strlen(g_keyboard.buf);
        if (len < KEYBOARD_BUFFER_SIZE - 2) {
            g_keyboard.buf[len] = (char)key;
            g_keyboard.buf[len + 1] = '\0';
        }
    } else {
        // Special key - send directly to focused widget via indev
        // For now, just add to buffer as a single-byte code
        size_t len = strlen(g_keyboard.buf);
        if (len < KEYBOARD_BUFFER_SIZE - 2) {
            g_keyboard.buf[len] = (char)key;
            g_keyboard.buf[len + 1] = '\0';
        }
    }

    // Trigger keyboard read
    if (g_keyboard.indev) {
        lv_indev_read(g_keyboard.indev);
    }
}

void ModuleUI::window_simulate_text(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    Value textVal { args[1] };

    if (!isString(textVal))
        return;

    std::string text;
    asUString(textVal).toUTF8String(text);

    // Add text to keyboard buffer
    size_t current_len = strlen(g_keyboard.buf);
    size_t text_len = text.length();
    size_t available = KEYBOARD_BUFFER_SIZE - current_len - 1;

    if (text_len > available) {
        text_len = available;
    }

    if (text_len > 0) {
        memcpy(g_keyboard.buf + current_len, text.c_str(), text_len);
        g_keyboard.buf[current_len + text_len] = '\0';
    }

    // Trigger keyboard read to process the text
    if (g_keyboard.indev) {
        lv_indev_read(g_keyboard.indev);
    }
}

void ModuleUI::window_simulate_close(ArgsView args)
{
    debug_assert_msg(instanceOf(args[0], windowType), "instance is Window");
    Value& window { args[0] };
    auto windowInst { asObjectInstance(window) };

    // Get the GLFW window
    Value lv_window_val = windowInst->getProperty("_lv_window");
    if (lv_window_val.isNil() || !isForeignPtr(lv_window_val))
        return;

    lv_opengles_window_t* lv_window = (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr);
    if (!lv_window)
        return;

    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    if (!glfw_window)
        return;

    // Trigger the window close callback (same as clicking the X button)
    // This will emit the WindowClose event which Roxal code can handle
    glfwSetWindowShouldClose(glfw_window, GLFW_TRUE);

    // Also emit the WindowClose event immediately so it can be handled
    emitUIEvent("WindowClose", window);
}


// ============================================================================
// Widget Event Simulation (for UI testing)
// ============================================================================

void ModuleUI::widget_simulate_click(ArgsView args)
{
    Value& widget { args[0] };
    if (!isObjectInstance(widget))
        return;

    auto widgetInst = asObjectInstance(widget);
    Value lv_obj_val = widgetInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_obj = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_obj)
        return;

    // Send click events: PRESSED -> CLICKED -> RELEASED
    lv_obj_send_event(lv_obj, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(lv_obj, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(lv_obj, LV_EVENT_RELEASED, nullptr);
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
            // For TextArea, only emit on actual text changes, not cursor movements
            // LVGL sends VALUE_CHANGED for both, but we only want actual text changes
            bool is_textarea = lv_obj_check_type(target, &lv_textarea_class);
            if (is_textarea) {
                const char* current_text = lv_textarea_get_text(target);
                icu::UnicodeString currentStr = current_text ? toUnicodeString(current_text) : icu::UnicodeString();

                // Get the last text from the widget's hidden property
                ObjectInstance* inst = asObjectInstance(roxalWidget);
                Value lastTextVal = inst->getProperty("_last_emitted_text");
                icu::UnicodeString lastStr = isString(lastTextVal) ? asUString(lastTextVal) : icu::UnicodeString();

                // Skip if text hasn't actually changed
                if (currentStr == lastStr) {
                    break;  // No actual text change, skip event
                }

                // Update the hidden property with current text
                inst->setProperty("_last_emitted_text", Value::stringVal(currentStr));

                module->emitUIEvent("ValueChanged", roxalWidget, {{"value", Value::intVal(0)}});
                break;
            }

            // Get value based on widget type
            int32_t value = 0;
            if (lv_obj_check_type(target, &lv_slider_class)) {
                value = lv_slider_get_value(target);
            } else if (lv_obj_check_type(target, &lv_checkbox_class)) {
                value = lv_obj_has_state(target, LV_STATE_CHECKED) ? 1 : 0;
            } else if (lv_obj_check_type(target, &lv_switch_class)) {
                value = lv_obj_has_state(target, LV_STATE_CHECKED) ? 1 : 0;
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

    // Build the payload map keyed by property name hash
    // Base Event has: target, widget, timestamp
    // Subclasses add their own fields
    std::unordered_map<int32_t, Value> payload;

    // Helper to add a property by name
    auto addProp = [&payload](const char* name, Value val) {
        payload[toUnicodeString(name).hashCode()] = val;
    };

    // Add base Event fields
    addProp("target", widget);  // target (for event filtering via where clause)
    addProp("timestamp", Value::intVal(static_cast<int64_t>(lv_tick_get())));

    // Add event-specific fields based on type name
    if (eventTypeName == "Clicked") {
        // Clicked has: x, y
        auto xIt = extraPayload.find("x");
        auto yIt = extraPayload.find("y");
        addProp("x", xIt != extraPayload.end() ? xIt->second : Value::intVal(0));
        addProp("y", yIt != extraPayload.end() ? yIt->second : Value::intVal(0));
    } else if (eventTypeName == "LongPressed") {
        // LongPressed has: duration
        auto durIt = extraPayload.find("duration");
        addProp("duration", durIt != extraPayload.end() ? durIt->second : Value::intVal(0));
    } else if (eventTypeName == "ValueChanged") {
        // ValueChanged has: value, previous
        auto valIt = extraPayload.find("value");
        auto prevIt = extraPayload.find("previous");
        addProp("value", valIt != extraPayload.end() ? valIt->second : Value::intVal(0));
        addProp("previous", prevIt != extraPayload.end() ? prevIt->second : Value::intVal(0));
    } else if (eventTypeName == "WindowResize") {
        // WindowResize has: newWidth, newHeight
        auto wIt = extraPayload.find("newWidth");
        auto hIt = extraPayload.find("newHeight");
        addProp("newWidth", wIt != extraPayload.end() ? wIt->second : Value::intVal(0));
        addProp("newHeight", hIt != extraPayload.end() ? hIt->second : Value::intVal(0));
    } else if (eventTypeName == "WindowMove") {
        // WindowMove has: newX, newY
        auto xIt = extraPayload.find("newX");
        auto yIt = extraPayload.find("newY");
        addProp("newX", xIt != extraPayload.end() ? xIt->second : Value::intVal(0));
        addProp("newY", yIt != extraPayload.end() ? yIt->second : Value::intVal(0));
    }
    // Pressed, Released, Focused, Defocused, WindowClose, WindowFocus, WindowDefocus,
    // WindowMinimize, WindowRestore have no extra fields

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

void ModuleUI::widget_update_scrollbar(ArgsView args)
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

    Value scrollbar_val = widgetInst->getProperty("scrollbar");
    if (scrollbar_val.isEnum()) {
        // ScrollbarMode enum: Hidden=0, Visible=1, Active=2, Auto=3
        int mode = scrollbar_val.asEnum();
        lv_scrollbar_mode_t lv_mode;
        switch (mode) {
            case 0: lv_mode = LV_SCROLLBAR_MODE_OFF; break;     // Hidden
            case 1: lv_mode = LV_SCROLLBAR_MODE_ON; break;      // Visible
            case 2: lv_mode = LV_SCROLLBAR_MODE_ACTIVE; break;  // Active
            case 3: lv_mode = LV_SCROLLBAR_MODE_AUTO; break;    // Auto
            default: lv_mode = LV_SCROLLBAR_MODE_AUTO; break;
        }
        lv_obj_set_scrollbar_mode(lv_obj, lv_mode);
    }
}

void ModuleUI::widget_update_scroll_dir(ArgsView args)
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

    Value scroll_dir_val = widgetInst->getProperty("scroll_dir");
    if (scroll_dir_val.isEnum()) {
        // ScrollDir enum: None=0, Horizontal=1, Vertical=2, Both=3
        int dir = scroll_dir_val.asEnum();
        lv_dir_t lv_dir;
        switch (dir) {
            case 0: lv_dir = LV_DIR_NONE; break;  // None
            case 1: lv_dir = LV_DIR_HOR; break;   // Horizontal
            case 2: lv_dir = LV_DIR_VER; break;   // Vertical
            case 3: lv_dir = LV_DIR_ALL; break;   // Both
            default: lv_dir = LV_DIR_ALL; break;
        }
        lv_obj_set_scroll_dir(lv_obj, lv_dir);
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
// Image Widget
// ============================================================================

Value ModuleUI::image_create(ArgsView args)
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

    // Create the LVGL image
    lv_obj_t* lv_image = lv_image_create(lv_parent);
    if (!lv_image) {
        raiseException("Failed to create LVGL image");
        return Value::nilVal();
    }

    // Create Roxal Image instance
    Value image = newUIObj(imageType);
    auto imageInst = asObjectInstance(image);

    // Store the LVGL object reference
    imageInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_image));

    // Note: Widget registry registration happens in Roxal init() via _widget_register(this)

    return image;
}

void ModuleUI::image_update_src(ArgsView args)
{
    Value& image = args[0];
    if (!isObjectInstance(image))
        return;

    auto imageInst = asObjectInstance(image);
    Value lv_obj_val = imageInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_image = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_image)
        return;

    Value src_val = imageInst->getProperty("src");
    if (isString(src_val)) {
        std::string srcStr;
        asUString(src_val).toUTF8String(srcStr);
        if (!srcStr.empty()) {
            // LVGL expects file paths with a drive letter prefix for POSIX fs
            // The 'U' prefix is for POSIX filesystem (LV_FS_POSIX_LETTER in lv_conf.h)
            std::string lvPath = "U:" + srcStr;
            lv_image_set_src(lv_image, lvPath.c_str());
        }
    }
}

void ModuleUI::image_update_align(ArgsView args)
{
    Value& image = args[0];
    if (!isObjectInstance(image))
        return;

    auto imageInst = asObjectInstance(image);
    Value lv_obj_val = imageInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_image = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_image)
        return;

    Value align_val = imageInst->getProperty("align");
    if (align_val.isEnum()) {
        // ImageAlign enum: Default=0, Stretch=1, Tile=2, Contain=3, Cover=4
        int align = align_val.asEnum();
        lv_image_align_t lv_align;
        switch (align) {
            case 0: lv_align = LV_IMAGE_ALIGN_DEFAULT; break;   // Default
            case 1: lv_align = LV_IMAGE_ALIGN_STRETCH; break;   // Stretch
            case 2: lv_align = LV_IMAGE_ALIGN_TILE; break;      // Tile
            case 3: lv_align = LV_IMAGE_ALIGN_CONTAIN; break;   // Contain
            case 4: lv_align = LV_IMAGE_ALIGN_COVER; break;     // Cover
            default: lv_align = LV_IMAGE_ALIGN_DEFAULT; break;
        }
        lv_image_set_inner_align(lv_image, lv_align);
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
// Checkbox Widget
// ============================================================================

Value ModuleUI::checkbox_create(ArgsView args)
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

    // Create the LVGL checkbox
    lv_obj_t* lv_checkbox = lv_checkbox_create(lv_parent);
    if (!lv_checkbox) {
        raiseException("Failed to create LVGL checkbox");
        return Value::nilVal();
    }

    // Create Roxal Checkbox instance
    Value checkbox = newUIObj(checkboxType);
    auto checkboxInst = asObjectInstance(checkbox);

    // Store the LVGL object reference
    checkboxInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_checkbox));

    // Add event callback
    lv_obj_add_event_cb(lv_checkbox, (lv_event_cb_t)lvglEventCallback, LV_EVENT_ALL, this);

    return checkbox;
}

void ModuleUI::checkbox_update_text(ArgsView args)
{
    Value& checkbox = args[0];
    if (!isObjectInstance(checkbox))
        return;

    auto checkboxInst = asObjectInstance(checkbox);
    Value lv_obj_val = checkboxInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_checkbox = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_checkbox)
        return;

    Value text_val = checkboxInst->getProperty("text");
    if (isString(text_val)) {
        std::string textStr;
        asUString(text_val).toUTF8String(textStr);
        lv_checkbox_set_text(lv_checkbox, textStr.c_str());
    }
}

void ModuleUI::checkbox_update_checked(ArgsView args)
{
    Value& checkbox = args[0];
    if (!isObjectInstance(checkbox))
        return;

    auto checkboxInst = asObjectInstance(checkbox);
    Value lv_obj_val = checkboxInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_checkbox = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_checkbox)
        return;

    Value checked_val = checkboxInst->getProperty("checked");
    if (checked_val.isBool()) {
        if (checked_val.asBool()) {
            lv_obj_add_state(lv_checkbox, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(lv_checkbox, LV_STATE_CHECKED);
        }
    }
}


// ============================================================================
// Switch Widget
// ============================================================================

Value ModuleUI::switch_create(ArgsView args)
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

    // Create the LVGL switch
    lv_obj_t* lv_switch = lv_switch_create(lv_parent);
    if (!lv_switch) {
        raiseException("Failed to create LVGL switch");
        return Value::nilVal();
    }

    // Create Roxal Switch instance
    Value switchObj = newUIObj(switchType);
    auto switchInst = asObjectInstance(switchObj);

    // Store the LVGL object reference
    switchInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_switch));

    // Add event callback
    lv_obj_add_event_cb(lv_switch, (lv_event_cb_t)lvglEventCallback, LV_EVENT_ALL, this);

    return switchObj;
}

void ModuleUI::switch_update_state(ArgsView args)
{
    Value& switchObj = args[0];
    if (!isObjectInstance(switchObj))
        return;

    auto switchInst = asObjectInstance(switchObj);
    Value lv_obj_val = switchInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_switch = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_switch)
        return;

    Value on_val = switchInst->getProperty("on");
    if (on_val.isBool()) {
        if (on_val.asBool()) {
            lv_obj_add_state(lv_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(lv_switch, LV_STATE_CHECKED);
        }
    }
}


// ============================================================================
// TextArea Widget
// ============================================================================

Value ModuleUI::textarea_create(ArgsView args)
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

    // Create the LVGL textarea
    lv_obj_t* lv_textarea = lv_textarea_create(lv_parent);
    if (!lv_textarea) {
        raiseException("Failed to create LVGL textarea");
        return Value::nilVal();
    }

    // Create Roxal TextArea instance
    Value textarea = newUIObj(textareaType);
    auto textareaInst = asObjectInstance(textarea);

    // Store the LVGL object reference
    textareaInst->setProperty("_lv_obj", Value::foreignPtrVal(lv_textarea));

    // Add event callback
    lv_obj_add_event_cb(lv_textarea, (lv_event_cb_t)lvglEventCallback, LV_EVENT_ALL, this);

    // Add to keyboard input group for text input support
    if (g_keyboard.group) {
        lv_group_add_obj(g_keyboard.group, lv_textarea);
    }

    return textarea;
}

void ModuleUI::textarea_update_text(ArgsView args)
{
    Value& textarea = args[0];
    if (!isObjectInstance(textarea))
        return;

    auto textareaInst = asObjectInstance(textarea);
    Value lv_obj_val = textareaInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_textarea = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_textarea)
        return;

    Value text_val = textareaInst->getProperty("text");
    if (isString(text_val)) {
        std::string textStr;
        asUString(text_val).toUTF8String(textStr);
        lv_textarea_set_text(lv_textarea, textStr.c_str());
    }
}

void ModuleUI::textarea_update_placeholder(ArgsView args)
{
    Value& textarea = args[0];
    if (!isObjectInstance(textarea))
        return;

    auto textareaInst = asObjectInstance(textarea);
    Value lv_obj_val = textareaInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_textarea = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_textarea)
        return;

    Value placeholder_val = textareaInst->getProperty("placeholder");
    if (isString(placeholder_val)) {
        std::string placeholderStr;
        asUString(placeholder_val).toUTF8String(placeholderStr);
        lv_textarea_set_placeholder_text(lv_textarea, placeholderStr.c_str());
    }
}

void ModuleUI::textarea_update_one_line(ArgsView args)
{
    Value& textarea = args[0];
    if (!isObjectInstance(textarea))
        return;

    auto textareaInst = asObjectInstance(textarea);
    Value lv_obj_val = textareaInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_textarea = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_textarea)
        return;

    Value one_line_val = textareaInst->getProperty("one_line");
    if (one_line_val.isBool()) {
        lv_textarea_set_one_line(lv_textarea, one_line_val.asBool());
    }
}

void ModuleUI::textarea_update_password(ArgsView args)
{
    Value& textarea = args[0];
    if (!isObjectInstance(textarea))
        return;

    auto textareaInst = asObjectInstance(textarea);
    Value lv_obj_val = textareaInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_textarea = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_textarea)
        return;

    Value password_val = textareaInst->getProperty("password");
    if (password_val.isBool()) {
        lv_textarea_set_password_mode(lv_textarea, password_val.asBool());
    }
}

void ModuleUI::textarea_update_max_length(ArgsView args)
{
    Value& textarea = args[0];
    if (!isObjectInstance(textarea))
        return;

    auto textareaInst = asObjectInstance(textarea);
    Value lv_obj_val = textareaInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_textarea = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_textarea)
        return;

    Value max_length_val = textareaInst->getProperty("max_length");
    if (max_length_val.isNumber()) {
        lv_textarea_set_max_length(lv_textarea, max_length_val.asInt());
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


void ModuleUI::layout_update_mode(ArgsView args)
{
    Value& layout = args[0];
    if (!isObjectInstance(layout))
        return;

    auto layoutInst = asObjectInstance(layout);
    Value lv_obj_val = layoutInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_layout = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_layout)
        return;

    // Get mode property
    Value mode_val = layoutInst->getProperty("mode");
    if (!isString(mode_val))
        return;

    std::string mode;
    asUString(mode_val).toUTF8String(mode);

    if (mode == "flex") {
        // Set flex layout
        lv_obj_set_layout(lv_layout, LV_LAYOUT_FLEX);

        // Get direction from backing field (set directly in init before mode triggers _update_mode)
        Value dir_val = layoutInst->getProperty("_direction");
        std::string dir;
        if (isString(dir_val)) {
            asUString(dir_val).toUTF8String(dir);
        }
        if (dir.empty()) dir = "row";  // default

        // Get wrap from backing field
        Value wrap_val = layoutInst->getProperty("_wrap");
        bool wrap = wrap_val.isBool() ? wrap_val.asBool() : false;

        // Determine flex flow
        lv_flex_flow_t flow;
        if (dir == "column") {
            flow = wrap ? LV_FLEX_FLOW_COLUMN_WRAP : LV_FLEX_FLOW_COLUMN;
        } else {
            flow = wrap ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW;
        }
        lv_obj_set_flex_flow(lv_layout, flow);

        // Get justify from backing field (main axis alignment)
        Value justify_val = layoutInst->getProperty("_justify");
        std::string justify;
        if (isString(justify_val)) {
            asUString(justify_val).toUTF8String(justify);
        }
        if (justify.empty()) justify = "start";  // default

        lv_flex_align_t main_align = LV_FLEX_ALIGN_START;
        if (justify == "center") main_align = LV_FLEX_ALIGN_CENTER;
        else if (justify == "end") main_align = LV_FLEX_ALIGN_END;
        else if (justify == "space-between") main_align = LV_FLEX_ALIGN_SPACE_BETWEEN;
        else if (justify == "space-around") main_align = LV_FLEX_ALIGN_SPACE_AROUND;
        else if (justify == "space-evenly") main_align = LV_FLEX_ALIGN_SPACE_EVENLY;

        // Get align from backing field (cross axis alignment)
        Value align_val = layoutInst->getProperty("_align");
        std::string align;
        if (isString(align_val)) {
            asUString(align_val).toUTF8String(align);
        }
        if (align.empty()) align = "start";  // default

        lv_flex_align_t cross_align = LV_FLEX_ALIGN_START;
        if (align == "center") cross_align = LV_FLEX_ALIGN_CENTER;
        else if (align == "end") cross_align = LV_FLEX_ALIGN_END;
        else if (align == "stretch") cross_align = LV_FLEX_ALIGN_START;  // stretch handled differently

        // Set flex alignment
        lv_obj_set_flex_align(lv_layout, main_align, cross_align, cross_align);

    } else if (mode == "grid") {
        // Grid layout - LVGL supports this but requires column/row definitions
        // For now, we'll set up a basic grid; more complex grids need additional properties
        lv_obj_set_layout(lv_layout, LV_LAYOUT_GRID);

        // Default to a simple 2-column grid (can be extended with cols/rows properties)
        static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
        static int32_t row_dsc[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                                    LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
        lv_obj_set_grid_dsc_array(lv_layout, col_dsc, row_dsc);

    } else {
        // mode == "none" - disable layout, use manual positioning
        lv_obj_set_layout(lv_layout, LV_LAYOUT_NONE);
    }
}


void ModuleUI::layout_update_padding(ArgsView args)
{
    Value& layout = args[0];
    if (!isObjectInstance(layout))
        return;

    auto layoutInst = asObjectInstance(layout);
    Value lv_obj_val = layoutInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_layout = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_layout)
        return;

    Value padding_val = layoutInst->getProperty("padding");
    if (padding_val.isNumber()) {
        int32_t padding = padding_val.asInt();
        lv_obj_set_style_pad_all(lv_layout, padding, 0);
    }
}


void ModuleUI::layout_update_gap(ArgsView args)
{
    Value& layout = args[0];
    if (!isObjectInstance(layout))
        return;

    auto layoutInst = asObjectInstance(layout);
    Value lv_obj_val = layoutInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_layout = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_layout)
        return;

    Value gap_val = layoutInst->getProperty("gap");
    if (gap_val.isNumber()) {
        int32_t gap = gap_val.asInt();
        // Set both row and column gap
        lv_obj_set_style_pad_row(lv_layout, gap, 0);
        lv_obj_set_style_pad_column(lv_layout, gap, 0);
    }
}

void ModuleUI::layout_update_border_width(ArgsView args)
{
    Value& layout = args[0];
    if (!isObjectInstance(layout))
        return;

    auto layoutInst = asObjectInstance(layout);
    Value lv_obj_val = layoutInst->getProperty("_lv_obj");
    if (lv_obj_val.isNil() || !isForeignPtr(lv_obj_val))
        return;

    lv_obj_t* lv_layout = (lv_obj_t*)(asForeignPtr(lv_obj_val)->ptr);
    if (!lv_layout)
        return;

    Value border_val = layoutInst->getProperty("border_width");
    if (border_val.isNumber()) {
        int32_t border_width = border_val.asInt();
        if (border_width >= 0) {
            // Set border width (0 = no border)
            lv_obj_set_style_border_width(lv_layout, border_width, 0);
        }
        // If -1 (default), don't change the LVGL default
    }
}


// ============================================================================
// Window Snapshot
// ============================================================================

// Internal structure for snapshot pixel data
struct SnapshotPixelData {
    uint8_t* pixels;
    int width;
    int height;
};

Value ModuleUI::window_capture(ArgsView args)
{
    Value& window = args[0];
    if (!isObjectInstance(window))
        return Value::nilVal();

    auto windowInst = asObjectInstance(window);
    Value lv_screen_val = windowInst->getProperty("_lv_screen");
    if (lv_screen_val.isNil() || !isForeignPtr(lv_screen_val))
        return Value::nilVal();

    lv_obj_t* lv_screen = (lv_obj_t*)(asForeignPtr(lv_screen_val)->ptr);
    if (!lv_screen)
        return Value::nilVal();

    // Get the lv_opengles_window_t from the Window object
    Value lv_window_val = windowInst->getProperty("_lv_window");
    if (lv_window_val.isNil() || !isForeignPtr(lv_window_val))
        return Value::nilVal();

    lv_opengles_window_t* lv_window = (lv_opengles_window_t*)(asForeignPtr(lv_window_val)->ptr);
    if (!lv_window)
        return Value::nilVal();

    // Get the GLFW window handle
    GLFWwindow* glfw_window = (GLFWwindow*)lv_opengles_glfw_window_get_glfw_window(lv_window);
    if (!glfw_window)
        return Value::nilVal();

    // Force a complete refresh before taking snapshot
    lv_display_t* disp = lv_obj_get_display(lv_screen);
    if (disp) {
        lv_refr_now(disp);
    }

    // Make the window's context current (save and restore the original context)
    GLFWwindow* prev_context = glfwGetCurrentContext();
    glfwMakeContextCurrent(glfw_window);

    // Get window size
    int width, height;
    glfwGetFramebufferSize(glfw_window, &width, &height);

    if (width <= 0 || height <= 0) {
        glfwMakeContextCurrent(prev_context);
        return Value::nilVal();
    }

    // Allocate buffer for pixel data (RGBA, 4 bytes per pixel)
    size_t buffer_size = width * height * 4;
    uint8_t* pixels = (uint8_t*)malloc(buffer_size);
    if (!pixels) {
        glfwMakeContextCurrent(prev_context);
        return Value::nilVal();
    }

    // Read the framebuffer using OpenGL
    // Note: OpenGL reads from bottom-left, so we'll need to flip vertically later
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Ensure all GL operations complete before we proceed
    glFinish();

    // Restore original context
    glfwMakeContextCurrent(prev_context);

    // Create a Snapshot object
    Value snapshotObj = newUIObj(snapshotType.isWeak() ? snapshotType.strongRef() : snapshotType);
    if (!isObjectInstance(snapshotObj)) {
        free(pixels);
        return Value::nilVal();
    }

    auto snapshotInst = asObjectInstance(snapshotObj);

    // Store the pixel buffer and dimensions
    SnapshotPixelData* snapData = new SnapshotPixelData{pixels, width, height};
    snapshotInst->setProperty("_snapshot_data", Value::foreignPtrVal(snapData));

    // Set width and height
    snapshotInst->setProperty("width", Value::intVal(width));
    snapshotInst->setProperty("height", Value::intVal(height));

    return snapshotObj;
}

void ModuleUI::snapshot_save(ArgsView args)
{
    Value& snapshot = args[0];
    Value& pathVal = args[1];

    if (!isObjectInstance(snapshot))
        return;

    auto snapshotInst = asObjectInstance(snapshot);
    Value snapshot_data_val = snapshotInst->getProperty("_snapshot_data");
    if (snapshot_data_val.isNil() || !isForeignPtr(snapshot_data_val))
        return;

    SnapshotPixelData* snapData = (SnapshotPixelData*)(asForeignPtr(snapshot_data_val)->ptr);
    if (!snapData || !snapData->pixels)
        return;

    if (!isString(pathVal))
        return;

    std::string path;
    asUString(pathVal).toUTF8String(path);

    int width = snapData->width;
    int height = snapData->height;
    uint8_t* pixels = snapData->pixels;

    // Open file for writing
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        return;
    }

    // Initialize PNG structures
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        fclose(fp);
        return;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        fclose(fp);
        return;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return;
    }

    png_init_io(png_ptr, fp);

    // Set image properties (RGBA)
    png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_write_info(png_ptr, info_ptr);

    // OpenGL reads from bottom-left, so we need to flip vertically
    int stride = width * 4;
    for (int y = 0; y < height; y++) {
        uint8_t* src_row = pixels + (height - 1 - y) * stride;
        png_write_row(png_ptr, src_row);
    }

    png_write_end(png_ptr, nullptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
}

void ModuleUI::snapshot_release(ArgsView args)
{
    Value& snapshot = args[0];

    if (!isObjectInstance(snapshot))
        return;

    auto snapshotInst = asObjectInstance(snapshot);
    Value snapshot_data_val = snapshotInst->getProperty("_snapshot_data");
    if (snapshot_data_val.isNil() || !isForeignPtr(snapshot_data_val))
        return;

    SnapshotPixelData* snapData = (SnapshotPixelData*)(asForeignPtr(snapshot_data_val)->ptr);
    if (snapData) {
        if (snapData->pixels) {
            free(snapData->pixels);
            snapData->pixels = nullptr;
        }
        delete snapData;
    }

    // Clear the property so we don't double-free
    snapshotInst->setProperty("_snapshot_data", Value::nilVal());
    snapshotInst->setProperty("width", Value::intVal(0));
    snapshotInst->setProperty("height", Value::intVal(0));
}
