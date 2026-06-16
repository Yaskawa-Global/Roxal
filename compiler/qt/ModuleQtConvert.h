#pragma once

#ifdef ROXAL_ENABLE_QT

#include "Value.h"

// Qt types (forward-declared; full Qt headers stay in the .cpp / call sites).
class QVariant;
class QJSValue;
class QJSEngine;

namespace roxal {

// Shared converters between Qt's dynamic value types and Roxal's Value, used by
// P1 property/method dispatch and reused by P2/P3. Recursive over containers.
//
// Mapping: nil↔invalid, bool, int (Int/LongLong), real (Double/Float), string↔QString,
// list↔QVariantList, dict↔QVariantMap (string keys), vector→QVariantList(doubles),
// QObject*→Item handle (ObjQtObject). Unsupported: read→nil, write→catchable error.
Value fromQVariant(const QVariant& v);
QVariant toQVariant(const Value& v);

// QJSValue (QML inline-JS / JS-engine boundary). `engine` is needed to construct
// JS values (arrays/objects); obtain it from a wrapped object via qjsEngine(obj).
Value fromQJSValue(const QJSValue& v, QJSEngine* engine);
QJSValue toQJSValue(const Value& v, QJSEngine* engine);

// Thread-affinity guard. Qt objects live on the main (UI) thread, but Roxal actors run
// on their own threads — and a Qt handle can reach an actor (a message, a captured
// closure). Every Qt-touching builtin calls this first so off-thread access fails fast
// with a clear error instead of corrupting Qt's single-threaded state. `op` names the
// attempted operation for the message. No-op when on the UI thread (or before the
// QApplication exists). Throws std::runtime_error otherwise.
void ensureQtUiThread(const char* op);

} // namespace roxal

#endif // ROXAL_ENABLE_QT
