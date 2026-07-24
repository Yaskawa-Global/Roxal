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

    // True while a Roxal callback dispatched by the hub (a qt.connect signal
    // handler or a qt.every timer tick) is executing on this (main) thread. The
    // Qt host-loop busy-pump reads this to preserve run-to-completion: it must
    // not dispatch another Qt slot -- re-entering the VM with a second callback
    // that shares state -- while one is mid-flight. Roxal's model isolates mutable
    // state per actor and forbids preemptive interleaving on a single thread; the
    // 1ms busy-pump would otherwise smuggle exactly that in (a timer tick firing
    // inside a key handler, etc.). An explicit `await` inside a handler is a
    // voluntary yield and still re-enters -- via the idle wait, not this pump.
    static bool inMainThreadCallback();

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
