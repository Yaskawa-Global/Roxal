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

} // namespace roxal

#endif // ROXAL_ENABLE_QT
