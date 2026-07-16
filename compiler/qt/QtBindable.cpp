#ifdef ROXAL_ENABLE_QT

// Roxal headers first (signals/slots/emit macro clash), then Qt.
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "GCRoots.h"
#include "ModuleQtConvert.h"
#include "dataflow/Signal.h"
#include "QtBindable.h"

#include <QVariant>
#include <QVariantList>
#include <QString>
#include <QJSValue>
#include <QJSEngine>
#include <QQmlEngine>
#include <QMetaObject>
#include <QtCore/private/qmetaobjectbuilder_p.h>   // moc-free runtime metaobject

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace roxal;

// ============================================================
// RoxalMethodBridge — a moc-free dynamic QObject that makes a Roxal object's methods
// callable from QML. It carries ONE runtime-built invokable, __call(name, args), which
// QML reaches through the JS forwarder values installed on the property map; the call is
// routed back into RoxalPropertyMap::callMethod → VM::invokeMethod. (This is the same
// metaobject moc would generate for a Q_INVOKABLE, but constructed at runtime so the
// plugin needs no moc / AUTOMOC — see QMetaObjectBuilder.)
// ============================================================

namespace roxal {

class RoxalMethodBridge : public QObject {
public:
    RoxalMethodBridge(RoxalPropertyMap* map, std::shared_ptr<std::atomic<bool>> alive)
        : map_(map), alive_(std::move(alive))
    {
        QMetaObjectBuilder b;
        b.setClassName("RoxalMethodBridge");
        b.setSuperClass(&QObject::staticMetaObject);
        QMetaMethodBuilder mm = b.addMethod("__call(QString,QVariantList)");
        mm.setReturnType("QVariant");
        meta_ = b.toMetaObject();
    }
    ~RoxalMethodBridge() override { std::free(meta_); }

    const QMetaObject* metaObject() const override { return meta_; }
    void* qt_metacast(const char* clname) override {
        if (clname && std::strcmp(clname, "RoxalMethodBridge") == 0)
            return static_cast<void*>(this);
        return QObject::qt_metacast(clname);
    }
    int qt_metacall(QMetaObject::Call c, int id, void** a) override {
        id = QObject::qt_metacall(c, id, a);
        if (id < 0) return id;
        if (c == QMetaObject::InvokeMetaMethod) {
            if (id == 0) {   // __call(QString name, QVariantList args) -> QVariant
                QVariant result;
                if (alive_ && alive_->load() && map_) {
                    const QString& name = *reinterpret_cast<QString*>(a[1]);
                    const QVariantList& args = *reinterpret_cast<QVariantList*>(a[2]);
                    result = map_->callMethod(name, args);
                }
                if (a[0]) *reinterpret_cast<QVariant*>(a[0]) = result;
            }
            id -= 1;   // we declared exactly one method
        }
        return id;
    }

private:
    RoxalPropertyMap* map_;                       // owner (bridge lifetime ⊆ map lifetime)
    std::shared_ptr<std::atomic<bool>> alive_;    // no-op any late call past destruction
    QMetaObject* meta_ = nullptr;
};

} // namespace roxal

// ============================================================
// RoxalPropertyMap
// ============================================================

RoxalPropertyMap::RoxalPropertyMap(const Value& obj)
    : QQmlPropertyMap(this, nullptr),   // protected ctor: register the derived metaobject
      obj_(obj), alive_(std::make_shared<std::atomic<bool>>(true))
{
    buildRoles();
    buildMethods();
    initValues();
    hookSignals();
}

RoxalPropertyMap::~RoxalPropertyMap()
{
    *alive_ = false;   // any late signal callback becomes a no-op
}

void RoxalPropertyMap::buildRoles()
{
    roles_.clear();
    if (!isObjectInstance(obj_)) return;
    ObjObjectType* t = asObjectType(asObjectInstance(obj_)->instanceType);
    if (!t) return;
    for (const auto& pv : t->orderedPublicProperties()) {
        Role r;
        r.uname     = pv.property->name;
        r.nameHash  = pv.property->name.hashCode();
        r.name      = QString::fromStdString(toUTF8StdString(pv.property->name));
        r.editable  = !pv.property->isConst;
        roles_.push_back(r);
    }

    // Computed (accessor) properties aren't in orderedPublicProperties — they live as
    // synthesized __get_<name> / __set_<name> methods (the getter method's access mirrors
    // the property's). Expose each PUBLIC one as a computed role: read via __get_, write via
    // __set_ (get-only → read-only). Own type only, matching the stored-property surface.
    std::unordered_set<int32_t> seen;
    for (const auto& r : roles_) seen.insert(r.nameHash);
    const icu::UnicodeString kGet("__get_");
    for (const auto& mentry : t->methods) {
        for (const auto& m : mentry.second.overloads) {
            if (m.access != ast::Access::Public || !m.name.startsWith(kGet))
                continue;
            icu::UnicodeString propName = m.name.tempSubString(kGet.length());  // strip "__get_"
            int32_t propHash = propName.hashCode();
            if (!seen.insert(propHash).second)
                continue;   // already a stored property or duplicate getter overload
            Role r;
            r.uname       = propName;
            r.nameHash    = propHash;
            r.name        = QString::fromStdString(toUTF8StdString(propName));
            r.computed    = true;
            r.getterName  = m.name;                                    // "__get_<name>"
            r.setterName  = icu::UnicodeString("__set_") + propName;   // "__set_<name>"
            r.backingHash = (icu::UnicodeString("_") + propName).hashCode();
            ObjObjectType::Method* setter = t->findUniqueMethod(r.setterName.hashCode());
            r.editable    = (setter != nullptr && setter->access == ast::Access::Public);
            roles_.push_back(r);
        }
    }
}

Value RoxalPropertyMap::readRole(const Role& role) const
{
    if (!isObjectInstance(obj_))
        return Value::nilVal();
    if (role.computed) {
        // Call the synthesized getter (re-enters the VM; safe from a parked Qt callback).
        auto [status, val] = VM::instance().invokeMethod(obj_, role.getterName, {});
        return status == ExecutionStatus::OK ? val : Value::nilVal();
    }
    return asObjectInstance(obj_)->getProperty(role.uname);
}

void RoxalPropertyMap::buildMethods()
{
    methodNames_.clear();
    if (!isObjectInstance(obj_)) return;
    ObjObjectType* t = asObjectType(asObjectInstance(obj_)->instanceType);
    if (!t) return;
    const icu::UnicodeString kGet("__get_"), kSet("__set_"), kInit("init");
    for (const auto& mentry : t->methods) {
        const auto& overloads = mentry.second.overloads;
        if (overloads.size() != 1) continue;          // VM::invokeMethod resolves a UNIQUE method
        const ObjObjectType::Method& m = overloads[0];
        if (m.access != ast::Access::Public) continue; // public surface only
        if (m.name == kInit) continue;                 // the constructor isn't a QML action
        if (m.name.startsWith(kGet) || m.name.startsWith(kSet)) continue;  // computed-property accessors
        if (m.name.startsWith("_")) continue;          // private/internal naming convention
        methodNames_.push_back(m.name);
    }
}

QVariant RoxalPropertyMap::callMethod(const QString& name, const QVariantList& args)
{
    if (!isObjectInstance(obj_)) return QVariant();
    icu::UnicodeString uname = icu::UnicodeString::fromUTF8(name.toUtf8().constData());
    std::vector<Value> vmArgs;
    vmArgs.reserve(static_cast<size_t>(args.size()));
    for (const QVariant& a : args) {
        try { vmArgs.push_back(fromQVariant(a)); }
        catch (...) { vmArgs.push_back(Value::nilVal()); }
    }
    // invokeMethod re-enters the VM dispatch loop (and is parked-callback safe). This always
    // runs on the UI thread (QML invoked it), so there is no actor/thread-affinity concern.
    auto [status, result] = VM::instance().invokeMethod(obj_, uname, vmArgs);
    if (status != ExecutionStatus::OK) return QVariant();
    try { return toQVariant(result); } catch (...) { return QVariant(); }
}

void RoxalPropertyMap::installMethods(QQmlEngine* engine)
{
    if (methodsInstalled_ || methodNames_.empty() || !engine) return;
    methodsInstalled_ = true;

    bridge_ = std::make_unique<RoxalMethodBridge>(this, alive_);
    // C++-owned: the hub/map owns the bridge; JS must never garbage-collect it.
    QQmlEngine::setObjectOwnership(bridge_.get(), QQmlEngine::CppOwnership);

    QJSValue jsBridge = engine->newQObject(bridge_.get());
    // Generic forwarder factory: (bridge, methodName) → a JS function that calls
    // bridge.__call(methodName, [args...]). One per method, stored as the map value so
    // QML can call `app.method(a, b)` natively.
    QJSValue factory = engine->evaluate(
        "(function(b, m){ return function(){ "
        "return b.__call(m, Array.prototype.slice.call(arguments)); }; })");
    if (!factory.isCallable()) return;

    for (const auto& uname : methodNames_) {
        QString qname = QString::fromStdString(toUTF8StdString(uname));
        QJSValue fn = factory.call(QJSValueList{ jsBridge, QJSValue(qname) });
        if (fn.isCallable())
            insert(qname, QVariant::fromValue(fn));
    }
}

void RoxalPropertyMap::initValues()
{
    if (!isObjectInstance(obj_)) return;
    for (const auto& r : roles_) {
        QVariant v;
        try { v = toQVariant(readRole(r)); }
        catch (...) { v = QVariant(); }   // non-convertible (e.g. nested object) → null
        insert(r.name, v);
    }
}

void RoxalPropertyMap::hookSignals()
{
    if (!isObjectInstance(obj_)) return;
    ObjectInstance* inst = asObjectInstance(obj_);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    RoxalPropertyMap* self = this;
    for (const auto& r : roles_) {
        // Observe the property's changes via the lightweight ChangeNotifier — binding an
        // object to QML creates NO dataflow signal. Fires synchronously on the VM/UI
        // thread when a Roxal-side write changes the value (assign() gates unchanged
        // writes). If the property is later also used in a Roxal `when … changes`,
        // ensureSignal() upgrades the notifier to a full signal and migrates this callback.
        //
        // A computed property observes its `_<name>` backing field (what its setter writes);
        // on change, onRoxalChange re-reads via the getter. Getters that depend on OTHER
        // fields won't auto-fire — use qt.notify(obj, name) for those.
        Role role = r;   // capture by value
        const int32_t observeHash = role.computed ? role.backingHash : role.nameHash;
        const icu::UnicodeString observeName =
            role.computed ? (icu::UnicodeString("_") + role.uname) : role.uname;
        inst->observePropertyChange(observeHash, toUTF8StdString(observeName),
            [alive, self, role](TimePoint, ptr<df::Signal>, const Value&) {
                if (!alive->load()) return;   // wrapper destroyed → ignore
                self->onRoxalChange(role);
            });
    }
}

const RoxalPropertyMap::Role* RoxalPropertyMap::roleByName(const QString& name) const
{
    for (const auto& r : roles_)
        if (r.name == name) return &r;
    return nullptr;
}

void RoxalPropertyMap::onRoxalChange(const Role& role)
{
    if (suppressKey_ == role.name)
        return;   // this change originated from the QML write we're servicing
    QVariant qv;
    try { qv = toQVariant(readRole(role)); }   // re-read (correct for computed properties too)
    catch (...) { qv = QVariant(); }
    insert(role.name, qv);   // QML bindings on this key update (insert() emits no valueChanged)
}

QVariant RoxalPropertyMap::updateValue(const QString& key, const QVariant& input)
{
    const Role* r = roleByName(key);
    if (!r || !r->editable || !isObjectInstance(obj_))
        return value(key);   // unknown / const → reject by keeping the current value
    ObjectInstance* inst = asObjectInstance(obj_);
    suppressKey_ = key;      // suppress the echo from our own change observer
    Value newVal;
    try { newVal = fromQVariant(input); } catch (...) { newVal = Value::nilVal(); }
    QVariant result = input;            // store what QML wrote into the map
    if (r->computed) {
        // Drive the value through the user's setter (re-enters the VM; safe while parked).
        VM::instance().invokeMethod(obj_, r->setterName, { newVal });
        // Reflect the post-setter value — a transforming setter/getter may differ from input.
        try { result = toQVariant(readRole(*r)); } catch (...) { result = input; }
    } else {
        inst->propertySlot(r->nameHash).assign(newVal);   // gated write
    }
    suppressKey_ = QString();
    return result;
}

void RoxalPropertyMap::pushProperty(const QString& name)
{
    const Role* r = roleByName(name);
    if (!r || !isObjectInstance(obj_)) return;
    onRoxalChange(*r);
}

void RoxalPropertyMap::pushAll()
{
    if (!isObjectInstance(obj_)) return;
    for (const auto& r : roles_)
        onRoxalChange(r);
}

// ============================================================
// QtBindHub (owner; the maps member is the GC root -- TracedMember, v2 B)
// ============================================================

struct QtBindHub::Impl {
    // Keep each exposed object reachable (its properties + change signals
    // follow).  Registered for the Impl's whole lifetime; shutdown()
    // clears the container, so post-shutdown traces see nothing.
    static void traceMaps(ValueVisitor& visitor,
                          const std::vector<std::unique_ptr<RoxalPropertyMap>>& maps) {
        for (auto& m : maps)
            if (m) visitor.visit(m->objValue());
    }
    TracedMember<std::vector<std::unique_ptr<RoxalPropertyMap>>> maps { &Impl::traceMaps };
    std::unordered_map<ObjectInstance*, RoxalPropertyMap*> byObject;
};

QtBindHub::QtBindHub() : impl_(std::make_unique<Impl>()) {}
QtBindHub::~QtBindHub() { shutdown(); }

QtBindHub& QtBindHub::instance()
{
    static QtBindHub hub;
    return hub;
}

void QtBindHub::init()
{
}

void QtBindHub::shutdown()
{
    impl_->byObject.clear();
    impl_->maps->clear();   // destroys the wrappers (sets their alive-guard false)
}

RoxalPropertyMap* QtBindHub::wrap(const Value& obj)
{
    if (!isObjectInstance(obj)) return nullptr;
    ObjectInstance* key = asObjectInstance(obj);
    auto it = impl_->byObject.find(key);
    if (it != impl_->byObject.end())
        return it->second;   // idempotent: one wrapper per object
    auto m = std::make_unique<RoxalPropertyMap>(obj);
    RoxalPropertyMap* raw = m.get();
    impl_->maps->push_back(std::move(m));
    impl_->byObject[key] = raw;
    return raw;
}

RoxalPropertyMap* QtBindHub::lookup(ObjectInstance* obj)
{
    auto it = impl_->byObject.find(obj);
    return it != impl_->byObject.end() ? it->second : nullptr;
}

#endif // ROXAL_ENABLE_QT
