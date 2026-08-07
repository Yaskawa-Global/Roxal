#ifdef __EMSCRIPTEN__

#include "ModuleDom.h"
#include "JsBridge.h"
#include "ObjJsValue.h"
#include "WebHostLoop.h"

#include "ArgsView.h"
#include "VM.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace roxal;
using namespace roxal::web;

namespace {

// Every entry point takes a JS value as its receiver; reject anything else with a
// message that says what was expected rather than failing inside the encoder.
uint32_t handleArg(ArgsView args, size_t i, const char* fn)
{
    if (!args.has(i) || args[i].isNil())
        throw std::runtime_error(std::string("dom.") + fn + ": expected a JS value, got nil");
    if (!isJsValue(args[i]))
        throw std::runtime_error(std::string("dom.") + fn + ": expected a JS value");
    return asJsValue(args[i])->handle;
}

Value globalByName(const std::string& name)
{
    Encoder e;
    e.op(Op::Global);
    e.str(name);
    return exec(e);
}

} // namespace

ModuleDom::ModuleDom()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("dom")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleDom::~ModuleDom()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleDom::registerBuiltins(VM& vm)
{
    setVM(vm);

    // No defaults vector: arity comes from modules/dom.rox and an unsupplied
    // parameter arrives as nil -- builtins must handle that nil themselves.
    link("_init",  [this](VM&, ArgsView a) { return init_builtin(a); });
    link("global", [this](VM&, ArgsView a) { return global_builtin(a); });
    link("get",    [this](VM&, ArgsView a) { return get_builtin(a); });
    link("set",    [this](VM&, ArgsView a) { return set_builtin(a); });
    link("call",   [this](VM&, ArgsView a) { return call_builtin(a); });
    link("on",     [this](VM&, ArgsView a) { return on_builtin(a); });
    link("run",    [this](VM&, ArgsView a) { return run_builtin(a); });
    link("stop",   [this](VM&, ArgsView a) { return stop_builtin(a); });
    link("off",    [this](VM&, ArgsView a) { return off_builtin(a); });
    link("flush",  [this](VM&, ArgsView a) { return flush_builtin(a); });
}

void ModuleDom::onModuleLoaded(VM& vm)
{
    // Shared with the web module -- the VM holds only one host loop, so each
    // module installing its own would disable the other.
    installHostLoop(vm);
}

void ModuleDom::onScriptComplete(VM& vm)
{
    (void)vm;
    // A script that ends with unflushed writes should still leave the page
    // correct -- otherwise the last thing it did silently never happened.
    if (canIssueOps()) web::flush();
}

void ModuleDom::onModuleUnloading(VM& vm)
{
    uninstallHostLoop(vm);
    web::shutdown();
}

// dom._init() -- populate the `document` and `window` module vars.
Value ModuleDom::init_builtin(ArgsView args)
{
    (void)args;
    if (!canIssueOps())
        throw std::runtime_error(
            "dom: this build has no browser main thread to talk to "
            "(the dom module needs the wasm host, not a native build)");

    ObjModuleType* mod = asModuleType(moduleTypeValue);
    mod->vars.store(toUnicodeString("document"), globalByName("document"), /*overwrite=*/true);
    mod->vars.store(toUnicodeString("window"),   globalByName("window"),   /*overwrite=*/true);
    return Value::nilVal();
}

Value ModuleDom::global_builtin(ArgsView args)
{
    return globalByName(args.getString(0));
}

Value ModuleDom::get_builtin(ArgsView args)
{
    Encoder e;
    e.op(Op::Get);
    e.u32(handleArg(args, 0, "get"));
    e.str(args.getString(1));
    return exec(e);
}

Value ModuleDom::set_builtin(ArgsView args)
{
    Encoder e;
    e.op(Op::Set);
    e.u32(handleArg(args, 0, "set"));
    e.str(args.getString(1));
    e.value(args.has(2) ? args[2] : Value::nilVal());
    defer(e);
    return Value::nilVal();
}

Value ModuleDom::call_builtin(ArgsView args)
{
    std::vector<Value> callArgs;
    if (args.has(2) && !args[2].isNil()) {
        if (!isList(args[2]))
            throw std::runtime_error("dom.call: args must be a list or nil");
        callArgs = asList(args[2])->getElements();
    }

    Encoder e;
    e.op(Op::Call);
    e.u32(handleArg(args, 0, "call"));
    e.str(args.getString(1));
    e.u32(static_cast<uint32_t>(callArgs.size()));
    for (const Value& a : callArgs) e.value(a);
    return exec(e);
}

Value ModuleDom::on_builtin(ArgsView args)
{
    if (!args.has(2) || !isClosure(args[2]))
        throw std::runtime_error("dom.on: expected a handler function");

    Encoder e;
    e.op(Op::Listen);
    e.u32(handleArg(args, 0, "on"));
    e.str(args.getString(1));
    e.u32(registerCallback(args[2]));
    return exec(e);
}

Value ModuleDom::off_builtin(ArgsView args)
{
    Encoder e;
    e.op(Op::Unlisten);
    e.u32(handleArg(args, 0, "off"));
    defer(e);
    return Value::nilVal();
}

// dom.run(seconds=nil) -- hand control to the page.
//
// Parks the Roxal thread and returns; the dispatch loop then services events and
// invokes handlers with a live thread context. See WebHostLoop.h for why this
// must not block in C++.
Value ModuleDom::run_builtin(ArgsView args)
{
    // getReal() must not be evaluated for a nil argument -- C++ evaluates both
    // call arguments regardless of the flag, and converting nil to real raises.
    const bool timed = args.has(0) && !args[0].isNil();
    parkCurrentThread(timed, timed ? args.getReal(0) : 0.0);
    return Value::nilVal();
}

Value ModuleDom::stop_builtin(ArgsView args)
{
    (void)args;
    requestStop();
    return Value::nilVal();
}

Value ModuleDom::flush_builtin(ArgsView args)
{
    (void)args;
    web::flush();
    return Value::nilVal();
}

#endif // __EMSCRIPTEN__
