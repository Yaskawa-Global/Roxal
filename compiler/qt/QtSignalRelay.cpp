#ifdef ROXAL_ENABLE_QT

#include "QtSignalRelay.h"

#include <QMetaType>
#include <QVariant>

using namespace roxal;

QtSignalRelay::QtSignalRelay(QObject* sender, const QMetaMethod& signal, Handler handler)
    : sender_(sender), signal_(signal), handler_(std::move(handler))
{
    // Connect the signal to a slot index one past QObject's own methods; our
    // qt_metacall() override intercepts that index (QSignalSpy convention).
    slotIndex_ = QObject::staticMetaObject.methodCount();
    if (sender)
        connection_ = QMetaObject::connect(sender, signal_.methodIndex(), this, slotIndex_,
                                           Qt::DirectConnection, nullptr);
}

QtSignalRelay::~QtSignalRelay()
{
    if (connection_)
        QObject::disconnect(connection_);
}

int QtSignalRelay::qt_metacall(QMetaObject::Call call, int id, void** args)
{
    id = QObject::qt_metacall(call, id, args);
    if (id < 0)
        return id;
    if (call == QMetaObject::InvokeMetaMethod) {
        if (id == 0) {
            // Our relay slot fired: read each signal argument by its declared type.
            const int pc = signal_.parameterCount();
            QVariantList vargs;
            vargs.reserve(pc);
            for (int i = 0; i < pc; ++i) {
                QMetaType t = signal_.parameterMetaType(i);
                vargs.append(QVariant(t, args[i + 1])); // args[0] = return slot
            }
            if (handler_)
                handler_(vargs);
            return -1;
        }
        --id;
    }
    return id;
}

#endif // ROXAL_ENABLE_QT
