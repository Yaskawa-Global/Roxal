#ifdef ROXAL_ENABLE_QT

// Roxal headers first (signals/slots/emit macro clash), then Qt.
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "ModuleQtConvert.h"
#include "dataflow/Signal.h"
#include "QtBindable.h"

#include <QVariant>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace roxal;

// ============================================================
// RoxalPropertyMap
// ============================================================

RoxalPropertyMap::RoxalPropertyMap(const Value& obj)
    : QQmlPropertyMap(this, nullptr),   // protected ctor: register the derived metaobject
      obj_(obj), alive_(std::make_shared<std::atomic<bool>>(true))
{
    buildRoles();
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
// QtBindHub (owner + GC ExternalRootProvider)
// ============================================================

struct QtBindHub::Impl : SimpleMarkSweepGC::ExternalRootProvider {
    std::vector<std::unique_ptr<RoxalPropertyMap>> maps;
    std::unordered_map<ObjectInstance*, RoxalPropertyMap*> byObject;
    bool rootRegistered { false };

    // Keep each exposed object reachable (its properties + change signals follow).
    void visitRoots(ValueVisitor& visitor) override {
        for (auto& m : maps)
            if (m) visitor.visit(m->objValue());
    }
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
    if (!impl_->rootRegistered) {
        SimpleMarkSweepGC::instance().registerExternalRootProvider(impl_.get());
        impl_->rootRegistered = true;
    }
}

void QtBindHub::shutdown()
{
    impl_->byObject.clear();
    impl_->maps.clear();   // destroys the wrappers (sets their alive-guard false)
    if (impl_->rootRegistered) {
        SimpleMarkSweepGC::instance().unregisterExternalRootProvider(impl_.get());
        impl_->rootRegistered = false;
    }
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
    impl_->maps.push_back(std::move(m));
    impl_->byObject[key] = raw;
    return raw;
}

RoxalPropertyMap* QtBindHub::lookup(ObjectInstance* obj)
{
    auto it = impl_->byObject.find(obj);
    return it != impl_->byObject.end() ? it->second : nullptr;
}

#endif // ROXAL_ENABLE_QT
