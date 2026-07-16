#ifdef ROXAL_ENABLE_QT

// Roxal headers first — they use identifiers (signals/slots/emit) that Qt defines
// as macros; including <QObject> before them breaks parsing.
#include "VM.h"
#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "GCRoots.h"
#include "ModuleQtConvert.h"
#include "ModuleQt.h"
#include "QtSignalHub.h"
// Qt-pulling headers last.
#include "ObjQtObject.h"
#include "QtSignalRelay.h"

#include <QObject>
#include <QMetaMethod>
#include <QByteArray>
#include <QVariant>

#include <string>
#include <unordered_map>
#include <vector>

using namespace roxal;

namespace {

struct CallbackEntry {
    int id;
    Value closure;
};

// One per (sender, signal): the relay + its Roxal delivery targets.
struct SignalEntry {
    std::unique_ptr<QtSignalRelay> relay;
    Value eventType;                       // nil until first `when … occurs` use
    std::vector<CallbackEntry> callbacks;  // slot-style handlers
};

std::string makeKey(const QObject* sender, const QMetaMethod& signal)
{
    return std::to_string(reinterpret_cast<uintptr_t>(sender)) + ":" +
           std::to_string(signal.methodIndex());
}

} // namespace

// ============================================================
// Impl (the entries member is the GC root -- TracedMember, v2 phase B)
// ============================================================

struct QtSignalHub::Impl {
    // Keep callback closures + event types of live-sender connections alive.
    static void traceEntries(ValueVisitor& visitor,
                             const std::unordered_map<std::string, SignalEntry>& entries) {
        for (auto& kv : entries) {
            const SignalEntry& e = kv.second;
            if (!e.relay || !e.relay->signalSender())
                continue; // dead sender → let its closures/event type be collected
            if (e.eventType.isNonNil())
                visitor.visit(e.eventType);
            for (auto& cb : e.callbacks)
                visitor.visit(cb.closure);
        }
    }
    TracedMember<std::unordered_map<std::string, SignalEntry>> entries { &Impl::traceEntries };
    int nextConnId { 1 };

    SignalEntry& ensureEntry(QObject* sender, const QMetaMethod& signal) {
        std::string key = makeKey(sender, signal);
        auto it = entries->find(key);
        if (it != entries->end())
            return it->second;
        SignalEntry& e = (*entries)[key];
        e.relay = std::make_unique<QtSignalRelay>(
            sender, signal,
            [this, key](const QVariantList& vargs) { dispatch(key, vargs); });
        return e;
    }

    void dispatch(const std::string& key, const QVariantList& vargs) {
        auto it = entries->find(key);
        if (it == entries->end())
            return;
        SignalEntry& e = it->second;

        // Convert the signal args once.
        std::vector<Value> args;
        args.reserve(static_cast<size_t>(vargs.size()));
        for (const QVariant& v : vargs)
            args.push_back(fromQVariant(v));

        // Snapshot targets BEFORE invoking (handlers may add/remove connections).
        Value eventType = e.eventType;
        QObject* sender = e.relay ? e.relay->signalSender() : nullptr;
        const QMetaMethod signal = e.relay ? e.relay->signal() : QMetaMethod();
        std::vector<Value> cbs;
        cbs.reserve(e.callbacks.size());
        for (auto& cb : e.callbacks)
            cbs.push_back(cb.closure);

        // (b) Event style: deferred — schedule a `when … occurs` event.
        if (eventType.isNonNil()) {
            Value instance = buildEventInstance(eventType, signal, args, sender);
            roxal::scheduleEventHandlers(eventType.weakRef(), asEventType(eventType),
                                         instance, TimePoint::currentTime());
        }
        // (a) Callback style: synchronous. The signal fires while the VM is parked in
        // run() (threadSleep set). invokeClosure() now un-parks for the duration of each
        // nested handler and restores the park on return (VM's ParkedInvokeScope), so the
        // old manual save/clear/restore dance is no longer needed here. The one Qt-specific
        // concern remains: a handler may request quit (qt.quit() → requestQuit() un-parks),
        // but ParkedInvokeScope re-parks on return — so after running the handlers we must
        // explicitly leave the thread un-parked when a quit is pending, else run() can't exit.
        if (!cbs.empty()) {
            for (Value& c : cbs) {
                if (isClosure(c))
                    VM::instance().invokeClosure(asClosure(c), args);
            }
            if (ModuleQt::quitRequested()) {
                if (Thread* t = VM::thread.get())
                    t->threadSleep = false;
            }
        }
    }

    static Value buildEventInstance(const Value& eventType, const QMetaMethod& signal,
                                    const std::vector<Value>& args, QObject* sender) {
        std::unordered_map<int32_t, Value> payload;
        const QList<QByteArray> pnames = signal.parameterNames();
        for (int i = 0; i < static_cast<int>(args.size()); ++i) {
            std::string pname = (i < pnames.size() && !pnames[i].isEmpty())
                                    ? std::string(pnames[i].constData())
                                    : ("arg" + std::to_string(i));
            payload[toUnicodeString(pname).hashCode()] = args[static_cast<size_t>(i)];
        }
        payload[toUnicodeString("target").hashCode()] = qtObjectValue(sender);
        return Value::eventInstanceVal(eventType, std::move(payload));
    }
};

// ============================================================
// QtSignalHub
// ============================================================

QtSignalHub::QtSignalHub() : impl_(std::make_unique<Impl>()) {}
QtSignalHub::~QtSignalHub() { shutdown(); }

QtSignalHub& QtSignalHub::instance()
{
    static QtSignalHub hub;
    return hub;
}

void QtSignalHub::init()
{
}

void QtSignalHub::shutdown()
{
    impl_->entries->clear(); // destroys relays (disconnects), drops Value refs
}

int QtSignalHub::connectCallback(QObject* sender, const QMetaMethod& signal, const Value& handler)
{
    SignalEntry& e = impl_->ensureEntry(sender, signal);
    int id = impl_->nextConnId++;
    e.callbacks.push_back(CallbackEntry{ id, handler });
    return id;
}

Value QtSignalHub::eventTypeFor(QObject* sender, const QMetaMethod& signal)
{
    SignalEntry& e = impl_->ensureEntry(sender, signal);
    if (e.eventType.isNonNil())
        return e.eventType;

    // Create a stable event type for this signal; declare payload props from the
    // signal's parameter names (the instance carries the values at fire time).
    Value et = Value::objVal(newEventTypeObj(toUnicodeString(signal.name().constData())));
    ObjEventType* ev = asEventType(et);
    const QList<QByteArray> pnames = signal.parameterNames();
    for (int i = 0; i < signal.parameterCount(); ++i) {
        std::string pname = (i < pnames.size() && !pnames[i].isEmpty())
                                ? std::string(pnames[i].constData())
                                : ("arg" + std::to_string(i));
        icu::UnicodeString uname = toUnicodeString(pname);
        ev->payloadProperties.push_back({ uname, Value::nilVal(), Value::nilVal() });
        ev->propertyLookup[uname.hashCode()] = ev->payloadProperties.size() - 1;
    }
    e.eventType = et;
    return et;
}

bool QtSignalHub::disconnectId(int connId)
{
    for (auto& kv : *impl_->entries) {
        auto& cbs = kv.second.callbacks;
        for (auto it = cbs.begin(); it != cbs.end(); ++it) {
            if (it->id == connId) {
                cbs.erase(it);
                return true;
            }
        }
    }
    return false;
}

#endif // ROXAL_ENABLE_QT
