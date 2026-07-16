#pragma once

#ifdef ROXAL_ENABLE_QT

// Roxal headers first — they use identifiers (signals/slots/emit) that Qt defines
// as macros, so the Qt headers must come after them.
#include "Object.h"   // pulls Value.h

#include <QQmlPropertyMap>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <vector>

class QQmlEngine;

namespace roxal {

class RoxalMethodBridge;   // moc-free dynamic QObject (QMetaObjectBuilder) — defined in the .cpp

// A moc-free bindable wrapper that exposes ONE Roxal object's public properties to
// QML as QQmlPropertyMap key→value pairs (the Q_PROPERTY-with-NOTIFY analogue):
//  - init/read: each role is read and inserted under its property name — a stored
//    property via getProperty, a computed (accessor) property via its __get_ method.
//  - QML write: updateValue() routes to the property — a stored property through Roxal's
//    gated assign(), a computed one through its __set_ method (get-only → read-only).
//  - Roxal edit: each property's change is observed via a lightweight ChangeNotifier (a
//    computed property observes its `_<name>` backing field) and the new value pushed via
//    insert() so QML re-binds.
// Stored vs computed: stored (`var`) properties surface in orderedPublicProperties();
// computed (accessor) ones don't — they're discovered from their __get_/__set_ methods.
// moc-free: only the virtual updateValue() is overridden; no Q_OBJECT/Q_PROPERTY.
class RoxalPropertyMap : public QQmlPropertyMap {
public:
    explicit RoxalPropertyMap(const Value& obj);
    ~RoxalPropertyMap() override;

    // Re-read property `name` (or all) from the Roxal object and push to QML. Used by
    // qt.notify for in-place mutation of a contained collection (where the property's
    // reference is unchanged, so assign() — and thus auto-notify — never fires).
    void pushProperty(const QString& name);
    void pushAll();

    const Value& objValue() const { return obj_; }   // for the hub's GC root provider

    // Expose the object's public methods to QML as callable values: for each method, a JS
    // forwarder is stored under its name so QML can call `app.method(args)` natively. The
    // forwarders route through a per-map dynamic QObject bridge into VM::invokeMethod.
    // Needs the QML engine (for JS values), so it's called at set_context_property time.
    // Idempotent; assumes a single engine per exposed object.
    void installMethods(QQmlEngine* engine);

    // Invoke a Roxal method by name with QVariant args (called by the bridge from QML).
    QVariant callMethod(const QString& name, const QVariantList& args);

protected:
    QVariant updateValue(const QString& key, const QVariant& input) override;

private:
    struct Role {
        QString name; icu::UnicodeString uname; int32_t nameHash; bool editable;
        bool computed { false };          // accessor property: read via __get_, write via __set_
        icu::UnicodeString getterName;    // "__get_<name>"  (computed only)
        icu::UnicodeString setterName;    // "__set_<name>"  (computed only)
        int32_t backingHash { 0 };        // hash of the "_<name>" backing field (computed; auto-notify)
    };
    void buildRoles();
    void buildMethods();      // collect public, non-overloaded, non-accessor method names
    void initValues();
    void hookSignals();
    const Role* roleByName(const QString& name) const;
    Value readRole(const Role& role) const;   // getter (computed) or stored slot (plain)
    void onRoxalChange(const Role& role);     // re-read the role and push to QML

    Value obj_;                                   // wrapped ObjectInstance (traced by hub)
    std::vector<Role> roles_;
    std::vector<icu::UnicodeString> methodNames_; // public methods exposed as callable values
    std::shared_ptr<std::atomic<bool>> alive_;    // guards signal callbacks past destruction
    QString suppressKey_;                         // re-entrancy guard during a QML write
    std::unique_ptr<RoxalMethodBridge> bridge_;   // dynamic QObject for QML→method dispatch
    bool methodsInstalled_ { false };
};

// Owns every live RoxalPropertyMap and keeps its wrapped object GC-alive via a GC
// its maps member is a typed GC root (TracedMember). Singleton; lifetime bracketed by ModuleQt (init() at module
// load, shutdown() at script-complete teardown), mirroring QtModelHub / QtSignalHub.
class QtBindHub {
public:
    static QtBindHub& instance();
    void init();
    void shutdown();

    // Wrap `obj` as a bindable property map (idempotent per ObjectInstance); returns
    // the wrapper (a QObject) for setContextProperty, or nullptr if obj isn't an object.
    RoxalPropertyMap* wrap(const Value& obj);
    // The wrapper for an already-exposed object, or nullptr (for qt.notify).
    RoxalPropertyMap* lookup(ObjectInstance* obj);

private:
    QtBindHub();
    ~QtBindHub();
    QtBindHub(const QtBindHub&) = delete;
    QtBindHub& operator=(const QtBindHub&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
