#ifdef ROXAL_ENABLE_WEB

#include <algorithm>

#include "ModuleWeb.h"
#include "JsBridge.h"
#include "RoxalStore.h"
#include "WebHostLoop.h"

#include "ArgsView.h"
#include "ModuleDebug.h"
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
            if (s) {
                s->invoke(method, args, callId);
            } else {
                // A call posted before the script exposes the store (or after
                // it is gone) must settle: dropping it left the JS promise to
                // the bridge's 20s timeout with no way to tell "not yet" from
                // "dead".
                rejectStoreCall(callId, store, method,
                                "is not exposed (has the script reached web.expose()?)");
            }
        },
        [](const std::string& store, const std::string& prop, const Value& value) {
            RoxalStore* s = WebStoreHub::instance().lookup(store);
            if (s) s->applyWrite(prop, value);
        });
    // While a debug stop is active, the inbound drain runs only ACTOR-backed
    // store calls (queue-only on the pumping thread; the excluded actor
    // executes them) -- this is its predicate.
    exposeDebugControl(vm);
    setStoreActorPredicate([](const std::string& store) {
        // ONLY the debugger control store runs while stopped: admitting
        // every actor store reorders deferred work against actor calls and
        // lets ordinary calls pile up.  The name is reserved -- web.expose
        // refuses it -- so actor-backed + named "debug" identifies the
        // host-exposed control actor.
        if (store != "debug")
            return false;
        RoxalStore* s = WebStoreHub::instance().lookup(store);
        return s != nullptr && isActorInstance(s->objValue());
    });
}

// Under a host-armed session, construct the debugger control actor FROM THE
// HOST (never script code -- the capability is unforgeable), register it
// with the debug module, and expose it as the "debug" store.  Runs at module
// load (covers the script that first imports web) and at every later script
// start (each run gets a fresh actor; the previous one died with its
// script).
void ModuleWeb::exposeDebugControl(VM& vm)
{
    if (!ModuleDebug::sessionPending())
        return;
    auto typeVal = asModuleType(moduleTypeValue)->vars.load(toUnicodeString("Debug"));
    if (!typeVal.has_value() || !isObjectType(typeVal.value()))
        return;
    Value inst = Value::actorInstanceVal(typeVal.value());
    vm.spawnActorThread(inst, /*debugExcluded=*/true);
    ModuleDebug::registerControlActor(inst);
    WebStoreHub::instance().expose("debug", inst);
}

void ModuleWeb::onScriptStart(VM& vm)
{
    exposeDebugControl(vm);
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
            "web: no web host is attached to publish to "
            "(run a native VM with `roxal --web-host`, or use the wasm host)");

    const std::string name = args.getString(0);
    if (!args.has(1) || !(isObjectInstance(args[1]) || isActorInstance(args[1])))
        throw std::runtime_error("web.expose: expected an object or actor instance to expose");

    // Reserved: the host builds and exposes the debugger control store
    // itself, and the stopped-world inbound drain identifies it by name.
    if (name == "debug")
        throw std::runtime_error(
            "web.expose: 'debug' is reserved for the debugger control store");
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

#endif // ROXAL_ENABLE_WEB
