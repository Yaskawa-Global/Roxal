#include "ValueRender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <unordered_set>

#include "../Object.h"
#include "../Introspection.h"

namespace roxal {
namespace {

struct RenderCtx {
    const RenderOptions& opts;
    std::string out;
    std::unordered_set<const Obj*> path;  // ancestors on the recursion path (cycle guard)
    bool truncated = false;

    // False once the total budget is exhausted (appends one marker).
    bool budgetLeft() {
        if (out.size() >= opts.maxTotalChars) {
            if (!truncated) { out += "..."; truncated = true; }
            return false;
        }
        return true;
    }
};

void renderInto(const Value& v, RenderCtx& ctx, size_t depth);

// Escape so a rendered string is always one line and quote-delimited:
// backslash, quote, \n \r \t named; other C0 control bytes as \xNN.
// UTF-8 multibyte sequences (>= 0x80) pass through untouched.
void appendEscaped(std::string& out, const std::string& s)
{
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
}

void renderString(const Value& v, RenderCtx& ctx)
{
    const ustring& s = asStringObj(v)->s;
    const bool clipped = (size_t)s.length() > ctx.opts.maxStringChars;
    std::string utf8;
    if (clipped) {
        utf8 = toUTF8StdString(s.tempSubString(0, (int32_t)ctx.opts.maxStringChars));
    } else {
        utf8 = toUTF8StdString(s);
    }
    ctx.out += "'";
    appendEscaped(ctx.out, utf8);
    ctx.out += clipped ? "...'" : "'";
}

void renderList(const Value& v, RenderCtx& ctx, size_t depth)
{
    const ObjList* l = asList(v);
    const size_t n = (size_t)l->length();
    if (depth >= ctx.opts.maxDepth) {
        ctx.out += "List(" + std::to_string(n) + ")";
        return;
    }
    ctx.out += "[";
    const size_t shown = std::min(n, ctx.opts.maxChildren);
    size_t i = 0;
    for (; i < shown && ctx.budgetLeft(); ++i) {
        if (i) ctx.out += ", ";
        renderInto(l->getElement(i), ctx, depth + 1);
    }
    if (n > i) ctx.out += ", ...+" + std::to_string(n - i);
    ctx.out += "]";
}

void renderDict(const Value& v, RenderCtx& ctx, size_t depth)
{
    const ObjDict* d = asDict(v);
    const size_t n = (size_t)d->length();
    if (depth >= ctx.opts.maxDepth) {
        ctx.out += "Dict(" + std::to_string(n) + ")";
        return;
    }
    // Bounded copy: at most maxChildren entries are ever touched -- a huge
    // dict must not cost O(n) copies/refcount traffic to preview.
    auto items = d->itemsPrefix(ctx.opts.maxChildren);
    ctx.out += "{";
    size_t i = 0;
    for (; i < items.size() && ctx.budgetLeft(); ++i) {
        if (i) ctx.out += ", ";
        renderInto(items[i].first, ctx, depth + 1);
        ctx.out += ": ";
        renderInto(items[i].second, ctx, depth + 1);
    }
    if (n > i) ctx.out += ", ...+" + std::to_string(n - i);
    ctx.out += "}";
}

void renderInstance(const Value& v, RenderCtx& ctx, size_t depth)
{
    ObjectInstance* inst = asObjectInstance(v);
    if (inst->instanceType.isNil()) {
        ctx.out += "<object>";
        return;
    }
    ObjObjectType* t = asObjectType(inst->instanceType);
    const std::string tname = toUTF8StdString(t->name);
    if (depth >= ctx.opts.maxDepth) {
        ctx.out += tname + "{...}";
        return;
    }
    // Declared properties only, read from backing fields -- never getters.
    // A computed property may therefore show a stale backing value; the
    // debugger UI surfaces that explicitly.
    // Bounded walk (+1 so a cap hit is detectable); remainder shown without
    // a count, since the total is deliberately not computed.
    auto entries = collectPropertyEntries(t, ctx.opts.maxChildren + 1);
    ctx.out += tname + "{";
    size_t shown = 0;
    for (const auto& e : entries) {
        if (shown >= ctx.opts.maxChildren || !ctx.budgetLeft()) break;
        if (shown) ctx.out += ", ";
        ctx.out += e.name + ": ";
        renderInto(inst->getProperty(e.name), ctx, depth + 1);
        ++shown;
    }
    if (entries.size() > shown) ctx.out += ", ...";
    ctx.out += "}";
}

void renderInto(const Value& v, RenderCtx& ctx, size_t depth)
{
    if (!ctx.budgetLeft()) return;

    // Modules are ObjTypeSpec-backed; check before the scalar/type switch.
    if (isModuleType(v)) {
        ctx.out += "<module " + toUTF8StdString(asModuleType(v)->name) + ">";
        return;
    }

    // Futures MUST be intercepted before the scalar switch: ObjType::Future
    // has no ValueType mapping (Obj::valueType() reports Nil), so a pending
    // future would take the scalar toString path -- and objToString RESOLVES
    // futures, which blocks forever when the producer thread is stopped by
    // the debugger.  Poll only -- never resolve, never wait.
    if (isFuture(v)) {
        auto& f = asFuture(v)->future;
        const bool ready = f.valid()
            && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        ctx.out += ready ? "<future ready>" : "<future pending>";
        return;
    }

    switch (v.type()) {
        case ValueType::Nil:
        case ValueType::Bool:
        case ValueType::Byte:
        case ValueType::Int:
        case ValueType::Real:
        case ValueType::Decimal:
        case ValueType::Enum:
        case ValueType::Type:
            // Scalar/type formatting is static -- no user code involved.
            ctx.out += roxal::toString(v);
            return;
        default:
            break;
    }

    if (isString(v)) { renderString(v, ctx); return; }

    Obj* obj = v.asObj();
    if (obj && !ctx.path.insert(obj).second) {
        ctx.out += "...";  // cycle: this object is its own ancestor
        return;
    }
    struct PathPop {
        RenderCtx& c; const Obj* o;
        ~PathPop() { if (o) c.path.erase(o); }
    } pop { ctx, obj };

    if (isActorInstance(v)) {
        // Opaque: never call into an actor. describeValueType already
        // includes the "actor" prefix (e.g. "actor Worker").
        ctx.out += "<" + describeValueType(v) + ">";
    } else if (isSignal(v)) {
        // Opaque until the debugger's synchronized signal snapshot lands;
        // sampling here would need the signal lock.
        ctx.out += "<signal>";
    } else if (isList(v)) {
        renderList(v, ctx, depth);
    } else if (isDict(v)) {
        renderDict(v, ctx, depth);
    } else if (isTensor(v)) {
        const ObjTensor* t = asTensor(v);
        std::string dims;
        for (int64_t d : t->shape()) {
            if (!dims.empty()) dims += "x";
            dims += std::to_string(d);
        }
        ctx.out += "<tensor " + roxal::to_string(t->dtype()) + "[" + dims + "]>";
    } else if (isClosure(v) || isFunction(v)) {
        const ObjFunction* fn = isClosure(v)
            ? asFunction(asClosure(v)->function) : asFunction(v);
        const std::string name = toUTF8StdString(fn->name);
        ctx.out += name.empty()
            ? "<func>"
            : "<func " + name + "/" + std::to_string(fn->arity) + ">";
    } else if (isRange(v)) {
        const ObjRange* r = asRange(v);
        renderInto(r->start, ctx, depth + 1);
        ctx.out += r->closed ? ".." : "..<";
        renderInto(r->stop, ctx, depth + 1);
        if (r->step.isNonNil()) {
            ctx.out += " step ";
            renderInto(r->step, ctx, depth + 1);
        }
    } else if (isObjectInstance(v)) {
        renderInstance(v, ctx, depth);
    } else {
        // Everything else (vector, matrix, orient, event, native, qt/js
        // handles, ...) renders as an opaque typed marker.
        ctx.out += "<" + describeValueType(v) + ">";
    }
}

} // namespace

std::string renderValue(const Value& v, const RenderOptions& opts)
{
    RenderCtx ctx { opts };
    renderInto(v, ctx, 0);
    // Hard cap, marker included: the result never exceeds maxTotalChars
    // (structural closers appended after budget exhaustion are trimmed here).
    if (ctx.out.size() > opts.maxTotalChars) {
        const size_t keep = opts.maxTotalChars > 3 ? opts.maxTotalChars - 3 : 0;
        ctx.out.resize(keep);
        ctx.out += "...";
    }
    return ctx.out;
}

} // namespace roxal
