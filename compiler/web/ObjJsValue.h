#pragma once

#ifdef __EMSCRIPTEN__

#include "Object.h"

#include <cstdint>

namespace roxal {

// A Roxal handle onto a JavaScript value living in the browser main thread's
// handle table. Native property and method syntax routes here:
//
//   el.text_content = "hi"          -> trySetDynamicProperty
//   var ctx = c.get_context("2d")   -> tryInvokeDynamicMethod
//
// Structurally the same idea as ObjQtObject, with a table index in place of a
// QPointer: the JS object is owned by the page, and collecting this wrapper
// only releases our reference to it.
//
// Naming: Roxal snake_case maps to JS camelCase (text_content -> textContent).
// Use dom.get()/dom.set()/dom.call() for names that are not valid Roxal
// identifiers, or when the name is computed.
struct ObjJsValue : public Obj {
    explicit ObjJsValue(uint32_t h) : handle(h) { type = ObjType::JsValue; }
    ~ObjJsValue() override;      // queues a Release for the main thread

    uint32_t handle;

    bool tryGetDynamicProperty(const Value& self, const ustring& name, Value& out) override;
    bool trySetDynamicProperty(const ustring& name, const Value& value) override;
    bool tryInvokeDynamicMethod(const ustring& name, const Value* args, int argCount, Value& out) override;

    // Holds no Roxal Values, so nothing to trace. Cloning shares the handle --
    // copying a handle must not duplicate the JS object.
    void trace(ValueVisitor& visitor) const override { (void)visitor; }
    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isJsValue(const Value& v) { return isObjType(v, ObjType::JsValue); }
inline ObjJsValue* asJsValue(const Value& v) { return static_cast<ObjJsValue*>(v.asObj()); }

// snake_case -> camelCase, the default Roxal->JS name mapping.
std::string toJsName(const ustring& roxalName);

} // namespace roxal

#endif // __EMSCRIPTEN__
