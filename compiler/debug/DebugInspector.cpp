#include "DebugInspector.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <limits>

#include "DebugInfo.h"
#include "StopCoordinator.h"
#include "../Chunk.h"
#include "../Introspection.h"
#include "../Object.h"
#include "../Thread.h"
#include "../VM.h"
#include "dataflow/Signal.h"

namespace roxal {

DebugInspector::DebugInspector(StopCoordinator& coord, DebugLimits limits)
    : coord_(coord), limits_(limits)
{
}

// ---- helpers ---------------------------------------------------------------

static const Chunk* frameChunk(const CallFrame& f)
{
    if (!isClosure(f.closure))
        return nullptr;
    const Value fn = asClosure(f.closure)->function;
    if (!isFunction(fn))
        return nullptr;
    return asFunction(fn)->chunk.get();
}

// Code offset a frame should be reported at.  The top frame's ip is the next
// instruction (== the statement start at a boundary stop); a caller frame's
// ip is the RETURN address, so step back one byte to land inside the call
// statement rather than the one after it.
static uint32_t frameCodeOffset(const Thread& t, size_t frameIndex)
{
    const CallFrame& f = t.frames[frameIndex];
    const Chunk* ch = frameChunk(f);
    if (!ch)
        return 0;
    uint32_t off = uint32_t(f.ip - ch->code.begin());
    const bool top = (frameIndex + 1 == t.frames.size());
    if (!top && off > 0)
        --off;
    return off;
}

// Values a frame's slot indices may legally address: everything from the
// frame's base to the thread's stack top (caller frames sit below their
// callees, so one upper bound covers every frame).
static size_t frameSlotLimit(const Thread& t, const CallFrame& f)
{
    if (!f.slots)
        return 0;
    const Value* base = t.stack.data();
    const Value* top = base + (t.stackTop - t.stack.begin());
    if (f.slots < base || f.slots > top)
        return 0;
    return size_t(top - f.slots);
}

static bool valueHasChildren(const Value& v)
{
    if (isList(v))
        return asList(v)->length() > 0;
    if (isDict(v))
        return asDict(v)->length() > 0;
    if (isObjectInstance(v))
        return !asObjectInstance(v)->instanceType.isNil();
    if (isFuture(v) || isSignal(v))
        return true;
    return false;   // tensors/actors/natives: opaque (summary rendered)
}

DebugVarDesc DebugInspector::makeVar(std::string name, const Value& v,
                                     uint64_t epoch, bool synthetic)
{
    DebugVarDesc d;
    d.name = std::move(name);
    d.value = renderValue(v, limits_.render);
    d.type = describeValueType(v);
    d.synthetic = synthetic;
    if (valueHasChildren(v)) {
        DebugHandle h;
        h.kind = DebugHandleKind::Variable;
        h.epoch = epoch;
        h.value = v;
        d.childHandle = coord_.handleTable().create(h);
    }
    return d;
}

ptr<Thread> DebugInspector::findEpochThread(uint64_t threadId)
{
    ptr<Thread> found;
    coord_.forEachEpochThread([&](const ptr<Thread>& t) {
        if (t->id() == threadId)
            found = t;
    });
    return found;
}

bool DebugInspector::resolveFrame(const DebugHandle& h, ptr<Thread>& tOut,
                                  size_t& frameIndexOut)
{
    tOut = findEpochThread(h.threadId);
    if (!tOut)
        return false;
    if (h.frameIndex >= tOut->frames.size())
        return false;   // stale: the frame stack shrank since creation
    const CallFrame& f = tOut->frames[h.frameIndex];
    if (f.activationId == 0 || f.activationId != h.activationId)
        return false;   // stale: a different activation reuses the depth
    frameIndexOut = h.frameIndex;
    return true;
}

// ---- threads / stackTrace / scopes ----------------------------------------

std::vector<DebugThreadDesc> DebugInspector::threads()
{
    // Every entry point runs under a GC mutator cover: the caller may be
    // any host thread (test controller today, the DAP servicer later), and
    // creating handles / touching Values must be visible to the collection
    // barrier.  No-op when the thread is already covered.
    ScopedGCMutatorCover gcCover;
    std::vector<DebugThreadDesc> out;
    if (!coord_.isStopped())
        return out;
    const uint64_t epoch = coord_.currentEpoch();
    coord_.forEachEpochThread([&](const ptr<Thread>& t) {
        DebugThreadDesc d;
        d.threadId = t->id();
        d.name = "thread " + std::to_string(t->id());
        DebugHandle h;
        h.kind = DebugHandleKind::Thread;
        h.epoch = epoch;
        h.threadId = t->id();
        d.handle = coord_.handleTable().create(h);
        out.push_back(std::move(d));
    });
    return out;
}

std::vector<DebugFrameDesc> DebugInspector::stackTrace(uint64_t threadId,
                                                       size_t startFrame,
                                                       size_t levels)
{
    ScopedGCMutatorCover gcCover;
    std::vector<DebugFrameDesc> out;
    if (!coord_.isStopped())
        return out;
    ptr<Thread> t = findEpochThread(threadId);
    if (!t)
        return out;
    const uint64_t epoch = coord_.currentEpoch();
    const size_t nFrames = t->frames.size();
    if (startFrame >= nFrames)
        return out;
    if (levels == 0 || levels > limits_.maxFramesPerRequest)
        levels = limits_.maxFramesPerRequest;

    // DAP order: index 0 = innermost.  Frame i (top-down) is frames vector
    // index nFrames-1-i.
    for (size_t i = startFrame; i < nFrames && out.size() < levels; ++i) {
        const size_t fi = nFrames - 1 - i;
        CallFrame& f = t->frames[fi];
        if (f.activationId == 0)
            f.activationId = t->nextActivationId++;   // lazy; thread is parked

        DebugFrameDesc d;
        const Chunk* ch = frameChunk(f);
        if (ch) {
            const std::string fname =
                toUTF8StdString(asFunction(asClosure(f.closure)->function)->name);
            d.name = fname.empty() ? "<module>" : fname;
            d.source = toUTF8StdString(ch->sourceName);
            if (ch->debugInfo && !ch->debugInfo->stmts.empty()) {
                const uint32_t off = frameCodeOffset(*t, fi);
                const int32_t idx = debugLocateStatement(*ch->debugInfo, off);
                if (idx >= 0) {
                    d.line = ch->debugInfo->stmts[idx].line;
                    d.column = ch->debugInfo->stmts[idx].column;
                }
            }
        } else {
            d.name = "<native>";
        }

        DebugHandle h;
        h.kind = DebugHandleKind::Frame;
        h.epoch = epoch;
        h.threadId = threadId;
        h.activationId = f.activationId;
        h.frameIndex = uint32_t(fi);
        d.handle = coord_.handleTable().create(h);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<DebugScopeDesc> DebugInspector::scopes(int32_t frameHandle)
{
    ScopedGCMutatorCover gcCover;
    std::vector<DebugScopeDesc> out;
    if (!coord_.isStopped())
        return out;
    const uint64_t epoch = coord_.currentEpoch();
    auto h = coord_.handleTable().lookup(frameHandle, epoch);
    if (!h || h->kind != DebugHandleKind::Frame)
        return out;
    ptr<Thread> t;
    size_t fi = 0;
    if (!resolveFrame(*h, t, fi))
        return out;
    const CallFrame& f = t->frames[fi];

    auto addScope = [&](DebugScopeKind kind, const char* name) {
        DebugHandle sh = *h;
        sh.kind = DebugHandleKind::Scope;
        sh.scope = kind;
        DebugScopeDesc d;
        d.name = name;
        d.variablesHandle = coord_.handleTable().create(sh);
        out.push_back(std::move(d));
    };

    addScope(DebugScopeKind::Locals, "Locals");
    if (isClosure(f.closure) && !asClosure(f.closure)->upvalues.empty())
        addScope(DebugScopeKind::Upvalues, "Upvalues");
    if (isClosure(f.closure) && !asFunction(asClosure(f.closure)->function)->moduleType.isNil())
        addScope(DebugScopeKind::ModuleVars, "Module");
    return out;
}

// ---- variables -------------------------------------------------------------

std::vector<DebugVarDesc> DebugInspector::variables(int32_t variablesHandle,
                                                    size_t start, size_t count)
{
    ScopedGCMutatorCover gcCover;
    std::vector<DebugVarDesc> out;
    if (!coord_.isStopped())
        return out;
    if (count == 0 || count > limits_.maxVariablesPerPage)
        count = limits_.maxVariablesPerPage;
    auto h = coord_.handleTable().lookup(variablesHandle, coord_.currentEpoch());
    if (!h)
        return out;
    if (h->kind == DebugHandleKind::Scope)
        return scopeChildren(*h, start, count);
    if (h->kind == DebugHandleKind::Variable)
        return valueChildren(*h, start, count);
    return out;
}

std::vector<DebugVarDesc> DebugInspector::scopeChildren(const DebugHandle& h,
                                                        size_t start, size_t count)
{
    std::vector<DebugVarDesc> out;
    ptr<Thread> t;
    size_t fi = 0;
    if (!resolveFrame(h, t, fi))
        return out;
    const CallFrame& f = t->frames[fi];
    const Chunk* ch = frameChunk(f);
    const uint64_t epoch = h.epoch;

    switch (h.scope) {
    case DebugScopeKind::Locals: {
        if (!ch || !ch->debugInfo)
            break;
        const uint32_t off = frameCodeOffset(*t, fi);
        const size_t slotLimit = frameSlotLimit(*t, f);
        size_t seen = 0;
        for (const auto& lv : ch->debugInfo->locals) {
            if (off < lv.startOffset || off >= lv.endOffset)
                continue;   // not live at this ip
            if (lv.slot >= slotLimit)
                continue;   // defensive: never read past the stack top
            if (!limits_.includeSynthetic
                && (lv.flags & DebugLocalSynthetic) != 0)
                continue;   // hidden (and never consumes a page slot)
            if (seen++ < start)
                continue;
            if (out.size() >= count)
                break;
            out.push_back(makeVar(toUTF8StdString(lv.name), f.slots[lv.slot],
                                  epoch,
                                  (lv.flags & DebugLocalSynthetic) != 0));
        }
        break;
    }
    case DebugScopeKind::Upvalues: {
        if (!ch || !ch->debugInfo || !isClosure(f.closure))
            break;
        const auto& ups = asClosure(f.closure)->upvalues;
        const auto& names = ch->debugInfo->upvalues;
        for (size_t i = start; i < ups.size() && out.size() < count; ++i) {
            if (!isUpvalue(ups[i]))
                continue;
            const ObjUpvalue* uv = asUpvalue(ups[i]);
            if (!uv->location)
                continue;
            // *location is authoritative whether the upvalue is still open
            // (points into a stopped thread's frozen stack) or closed
            // (points at its own `closed` box).
            const std::string name = (i < names.size())
                ? toUTF8StdString(names[i].name)
                : "upvalue " + std::to_string(i);
            out.push_back(makeVar(name, *uv->location, epoch));
        }
        break;
    }
    case DebugScopeKind::ModuleVars: {
        if (!isClosure(f.closure))
            break;
        const Value mt = asFunction(asClosure(f.closure)->function)->moduleType;
        if (mt.isNil())
            break;
        // Already-synchronized PAGED snapshot: serving one page must not
        // copy and refcount the whole module.
        auto vars = asModuleType(mt)->vars.snapshotRange(start, count);
        for (const auto& nv : vars)
            out.push_back(makeVar(toUTF8StdString(nv.first), nv.second, epoch));
        break;
    }
    }
    return out;
}

std::vector<DebugVarDesc> DebugInspector::valueChildren(const DebugHandle& h,
                                                        size_t start, size_t count)
{
    std::vector<DebugVarDesc> out;
    const Value& v = h.value;
    const uint64_t epoch = h.epoch;

    if (isList(v)) {
        const ObjList* l = asList(v);
        const size_t n = size_t(l->length());
        for (size_t i = start; i < n && out.size() < count; ++i)
            out.push_back(makeVar("[" + std::to_string(i) + "]",
                                  l->getElement(i), epoch));
    } else if (isDict(v)) {
        // Bounded enumeration: touches only the requested page.
        auto items = asDict(v)->itemsRange(start, count);
        for (const auto& kv : items)
            out.push_back(makeVar(renderValue(kv.first, limits_.render),
                                  kv.second, epoch));
    } else if (isObjectInstance(v)) {
        ObjectInstance* inst = asObjectInstance(v);
        if (inst->instanceType.isNil())
            return out;
        ObjObjectType* type = asObjectType(inst->instanceType);
        // Declared properties, BACKING FIELDS only -- a user getter at a
        // breakpoint is arbitrary code.  Bounded walk.
        auto entries = collectPropertyEntries(type, start + count + 1);
        for (size_t i = start; i < entries.size() && out.size() < count; ++i)
            out.push_back(makeVar(entries[i].name,
                                  inst->getProperty(entries[i].name), epoch));
    } else if (isFuture(v) || isSignal(v)) {
        // Fixed logical child lists paged with the SAME [start, start+count)
        // contract as every other value.  emit() is lazy so a sliced-out
        // child never creates a handle.
        size_t logicalIdx = 0;
        auto emit = [&](const std::function<DebugVarDesc()>& make) {
            if (logicalIdx >= start && out.size() < count)
                out.push_back(make());
            ++logicalIdx;
        };
        if (isFuture(v)) {
            // Poll only: never resolve, never await.  Reading a READY
            // shared_future's stored value is a plain synchronized read.
            auto& fut = asFuture(v)->future;
            const bool ready = fut.valid()
                && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            emit([&] {
                DebugVarDesc st;
                st.name = "state";
                st.value = ready ? "ready" : "pending";
                st.type = "string";
                return st;
            });
            if (ready)
                emit([&] { return makeVar("value", fut.get(), epoch); });
        } else {
            // Synchronized sample of the last quiesced value (values mutex).
            auto sig = asSignal(v)->signal;
            if (sig && sig->hasValues())
                emit([&] { return makeVar("last", sig->lastValue(), epoch); });
            emit([&] {
                DebugVarDesc n;
                n.name = "samples";
                n.value = std::to_string(sig ? sig->valuesCount() : 0);
                n.type = "int";
                return n;
            });
        }
    }
    return out;
}

// ---- Tier 1 path evaluation -------------------------------------------------

namespace {

// One parsed step of a Tier 1 path: `.name`, `[123]`, or `['key']`.
struct PathStep {
    enum Kind { Prop, IndexInt, IndexStr } kind { Prop };
    std::string name;
    int64_t index { 0 };
};

bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdentChar(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }

// Parse `ident(.ident | [int] | ['str'] | ["str"])*`.  Returns false on any
// deviation -- Tier 1 evaluates paths, never expressions.
bool parsePath(const std::string& exprIn, std::string& root,
               std::vector<PathStep>& steps)
{
    std::string expr = exprIn;
    // trim
    size_t b = expr.find_first_not_of(" \t");
    size_t e = expr.find_last_not_of(" \t");
    if (b == std::string::npos)
        return false;
    expr = expr.substr(b, e - b + 1);

    size_t i = 0;
    if (i >= expr.size() || !isIdentStart(expr[i]))
        return false;
    size_t s = i;
    while (i < expr.size() && isIdentChar(expr[i])) ++i;
    root = expr.substr(s, i - s);

    while (i < expr.size()) {
        if (expr[i] == '.') {
            ++i;
            if (i >= expr.size() || !isIdentStart(expr[i]))
                return false;
            s = i;
            while (i < expr.size() && isIdentChar(expr[i])) ++i;
            steps.push_back({ PathStep::Prop, expr.substr(s, i - s), 0 });
        } else if (expr[i] == '[') {
            ++i;
            if (i < expr.size() && (expr[i] == '\'' || expr[i] == '"')) {
                const char q = expr[i++];
                s = i;
                while (i < expr.size() && expr[i] != q) ++i;
                if (i >= expr.size())
                    return false;
                PathStep st { PathStep::IndexStr, expr.substr(s, i - s), 0 };
                ++i;
                if (i >= expr.size() || expr[i] != ']')
                    return false;
                ++i;
                steps.push_back(std::move(st));
            } else {
                bool neg = false;
                if (i < expr.size() && expr[i] == '-') { neg = true; ++i; }
                s = i;
                while (i < expr.size() && std::isdigit((unsigned char)expr[i])) ++i;
                if (s == i || i >= expr.size() || expr[i] != ']')
                    return false;
                // Checked accumulation: an overlong literal is a parse
                // failure, never a wrapped index that silently reads the
                // wrong element.
                int64_t v = 0;
                for (size_t k = s; k < i; ++k) {
                    if (v > (std::numeric_limits<int64_t>::max() - 9) / 10)
                        return false;
                    v = v * 10 + (expr[k] - '0');
                }
                ++i;
                steps.push_back({ PathStep::IndexInt, {}, neg ? -v : v });
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

DebugInspector::EvalResult DebugInspector::evaluate(int32_t frameHandle,
                                                    const std::string& expression)
{
    ScopedGCMutatorCover gcCover;
    EvalResult res;
    if (!coord_.isStopped()) {
        res.error = "not stopped";
        return res;
    }
    std::string root;
    std::vector<PathStep> steps;
    if (!parsePath(expression, root, steps)) {
        res.error = "only simple paths (name, name.prop, name[index]) are evaluated";
        return res;
    }
    auto h = coord_.handleTable().lookup(frameHandle, coord_.currentEpoch());
    if (!h || h->kind != DebugHandleKind::Frame) {
        res.error = "invalid frame";
        return res;
    }
    ptr<Thread> t;
    size_t fi = 0;
    if (!resolveFrame(*h, t, fi)) {
        res.error = "stale frame";
        return res;
    }
    const CallFrame& f = t->frames[fi];
    const Chunk* ch = frameChunk(f);

    // Root: innermost live local shadow, then upvalues, then module vars.
    Value cur = Value::nilVal();
    bool found = false;
    if (ch && ch->debugInfo) {
        const uint32_t off = frameCodeOffset(*t, fi);
        const size_t slotLimit = frameSlotLimit(*t, f);
        uint32_t bestStart = 0;
        for (const auto& lv : ch->debugInfo->locals) {
            if (off < lv.startOffset || off >= lv.endOffset)
                continue;
            if (lv.slot >= slotLimit)
                continue;
            if (toUTF8StdString(lv.name) != root)
                continue;
            if (!found || lv.startOffset >= bestStart) {
                cur = f.slots[lv.slot];
                bestStart = lv.startOffset;
                found = true;
            }
        }
        if (!found && isClosure(f.closure)) {
            const auto& ups = asClosure(f.closure)->upvalues;
            const auto& names = ch->debugInfo->upvalues;
            for (size_t i = 0; i < ups.size() && i < names.size(); ++i) {
                if (toUTF8StdString(names[i].name) != root || !isUpvalue(ups[i]))
                    continue;
                const ObjUpvalue* uv = asUpvalue(ups[i]);
                if (uv->location) {
                    cur = *uv->location;
                    found = true;
                }
                break;
            }
        }
    }
    if (!found && isClosure(f.closure)) {
        const Value mt = asFunction(asClosure(f.closure)->function)->moduleType;
        if (!mt.isNil()) {
            auto v = asModuleType(mt)->vars.load(toUnicodeString(root));
            if (v.has_value()) {
                cur = v.value();
                found = true;
            }
        }
    }
    if (!found) {
        res.error = "'" + root + "' is not a visible variable";
        return res;
    }
    // (The root scan deliberately does NOT hide DebugLocalSynthetic locals:
    // hover can never ask for one -- __index__ is not in the source text --
    // so an explicit Watch entry is a power user's escape hatch to the loop
    // temps the Variables view filters.)

    // Steps: the same safe accessors as the variables view; anything
    // opaque (futures, signals, actors, natives) refuses traversal.  The
    // try/catch is required: a storage comparator can throw on an
    // incompatible key type -- that must surface as a clean evaluation
    // error, never escape to the caller.
    try {
        for (const auto& st : steps) {
            if (st.kind == PathStep::Prop) {
                if (!isObjectInstance(cur)
                    || asObjectInstance(cur)->instanceType.isNil()) {
                    res.error = "no safe property access on " + describeValueType(cur);
                    return res;
                }
                // Presence check: absence must be an error, not a fake nil.
                ObjObjectType* type = asObjectType(asObjectInstance(cur)->instanceType);
                bool hasProp = false;
                for (const auto& pe : collectPropertyEntries(type))
                    if (pe.name == st.name) { hasProp = true; break; }
                if (!hasProp) {
                    res.error = "no property '" + st.name + "' on "
                                + describeValueType(cur);
                    return res;
                }
                cur = asObjectInstance(cur)->getProperty(st.name);
            } else if (st.kind == PathStep::IndexInt) {
                if (isList(cur)) {
                    const ObjList* l = asList(cur);
                    if (st.index < 0 || st.index >= l->length()) {
                        res.error = "index out of range";
                        return res;
                    }
                    cur = l->getElement(size_t(st.index));
                } else if (isDict(cur)) {
                    const Value key = Value::intVal(st.index);
                    if (!asDict(cur)->contains(key)) {
                        res.error = "key not found";
                        return res;
                    }
                    cur = asDict(cur)->at(key);
                } else {
                    res.error = "no safe indexing on " + describeValueType(cur);
                    return res;
                }
            } else {   // IndexStr
                if (!isDict(cur)) {
                    res.error = "no safe string indexing on " + describeValueType(cur);
                    return res;
                }
                const Value key = Value::stringVal(toUnicodeString(st.name));
                if (!asDict(cur)->contains(key)) {
                    res.error = "key not found";
                    return res;
                }
                cur = asDict(cur)->at(key);
            }
        }
    } catch (const std::exception& e) {
        res.error = std::string("evaluation failed: ") + e.what();
        return res;
    } catch (...) {
        res.error = "evaluation failed";
        return res;
    }

    DebugVarDesc d = makeVar(expression, cur, coord_.currentEpoch());
    res.ok = true;
    res.value = std::move(d.value);
    res.type = std::move(d.type);
    res.childHandle = d.childHandle;
    return res;
}

} // namespace roxal
