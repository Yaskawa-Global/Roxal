#include "ModuleDebug.h"

#include <algorithm>

#include "ArgsView.h"
#include "Chunk.h"
#include "Object.h"
#include "VM.h"
#include "debug/BreakpointManager.h"
#include "debug/DebugInfo.h"
#include "debug/DebugInspector.h"
#include "debug/ModuleDebugIndex.h"
#include "debug/StopCoordinator.h"

namespace roxal {

static std::atomic<bool> s_debugSessionPending{false};

// The debugger CONTROL CAPABILITY: the first actor exposed as the "debug"
// store under an armed session becomes THE control actor, and every control
// native validates that it is running on that actor's thread.  Application
// code that imports `debug` cannot self-exclude or drive stops: it is not
// on the control actor's thread, and it cannot displace the capability
// (first-wins; reset per script).  Compare-only raw pointer -- never
// dereferenced, so no rooting needed.
static std::atomic<const void*> s_controlActor{nullptr};

static bool callerIsControlActor()
{
    const void* ctl = s_controlActor.load(std::memory_order_acquire);
    return ctl != nullptr && VM::thread
        && VM::thread->actorIdentity() == ctl;
}

ModuleDebug::ModuleDebug()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("debug")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleDebug::~ModuleDebug()
{
    destroyModuleType(moduleTypeValue);
}

bool ModuleDebug::controlActorPresent()
{
    return s_controlActor.load(std::memory_order_acquire) != nullptr;
}

void ModuleDebug::registerControlActor(const Value& actorInstance)
{
    s_controlActor.store(static_cast<const void*>(actorInstance.asObj()),
                         std::memory_order_release);
}

void ModuleDebug::onScriptComplete(VM& vm)
{
    // Each run constructs a fresh control actor; the old capability dies
    // with its script.  With no session armed, debugger policy dies too
    // (an armed session re-establishes it before the next run).
    s_controlActor.store(nullptr, std::memory_order_release);
    if (!sessionPending())
        vm.stopCoordinator().setStopOnFatal(false);
}

void ModuleDebug::registerBuiltins(VM& vm)
{
    setVM(vm);
    for (const char* op : { "session_pending", "exclude", "arm",
                            "set_breakpoints", "locations",
                            "pause", "resume", "step",
                            "state", "threads", "stack",
                            "scopes", "variables", "evaluate" }) {
        std::string name(op);
        link(name.c_str(), [this, name](VM&, ArgsView a) {
            return debug_builtin(name, a);
        });
    }
}

// ---- Debugger natives ------------------------------------------------------
// These run on the web Debug actor's thread (an excluded service thread --
// the in-VM controller precedent from the native test suite): they drive the
// stop coordinator directly and read through DebugInspector's GC-covered
// entry points.  The web transport has no other controller.



void ModuleDebug::setSessionPending(bool on)
{
    s_debugSessionPending.store(on, std::memory_order_release);
    if (!on) {
        // Session teardown must not leave debugger policy or a frozen world
        // behind: disarm stop-on-fatal, and resolve any active stop on the
        // worker (the sole controller once the web control actor is gone).
        auto& coord = VM::instance().stopCoordinator();
        coord.setStopOnFatal(false);
        coord.postToWorker([] {
            auto& c = VM::instance().stopCoordinator();
            if (c.isStopped())
                c.resume(/*releaseHold=*/false);
        });
    }
}

bool ModuleDebug::sessionPending()
{
    return s_debugSessionPending.load(std::memory_order_acquire);
}

static DebugInspector& webDebugInspector()
{
    static DebugInspector inspector(VM::instance().stopCoordinator());
    return inspector;
}

static Value mkStr(const std::string& s) { return Value::stringVal(toUnicodeString(s)); }

static const char* stopReasonName(DebugStopReason r)
{
    switch (r) {
        case DebugStopReason::Breakpoint: return "breakpoint";
        case DebugStopReason::Step:       return "step";
        case DebugStopReason::Exception:  return "exception";
        case DebugStopReason::Pause:
        case DebugStopReason::Host:       return "pause";
    }
    return "pause";
}

Value ModuleDebug::debug_builtin(const std::string& op, ArgsView args)
{
    VM& vm = VM::instance();
    auto& coord = vm.stopCoordinator();
    auto& insp = webDebugInspector();

    if (op == "session_pending")
        return Value::boolVal(sessionPending());
    if (!sessionPending())
        throw std::runtime_error(
            "debug." + op + ": no debugger session is armed (host-controlled)");
    // Everything below is a debugger CONTROL operation: only the registered
    // control actor's thread may call it.  (The native C++/DAP controllers
    // never come through these script natives.)
    if (!callerIsControlActor())
        throw std::runtime_error(
            "debug." + op + ": not the debugger control context");
    if (op == "exclude") {
        if (VM::thread)
            VM::thread->debugExcluded.store(true, std::memory_order_release);
        return Value::nilVal();
    }
    if (op == "arm") {
        coord.ensureWorker();
        coord.setStopOnFatal(args.getBool(0, true));
        return Value::nilVal();
    }
    if (op == "set_breakpoints") {
        std::vector<int> lines;
        if (args.has(1) && isList(args[1])) {
            ObjList* l = asList(args[1]);
            for (int32_t i = 0; i < l->length(); ++i)
                lines.push_back(int(l->getElement(size_t(i)).asInt()));
        }
        auto bound = BreakpointManager::instance().setBreakpoints(
            args.getString(0), lines);
        Value outV { Value::objVal(newListObj()) };
        ObjList* out = asList(outV);
        for (const auto& b : bound) {
            Value eV { Value::objVal(newDictObj()) };
            ObjDict* d = asDict(eV);
            d->store(mkStr("line"), Value::intVal(b.verified ? b.boundLine
                                                             : b.requestedLine));
            d->store(mkStr("verified"), Value::boolVal(b.verified));
            out->append(eV);
        }
        return outV;
    }
    if (op == "locations") {
        const int l0 = args.getInt(1, 1);
        const int l1 = args.getInt(2, l0);
        Value outV { Value::objVal(newListObj()) };
        ObjList* out = asList(outV);
        auto entry = ModuleDebugIndex::instance().lookupBySource(args.getString(0));
        if (entry.has_value() && isList(entry->functions)) {
            std::vector<int> lines;
            ObjList* fns = asList(entry->functions);
            for (int32_t fi = 0; fi < fns->length(); ++fi) {
                Value fv = fns->getElement(size_t(fi));
                if (!isFunction(fv)) continue;
                const Chunk* ch = asFunction(fv)->chunk.get();
                if (!ch || !ch->debugInfo) continue;
                for (const auto& st : ch->debugInfo->stmts)
                    if (st.kind == uint8_t(DebugStmtKind::StatementStart)
                        && st.line >= l0 && st.line <= l1)
                        lines.push_back(st.line);
            }
            std::sort(lines.begin(), lines.end());
            lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
            for (int l : lines)
                out->append(Value::intVal(l));
        }
        return outV;
    }
    if (op == "pause") {
        if (coord.isStopped())
            return Value::boolVal(true);
        auto out = coord.requestStop(DebugStopReason::Pause,
                                     TimeDuration::milliSecs(3000));
        if (!out.stopped && !coord.isStopped())
            VM::emitDiagnostic("debugger: pause failed: " + out.failure,
                               OutputSeverity::Warning, "debug");
        return Value::boolVal(out.stopped || coord.isStopped());
    }
    if (op == "resume") {
        if (coord.isStopped())
            coord.resume(/*releaseHold=*/true);
        return Value::boolVal(true);
    }
    if (op == "step") {
        if (!coord.isStopped())
            return Value::boolVal(false);
        const uint64_t tid = uint64_t(args.getInt(0, 0));
        const std::string mode = args.getString(1, "next");
        ptr<Thread> target;
        coord.forEachEpochThread([&](const ptr<Thread>& t) {
            if (t->id() == tid) target = t;
        });
        if (!target)
            return Value::boolVal(false);
        coord.stepAndResume(*target,
            mode == "in" ? Thread::DebugStepMode::In
          : mode == "out" ? Thread::DebugStepMode::Out
                          : Thread::DebugStepMode::Next);
        return Value::boolVal(true);
    }
    if (op == "state") {
        Value dV { Value::objVal(newDictObj()) };
        ObjDict* d = asDict(dV);
        auto info = coord.stoppedInfo();
        d->store(mkStr("stopped"), Value::boolVal(info.has_value()));
        if (info) {
            // Distinguishes one stop from the next even at the same line
            // (a re-fired breakpoint): UIs key their refetch on it.
            d->store(mkStr("epoch"), Value::intVal(int64_t(coord.currentEpoch())));
            d->store(mkStr("reason"), mkStr(stopReasonName(info->reason)));
            d->store(mkStr("thread_id"), Value::intVal(int64_t(info->threadId)));
            d->store(mkStr("source"), mkStr(info->sourceName));
            d->store(mkStr("line"), Value::intVal(info->line));
            d->store(mkStr("text"), mkStr(info->description));
        }
        return dV;
    }
    if (op == "threads") {
        Value outV { Value::objVal(newListObj()) };
        ObjList* out = asList(outV);
        for (const auto& td : insp.threads()) {
            Value eV { Value::objVal(newDictObj()) };
            ObjDict* d = asDict(eV);
            d->store(mkStr("id"), Value::intVal(int64_t(td.threadId)));
            d->store(mkStr("name"), mkStr(td.name));
            out->append(eV);
        }
        return outV;
    }
    if (op == "stack") {
        Value outV { Value::objVal(newListObj()) };
        ObjList* out = asList(outV);
        for (const auto& f : insp.stackTrace(uint64_t(args.getInt(0, 0)),
                                             size_t(args.getInt(1, 0)),
                                             size_t(args.getInt(2, 0)))) {
            Value eV { Value::objVal(newDictObj()) };
            ObjDict* d = asDict(eV);
            d->store(mkStr("id"), Value::intVal(f.handle));
            d->store(mkStr("name"), mkStr(f.name));
            d->store(mkStr("source"), mkStr(f.source));
            d->store(mkStr("line"), Value::intVal(f.line));
            d->store(mkStr("column"), Value::intVal(f.column));
            out->append(eV);
        }
        return outV;
    }
    if (op == "scopes") {
        Value outV { Value::objVal(newListObj()) };
        ObjList* out = asList(outV);
        for (const auto& sc : insp.scopes(args.getInt(0, 0))) {
            Value eV { Value::objVal(newDictObj()) };
            ObjDict* d = asDict(eV);
            d->store(mkStr("name"), mkStr(sc.name));
            d->store(mkStr("ref"), Value::intVal(sc.variablesHandle));
            out->append(eV);
        }
        return outV;
    }
    if (op == "variables") {
        Value outV { Value::objVal(newListObj()) };
        ObjList* out = asList(outV);
        for (const auto& v : insp.variables(args.getInt(0, 0),
                                            size_t(args.getInt(1, 0)),
                                            size_t(args.getInt(2, 0)))) {
            Value eV { Value::objVal(newDictObj()) };
            ObjDict* d = asDict(eV);
            d->store(mkStr("name"), mkStr(v.name));
            d->store(mkStr("value"), mkStr(v.value));
            d->store(mkStr("type"), mkStr(v.type));
            d->store(mkStr("ref"), Value::intVal(v.childHandle));
            out->append(eV);
        }
        return outV;
    }
    if (op == "evaluate") {
        auto r = insp.evaluate(args.getInt(0, 0), args.getString(1));
        Value dV { Value::objVal(newDictObj()) };
        ObjDict* d = asDict(dV);
        d->store(mkStr("ok"), Value::boolVal(r.ok));
        if (r.ok) {
            d->store(mkStr("value"), mkStr(r.value));
            d->store(mkStr("type"), mkStr(r.type));
            d->store(mkStr("ref"), Value::intVal(r.childHandle));
        } else {
            d->store(mkStr("error"), mkStr(r.error));
        }
        return dV;
    }
    return Value::nilVal();
}


} // namespace roxal
