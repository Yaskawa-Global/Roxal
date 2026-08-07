#ifdef __EMSCRIPTEN__

#include "ModuleWeb.h"
#include "JsBridge.h"
#include "RoxalStore.h"
#include "WebHostLoop.h"

#include "ArgsView.h"
#include "VM.h"

#include <stdexcept>
#include <string>

using namespace roxal;
using namespace roxal::web;

ModuleWeb::ModuleWeb()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("web")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleWeb::~ModuleWeb()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleWeb::registerBuiltins(VM& vm)
{
    setVM(vm);

    // No defaults vector needed: the arity comes from modules/web.rox, and
    // marshalArgs() fills an unsupplied parameter with nil. link()'s `defaults`
    // argument is vestigial for that purpose -- what a builtin must do is handle
    // the nil, which is why serve()/notify() check before converting.
    link("expose", [this](VM&, ArgsView a) { return expose_builtin(a); });
    link("notify", [this](VM&, ArgsView a) { return notify_builtin(a); });
    // "_serve", not "serve": web.rox wraps the native park in a Roxal-level
    // serve() that first (re)exposes the Ide language service.
    link("_serve", [this](VM&, ArgsView a) { return serve_builtin(a); });
    link("stop",   [this](VM&, ArgsView a) { return stop_builtin(a); });
}

void ModuleWeb::onModuleLoaded(VM& vm)
{
    // Shared with the dom module -- the VM holds only one host loop.
    installHostLoop(vm);

    // Route JS-originated calls and writes to the right store. Installed here so
    // JsBridge needs no knowledge of RoxalStore.
    setStoreHandlers(
        [](const std::string& store, const std::string& method,
           const std::vector<Value>& args, uint32_t callId) {
            RoxalStore* s = WebStoreHub::instance().lookup(store);
            if (s) s->invoke(method, args, callId);
        },
        [](const std::string& store, const std::string& prop, const Value& value) {
            RoxalStore* s = WebStoreHub::instance().lookup(store);
            if (s) s->applyWrite(prop, value);
        });
}

void ModuleWeb::onScriptComplete(VM& vm)
{
    (void)vm;
    // Whatever the script last changed should still reach the UI, even if it never
    // called serve().
    if (canIssueOps()) {
        WebStoreHub::instance().flushAll();
        web::flush();
    }
}

void ModuleWeb::onModuleUnloading(VM& vm)
{
    WebStoreHub::instance().shutdown();
    uninstallHostLoop(vm);
}

Value ModuleWeb::expose_builtin(ArgsView args)
{
    if (!canIssueOps())
        throw std::runtime_error(
            "web: this build has no browser main thread to publish to "
            "(the web module needs the wasm host, not a native build)");

    const std::string name = args.getString(0);
    if (!args.has(1) || !(isObjectInstance(args[1]) || isActorInstance(args[1])))
        throw std::runtime_error("web.expose: expected an object or actor instance to expose");

    if (!WebStoreHub::instance().expose(name, args[1]))
        throw std::runtime_error("web.expose: '" + name + "' could not be exposed");
    return Value::nilVal();
}

Value ModuleWeb::notify_builtin(ArgsView args)
{
    const std::string name = args.getString(0);
    RoxalStore* store = WebStoreHub::instance().lookup(name);
    if (!store)
        throw std::runtime_error("web.notify: no store named '" + name + "'");

    if (args.has(1) && !args[1].isNil())
        store->markDirty(ustring::fromUTF8(args.getString(1)));
    else
        store->markAllDirty();
    return Value::nilVal();
}

Value ModuleWeb::serve_builtin(ArgsView args)
{
    // Publish the current state before parking, so a UI that mounts immediately
    // sees real values rather than the snapshot taken at expose() time.
    WebStoreHub::instance().flushAll();
    // getReal() must not be evaluated for a nil argument -- C++ evaluates both
    // call arguments regardless of the flag, and converting nil to real raises.
    const bool timed = args.has(0) && !args[0].isNil();
    parkCurrentThread(timed, timed ? args.getReal(0) : 0.0);
    return Value::nilVal();
}

Value ModuleWeb::stop_builtin(ArgsView args)
{
    (void)args;
    requestStop();
    return Value::nilVal();
}

#endif // __EMSCRIPTEN__
