#include "ModuleUI.h"
#include "../compiler/VM.h"
#include "../compiler/Object.h"

#include <GLFW/glfw3.h>
#include <lvgl.h>

using namespace roxal;

ModuleUI::ModuleUI()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("ui")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleUI::~ModuleUI()
{
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

    linkMethod("Display", "create_window", [this](VM& vm, ArgsView a){ return display_create_window(a); });

    linkMethod("Window", "close", [this](VM& vm, ArgsView a){ window_close(a); return Value::nilVal(); });
    linkMethod("Window", "open", [this](VM& vm, ArgsView a){ window_open(a); return Value::nilVal(); });
    linkMethod("Window", "when_title_changes", [this](VM& vm, ArgsView a){ window_when_title_changes(a); return Value::nilVal(); });
    linkMethod("Window", "when_position_changes", [this](VM& vm, ArgsView a){ window_when_position_changes(a); return Value::nilVal(); });
    linkMethod("Window", "when_size_changes", [this](VM& vm, ArgsView a){ window_when_size_changes(a); return Value::nilVal(); });

    displayType = uiType("Display").weakRef();
    windowType = uiType("Window").weakRef();
}


void ModuleUI::initialize()
{
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
