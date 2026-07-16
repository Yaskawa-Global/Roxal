#pragma once

#ifdef ROXAL_ENABLE_QT

#include "Value.h"

#include <memory>

// Qt types forward-declared (full headers stay in the .cpp); this header must NOT
// pull Qt in, since it's reachable from module TUs but we keep the surface minimal.
class QObject;
class QMetaMethod;

namespace roxal {

// Owns Qt signal → Roxal connections. For each (sender QObject, signal) it holds a
// moc-free QtSignalRelay plus the Roxal delivery targets: a list of callback
// closures (slot-style, invoked synchronously) and an optional per-signal event
// type (for `when … occurs`). Its entries map is a typed GC root (TracedMember) so those closures and
// event types stay alive while connected. Singleton; lifetime bracketed by ModuleQt
// (init() at module load, shutdown() at script-complete teardown).
class QtSignalHub {
public:
    static QtSignalHub& instance();

    void init();      // register the GC root provider (idempotent)
    void shutdown();  // disconnect+clear everything, unregister the root provider

    // Connect `sender`.`signal` to a Roxal callback closure; returns an int
    // connection id (for qt.disconnect). The callback fires synchronously with the
    // signal's args when the signal emits.
    int connectCallback(QObject* sender, const QMetaMethod& signal, const Value& handler);

    // The stable per-(sender,signal) event type used for `when … occurs`. Created
    // lazily (payload props from the signal's parameter names; `target` = the item).
    Value eventTypeFor(QObject* sender, const QMetaMethod& signal);

    // Remove a callback connection by id. Returns true if one was removed.
    bool disconnectId(int connId);

private:
    QtSignalHub();
    ~QtSignalHub();
    QtSignalHub(const QtSignalHub&) = delete;
    QtSignalHub& operator=(const QtSignalHub&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
