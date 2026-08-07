#ifdef __EMSCRIPTEN__

#include "ObjJsValue.h"
#include "JsBridge.h"

#include <cctype>
#include <stdexcept>
#include <string>

using namespace roxal;
using namespace roxal::web;

// snake_case -> camelCase. Chosen over passing names through verbatim so that
// Roxal code reads like Roxal; dom.get()/dom.set() take the exact JS name when
// that matters (hyphenated attributes, computed names).
std::string roxal::toJsName(const ustring& roxalName)
{
    const std::string in = toUTF8StdString(roxalName);
    std::string out;
    out.reserve(in.size());
    bool upperNext = false;
    for (char c : in) {
        if (c == '_') { upperNext = true; continue; }
        out.push_back(upperNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        upperNext = false;
    }
    return out;
}

ObjJsValue::~ObjJsValue()
{
    if (handle == kNullHandle) return;
    // Deferred, never synchronous: this can run on the GC thread, and a
    // synchronous proxy call from there would block collection on the browser's
    // event loop.
    if (!canIssueOps()) return;
    Encoder e;
    e.op(Op::Release);
    e.u32(handle);
    defer(e);
}

bool ObjJsValue::tryGetDynamicProperty(const Value& self, const ustring& name, Value& out)
{
    (void)self;
    Encoder e;
    e.op(Op::Get);
    e.u32(handle);
    e.str(toJsName(name));
    out = exec(e);
    return true;
}

bool ObjJsValue::trySetDynamicProperty(const ustring& name, const Value& value)
{
    Encoder e;
    e.op(Op::Set);
    e.u32(handle);
    e.str(toJsName(name));
    e.value(value);
    // A write has no result, so it costs nothing to batch -- and batching is what
    // keeps a mutation-heavy script from paying one event-loop turn per property.
    defer(e);
    return true;
}

bool ObjJsValue::tryInvokeDynamicMethod(const ustring& name, const Value* args, int argCount, Value& out)
{
    Encoder e;
    e.op(Op::Call);
    e.u32(handle);
    e.str(toJsName(name));
    e.u32(static_cast<uint32_t>(argCount < 0 ? 0 : argCount));
    for (int i = 0; i < argCount; ++i) e.value(args[i]);
    out = exec(e);
    return true;
}

unique_ptr<Obj, UnreleasedObj> ObjJsValue::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // a JS handle is an opaque reference; cloning shares it
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjJsValue*>(this));
}

void ObjJsValue::write(std::ostream&, roxal::ptr<SerializationContext>) const
{
    throw std::runtime_error("dom: JavaScript handles cannot be serialized");
}

void ObjJsValue::read(std::istream&, roxal::ptr<SerializationContext>)
{
    throw std::runtime_error("dom: JavaScript handles cannot be deserialized");
}

Value roxal::web::jsValue(uint32_t handle)
{
    if (handle == kNullHandle)
        return Value::nilVal();   // JS null/undefined is Roxal nil
    #ifdef DEBUG_BUILD
    return Value::objVal(newObj<ObjJsValue>(__func__, __FILE__, __LINE__, handle));
    #else
    return Value::objVal(newObj<ObjJsValue>(handle));
    #endif
}

#endif // __EMSCRIPTEN__
