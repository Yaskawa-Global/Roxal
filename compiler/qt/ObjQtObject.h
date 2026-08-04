#pragma once

#ifdef ROXAL_ENABLE_QT

#include "Object.h"
#include <QPointer>

class QObject;
class QMetaMethod;

namespace roxal {

// Find a Qt signal meta-method by name on `o` (first match; invalid QMetaMethod
// if none). Shared by the dynamic dispatch and the qt.connect/on module fns.
QMetaMethod findQtSignal(QObject* o, const char* name);


// Non-owning, auto-nulling handle to a QObject / QML item. Native property and
// method syntax (`item.text`, `item.foo(args)`) routes through the dynamic-dispatch
// virtuals overridden here. The QObject is owned by Qt (the QML engine); Roxal GC
// collecting this wrapper never deletes it, and `trace()` is empty (no Roxal Values).
struct ObjQtObject : public Obj {
    explicit ObjQtObject(QObject* o) : qobj(o) { type = ObjType::QtObject; }
    virtual ~ObjQtObject() {}

    QPointer<QObject> qobj; // non-owning; auto-nulls when the QObject is destroyed

    // Null-guard: return the live QObject or throw (→ catchable Roxal exception).
    // Every override calls this first, except the `valid` intrinsic.
    QObject* deref(const char* op) const;

    bool tryGetDynamicProperty(const Value& self, const ustring& name, Value& out) override;
    bool trySetDynamicProperty(const ustring& name, const Value& value) override;
    bool tryInvokeDynamicMethod(const ustring& name, const Value* args, int argCount, Value& out) override;

    // No Roxal Value refs held; share-on-clone (a handle copy); not serializable.
    void trace(ValueVisitor& visitor) const override { (void)visitor; }
    unique_ptr<Obj, UnreleasedObj> clone(roxal::ptr<CloneContext> ctx) const override;
    void write(std::ostream& out, roxal::ptr<SerializationContext> ctx = nullptr) const override;
    void read(std::istream& in, roxal::ptr<SerializationContext> ctx = nullptr) override;
};

inline bool isQtObject(const Value& v) { return isObjType(v, ObjType::QtObject); }
inline ObjQtObject* asQtObject(const Value& v) { return static_cast<ObjQtObject*>(v.asObj()); }

unique_ptr<ObjQtObject, UnreleasedObj> newQtObjectObj(QObject* obj);
// Wrap a QObject* as a Roxal Value (nil if obj is null).
Value qtObjectValue(QObject* obj);

} // namespace roxal

#endif // ROXAL_ENABLE_QT
