#pragma once

#ifdef ROXAL_ENABLE_QT

#include <QObject>
#include <QPointer>
#include <QMetaMethod>
#include <QVariantList>

#include <functional>

namespace roxal {

// A moc-free QObject that connects to an arbitrary, runtime-named Qt signal and
// delivers the signal's arguments (as a QVariantList) to a handler. It is the
// canonical QSignalSpy mechanism: it overrides qt_metacall() and connects the
// signal to an out-of-band method slot index it handles itself — so no Q_OBJECT
// / moc / AUTOMOC is required. Qt-only (no Roxal types); the owner converts the
// QVariantList and dispatches into the VM.
class QtSignalRelay : public QObject {
public:
    using Handler = std::function<void(const QVariantList& args)>;

    // Connect `sender`'s `signal` (a Signal QMetaMethod) to `handler`. Lifetime is
    // owned by the caller; the connection auto-drops if `sender` is destroyed.
    QtSignalRelay(QObject* sender, const QMetaMethod& signal, Handler handler);
    ~QtSignalRelay() override;

    bool isConnected() const { return static_cast<bool>(connection_); }
    QObject* signalSender() const { return sender_.data(); }
    const QMetaMethod& signal() const { return signal_; }

    int qt_metacall(QMetaObject::Call call, int id, void** args) override;

private:
    QPointer<QObject> sender_;
    QMetaMethod signal_;
    Handler handler_;
    QMetaObject::Connection connection_;
    int slotIndex_; // method index the signal is connected to (handled in qt_metacall)
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
