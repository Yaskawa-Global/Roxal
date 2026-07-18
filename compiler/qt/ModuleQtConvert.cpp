#ifdef ROXAL_ENABLE_QT

#include "ModuleQtConvert.h"
#include "ObjQtObject.h"
#include "Object.h"

#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QMetaType>
#include <QObject>
#include <QJSValue>
#include <QJSValueIterator>
#include <QJSEngine>
#include <QThread>
#include <QCoreApplication>
#include <QImage>
#include <QPixmap>

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using namespace roxal;

void roxal::ensureQtUiThread(const char* op)
{
    QCoreApplication* app = QCoreApplication::instance();
    // app->thread() is the thread the QGuiApplication was created on (the VM's main
    // thread, set up at module load). Any other thread (an actor, the dataflow engine)
    // touching Qt is a bug — fail fast rather than corrupt Qt's single-threaded state.
    if (app && QThread::currentThread() != app->thread())
        throw std::runtime_error(
            std::string("qt: '") + op + "' may only be used on the main (UI) thread — Qt "
            "objects are not accessible from actors. Send the actor's result back to the "
            "main thread (a message / event) and update the UI from there.");
}

// ============================================================
// tensor <-> QImage
// ============================================================

QImage roxal::toQImage(const ObjTensor* t)
{
    if (t->dtype() != TensorDType::UInt8 || t->rank() != 3)
        throw std::runtime_error(
            "qt: an image tensor must be uint8 with shape [H, W, C] — got dtype " +
            to_string(t->dtype()) + ", rank " + std::to_string(t->rank()) +
            " (use astype('uint8', scale=...) / to_uint8() first)");

    const std::vector<int64_t>& shape = t->shape();
    const int64_t h = shape[0], w = shape[1], c = shape[2];
    if (c != 1 && c != 3 && c != 4)
        throw std::runtime_error(
            "qt: an image tensor must have 1 (grayscale), 3 (RGB) or 4 (RGBA) channels — got " +
            std::to_string(c));
    if (h <= 0 || w <= 0 || h > std::numeric_limits<int>::max() || w > std::numeric_limits<int>::max())
        throw std::runtime_error("qt: image tensor has invalid dimensions " +
                                 std::to_string(h) + "x" + std::to_string(w));

#ifdef ROXAL_ENABLE_ONNX
    t->ensureCpu();
#endif

    const QImage::Format fmt = (c == 1) ? QImage::Format_Grayscale8
                             : (c == 3) ? QImage::Format_RGB888
                                        : QImage::Format_RGBA8888;
    QImage img(static_cast<int>(w), static_cast<int>(h), fmt);
    if (img.isNull())
        throw std::runtime_error("qt: failed to allocate a " + std::to_string(w) + "x" +
                                 std::to_string(h) + " image");

    const auto* src = static_cast<const uint8_t*>(t->rawData());
    const size_t rowBytes = static_cast<size_t>(w) * static_cast<size_t>(c);
    for (int64_t y = 0; y < h; ++y)
        std::memcpy(img.scanLine(static_cast<int>(y)), src + static_cast<size_t>(y) * rowBytes,
                    rowBytes);
    return img;
}

Value roxal::fromQImage(const QImage& image)
{
    if (image.isNull())
        return Value::nilVal();

    // Normalize to one of the three formats the tensor side speaks. Grayscale
    // stays 1-channel; anything with alpha becomes RGBA; everything else RGB.
    QImage img;
    int64_t c;
    if (image.format() == QImage::Format_Grayscale8) {
        img = image;
        c = 1;
    } else if (image.hasAlphaChannel()) {
        img = image.convertToFormat(QImage::Format_RGBA8888);
        c = 4;
    } else {
        img = image.convertToFormat(QImage::Format_RGB888);
        c = 3;
    }

    const int64_t h = img.height(), w = img.width();
    const size_t rowBytes = static_cast<size_t>(w) * static_cast<size_t>(c);
    std::vector<uint8_t> bytes(static_cast<size_t>(h) * rowBytes);
    for (int64_t y = 0; y < h; ++y)
        std::memcpy(bytes.data() + static_cast<size_t>(y) * rowBytes,
                    img.constScanLine(static_cast<int>(y)), rowBytes);

    return Value::objVal(newTensorObj({h, w, c}, TensorDType::UInt8, std::move(bytes)));
}

// ============================================================
// QVariant <-> Value
// ============================================================

Value roxal::fromQVariant(const QVariant& v)
{
    if (!v.isValid() || v.isNull())
        return Value::nilVal();

    switch (v.typeId()) {
        case QMetaType::Bool:
            return Value::boolVal(v.toBool());
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            return Value::intVal(static_cast<int64_t>(v.toLongLong()));
        case QMetaType::Double:
        case QMetaType::Float:
            return Value::realVal(v.toDouble());
        case QMetaType::QString:
            return Value::stringVal(toUnicodeString(v.toString().toStdString()));
        case QMetaType::QVariantList: {
            Value list = Value::objVal(newListObj());
            ObjList* l = asList(list);
            const QVariantList items = v.toList();
            for (const QVariant& e : items)
                l->append(fromQVariant(e));
            return list;
        }
        case QMetaType::QVariantMap: {
            Value d = Value::objVal(newDictObj());
            ObjDict* dict = asDict(d);
            const QVariantMap m = v.toMap();
            for (auto it = m.constBegin(); it != m.constEnd(); ++it)
                dict->store(Value::stringVal(toUnicodeString(it.key().toStdString())),
                            fromQVariant(it.value()));
            return d;
        }
        case QMetaType::QImage:
            return fromQImage(v.value<QImage>());
        case QMetaType::QPixmap:
            return fromQImage(v.value<QPixmap>().toImage());
        default:
            break;
    }

    // A QML function returning a JS object/array arrives as a QVariant wrapping a
    // QJSValue — unwrap it through the JS-value converter.
    if (v.metaType() == QMetaType::fromType<QJSValue>())
        return fromQJSValue(v.value<QJSValue>(), nullptr);

    // QObject* (incl. QML items) → a Roxal Item handle (recursive/nested access).
    if (v.canConvert<QObject*>()) {
        QObject* o = v.value<QObject*>();
        if (o)
            return qtObjectValue(o);
    }
    // Last resort: a string representation if one is available; else nil.
    if (v.canConvert<QString>())
        return Value::stringVal(toUnicodeString(v.toString().toStdString()));
    return Value::nilVal();
}

QVariant roxal::toQVariant(const Value& v)
{
    if (v.isNil())  return QVariant();
    if (v.isBool()) return QVariant(v.asBool());
    if (v.isInt())  return QVariant(static_cast<qlonglong>(v.asInt()));
    if (v.isReal()) return QVariant(v.asReal());
    if (isString(v))
        return QVariant(QString::fromStdString(toUTF8StdString(asStringObj(v)->s)));
    if (isList(v)) {
        QVariantList out;
        ObjList* l = asList(v);
        for (int32_t i = 0; i < l->length(); ++i)
            out.append(toQVariant(l->getElement(static_cast<size_t>(i))));
        return out;
    }
    if (isDict(v)) {
        QVariantMap out;
        ObjDict* d = asDict(v);
        for (const auto& kv : d->items()) {
            if (!isString(kv.first))
                throw std::runtime_error("qt: dict keys must be strings to convert to a Qt map");
            out.insert(QString::fromStdString(toUTF8StdString(asStringObj(kv.first)->s)),
                       toQVariant(kv.second));
        }
        return out;
    }
    if (isVector(v)) {
        QVariantList out;
        ObjVector* vec = asVector(v);
        const Eigen::VectorXd& e = vec->vec();
        for (int32_t i = 0; i < vec->length(); ++i)
            out.append(e(i));
        return out;
    }
    if (isQtObject(v)) {
        // A live Item handle round-trips back to its QObject* (nil if destroyed).
        QObject* o = asQtObject(v)->qobj.data();
        return QVariant::fromValue(o);
    }
    if (isTensor(v))
        return QVariant::fromValue(toQImage(asTensor(v)));
    throw std::runtime_error("qt: cannot convert Roxal value of type '" +
                             to_string(v.type()) + "' to a Qt value");
}

// ============================================================
// QJSValue <-> Value
// ============================================================

Value roxal::fromQJSValue(const QJSValue& v, QJSEngine* engine)
{
    if (v.isNull() || v.isUndefined())
        return Value::nilVal();
    if (v.isBool())
        return Value::boolVal(v.toBool());
    if (v.isNumber()) {
        double d = v.toNumber();
        if (std::floor(d) == d && std::abs(d) < 9.0e15)
            return Value::intVal(static_cast<int64_t>(d));
        return Value::realVal(d);
    }
    if (v.isString())
        return Value::stringVal(toUnicodeString(v.toString().toStdString()));
    if (v.isQObject())
        return qtObjectValue(v.toQObject());
    // Opaque QVariant payloads (QImage, dates, ...) report isObject() too, so the
    // generic object walk below would flatten them into an empty dict. Probe the
    // wrapped metatype: RetainJSObjects keeps genuine JS objects/arrays as QJSValue
    // (→ fall through), while real variant payloads surface as their own type.
    if (v.isObject() && !v.isArray()) {
        const QVariant qv = v.toVariant(QJSValue::RetainJSObjects);
        if (qv.isValid() && qv.metaType() != QMetaType::fromType<QJSValue>())
            return fromQVariant(qv);
    }
    if (v.isArray()) {
        const int len = v.property("length").toInt();
        Value list = Value::objVal(newListObj());
        ObjList* l = asList(list);
        for (int i = 0; i < len; ++i)
            l->append(fromQJSValue(v.property(static_cast<quint32>(i)), engine));
        return list;
    }
    if (v.isObject()) {
        Value d = Value::objVal(newDictObj());
        ObjDict* dict = asDict(d);
        QJSValueIterator it(v);
        while (it.hasNext()) {
            it.next();
            dict->store(Value::stringVal(toUnicodeString(it.name().toStdString())),
                        fromQJSValue(it.value(), engine));
        }
        return d;
    }
    // Anything else (dates, etc.) → go through QVariant.
    return fromQVariant(v.toVariant());
}

QJSValue roxal::toQJSValue(const Value& v, QJSEngine* engine)
{
    if (!engine)
        throw std::runtime_error("qt: toQJSValue requires a QJSEngine");
    if (v.isNil())
        return QJSValue(QJSValue::NullValue);
    if (v.isBool())
        return QJSValue(v.asBool());
    if (v.isInt())
        return QJSValue(static_cast<int>(v.asInt()));
    if (v.isReal())
        return QJSValue(v.asReal());
    if (isString(v))
        return QJSValue(QString::fromStdString(toUTF8StdString(asStringObj(v)->s)));
    if (isList(v) || isVector(v)) {
        // Build a JS array (vector becomes a numeric array).
        QVariant qv = toQVariant(v);
        const QVariantList items = qv.toList();
        QJSValue arr = engine->newArray(static_cast<uint>(items.size()));
        for (int i = 0; i < items.size(); ++i)
            arr.setProperty(static_cast<quint32>(i), engine->toScriptValue(items.at(i)));
        return arr;
    }
    if (isDict(v)) {
        QJSValue obj = engine->newObject();
        ObjDict* d = asDict(v);
        for (const auto& kv : d->items()) {
            if (!isString(kv.first))
                throw std::runtime_error("qt: dict keys must be strings to convert to a JS object");
            obj.setProperty(QString::fromStdString(toUTF8StdString(asStringObj(kv.first)->s)),
                            toQJSValue(kv.second, engine));
        }
        return obj;
    }
    if (isQtObject(v)) {
        QObject* o = asQtObject(v)->qobj.data();
        return o ? engine->newQObject(o) : QJSValue(QJSValue::NullValue);
    }
    // Fallback through QVariant.
    return engine->toScriptValue(toQVariant(v));
}

#endif // ROXAL_ENABLE_QT
