#ifdef ROXAL_ENABLE_QT

#include "ObjQtObject.h"
#include "ModuleQtConvert.h"
#include "QtSignalHub.h"
#include "ArgsView.h"

#include <QObject>
#include <QMetaObject>
#include <QMetaMethod>
#include <QMetaType>
#include <QByteArray>
#include <QString>
#include <QVariant>

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

using namespace roxal;

// ============================================================
// Null-guard + helpers
// ============================================================

QObject* ObjQtObject::deref(const char* op) const
{
    QObject* o = qobj.data();
    if (!o)
        throw std::runtime_error(std::string("qt: ") + op + " on a destroyed Qt object");
    return o;
}

static QByteArray nameToUtf8(const icu::UnicodeString& name)
{
    return QByteArray::fromStdString(toUTF8StdString(name));
}

// ============================================================
// Dynamic property get / set
// ============================================================

static bool hasMetaMethod(QObject* o, const QByteArray& nm)
{
    const QMetaObject* mo = o->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        QMetaMethod mm = mo->method(i);
        if (mm.methodType() == QMetaMethod::Signal || mm.methodType() == QMetaMethod::Constructor)
            continue;
        if (mm.name() == nm)
            return true;
    }
    return false;
}

// Find a Qt signal meta-method by name (first match; invalid if none). Exported.
QMetaMethod roxal::findQtSignal(QObject* o, const char* name)
{
    const QMetaObject* mo = o->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        QMetaMethod mm = mo->method(i);
        if (mm.methodType() == QMetaMethod::Signal && mm.name() == name)
            return mm;
    }
    return QMetaMethod();
}

// "onClicked" -> "clicked"; "" if not an on<Capital> handler name.
static std::string deriveSignalFromHandlerName(const std::string& m)
{
    if (m.size() > 2 && m[0] == 'o' && m[1] == 'n' &&
        std::isupper(static_cast<unsigned char>(m[2]))) {
        std::string s = m.substr(2);
        s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
        return s;
    }
    return "";
}

// A method name resolves to a bound callable (a func-typed member); calling it
// re-dispatches through tryInvokeDynamicMethod with the actual args at call time.
static Value makeBoundMethod(const Value& self, std::string mname)
{
    return Value::boundNativeVal(self, [mname](VM&, ArgsView a) -> Value {
        Value result;
        const Value* callArgs = (a.size() > 1) ? (a.data + 1) : nullptr;
        int n = (a.size() > 0) ? static_cast<int>(a.size()) - 1 : 0;
        asQtObject(a[0])->tryInvokeDynamicMethod(toUnicodeString(mname), callArgs, n, result);
        return result;
    });
}

bool ObjQtObject::tryGetDynamicProperty(const Value& self, const icu::UnicodeString& name, Value& out)
{
    const std::string m = toUTF8StdString(name);
    QObject* o = qobj.data();

    if (!o) {
        // Destroyed handle: only valid() is answerable; anything else errors.
        if (m == "valid") { out = makeBoundMethod(self, m); return true; }
        throw std::runtime_error("qt: '" + m + "' on a destroyed Qt object");
    }

    QByteArray nm = QByteArray::fromStdString(m);
    // 1. A genuine Qt property → its value.
    if (o->metaObject()->indexOfProperty(nm.constData()) >= 0 ||
        o->dynamicPropertyNames().contains(nm)) {
        out = fromQVariant(o->property(nm.constData()));
        return true;
    }
    // 2. A bare Qt signal name → its event type (for `when item.sig occurs`).
    {
        QMetaMethod sig = findQtSignal(o, nm.constData());
        if (sig.isValid()) {
            out = QtSignalHub::instance().eventTypeFor(o, sig);
            return true;
        }
    }
    // 3. on<Signal>(handler) → a bound callable that connects a slot-style callback
    //    (mirrors QML's onClicked handler). Tried only if it resolves to a signal.
    {
        std::string sigName = deriveSignalFromHandlerName(m);
        if (!sigName.empty() && findQtSignal(o, sigName.c_str()).isValid()) {
            out = Value::boundNativeVal(self, [sigName](VM&, ArgsView a) -> Value {
                if (a.size() < 2 || !isClosure(a[1]))
                    throw std::runtime_error("qt: on" + sigName + "(handler) expects a callable");
                ObjQtObject* recv = asQtObject(a[0]);
                QObject* obj = recv->deref("signal connect");
                QMetaMethod s = findQtSignal(obj, sigName.c_str());
                if (!s.isValid())
                    throw std::runtime_error("qt: no signal '" + sigName + "'");
                int id = QtSignalHub::instance().connectCallback(obj, s, a[1]);
                return Value::intVal(id);
            });
            return true;
        }
    }
    // 4. A Qt meta-method, or 5. an item intrinsic → a bound callable (Roxal
    //    methods are func-typed members, looked up then called).
    if (hasMetaMethod(o, nm) || m == "find" || m == "find_all" || m == "valid") {
        out = makeBoundMethod(self, m);
        return true;
    }
    throw std::runtime_error("qt: no property or method '" + m + "' on " +
                             o->metaObject()->className());
}

bool ObjQtObject::trySetDynamicProperty(const icu::UnicodeString& name, const Value& value)
{
    QObject* o = deref("property write");
    QByteArray nm = nameToUtf8(name);
    // Require a declared meta-property, so a typo errors instead of QObject
    // silently creating a stray dynamic property.
    if (o->metaObject()->indexOfProperty(nm.constData()) < 0) {
        throw std::runtime_error("qt: no settable property '" + std::string(nm.constData()) +
                                 "' on " + o->metaObject()->className());
    }
    if (!o->setProperty(nm.constData(), toQVariant(value))) {
        throw std::runtime_error("qt: failed to set property '" + std::string(nm.constData()) +
                                 "' on " + o->metaObject()->className());
    }
    return true;
}

// Invoke a Qt meta-method (Q_INVOKABLE or QML JS function) by index, converting
// args to/from QVariant. Returns the result via `out`.
static void invokeMetaMethod(QObject* o, const QMetaMethod& mm, const Value* args, int argCount,
                             Value& out)
{
    // Convert each arg to a QVariant matching the parameter's declared type.
    std::vector<QVariant> argStore;
    argStore.reserve(static_cast<size_t>(argCount));
    for (int i = 0; i < argCount; ++i) {
        QVariant a = toQVariant(args[i]);
        QMetaType pt = mm.parameterMetaType(i);
        if (pt.isValid() && pt.id() != QMetaType::QVariant && a.metaType() != pt) {
            QVariant conv = a;
            if (conv.convert(pt))
                a = std::move(conv);
        }
        argStore.push_back(std::move(a));
    }

    // QGenericArgument data points at the QVariant itself for QVariant params,
    // else at the contained value. Unused slots stay default (null).
    QGenericArgument g[10];
    for (int i = 0; i < argCount && i < 10; ++i) {
        QMetaType pt = mm.parameterMetaType(i);
        if (pt.id() == QMetaType::QVariant)
            g[i] = QGenericArgument("QVariant", &argStore[static_cast<size_t>(i)]);
        else
            g[i] = QGenericArgument(pt.name(), argStore[static_cast<size_t>(i)].constData());
    }

    QMetaType retType = mm.returnMetaType();
    bool ok;
    if (!retType.isValid() || retType.id() == QMetaType::Void) {
        ok = mm.invoke(o, Qt::DirectConnection, QGenericReturnArgument(),
                       g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
        out = Value::nilVal();
    } else if (retType.id() == QMetaType::QVariant) {
        QVariant ret;
        ok = mm.invoke(o, Qt::DirectConnection, QGenericReturnArgument("QVariant", &ret),
                       g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
        out = fromQVariant(ret);
    } else {
        QVariant ret(retType);
        ok = mm.invoke(o, Qt::DirectConnection, QGenericReturnArgument(retType.name(), ret.data()),
                       g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
        out = fromQVariant(ret);
    }
    if (!ok)
        throw std::runtime_error(std::string("qt: failed to invoke method '") +
                                 mm.name().constData() + "' on " + o->metaObject()->className());
}

bool ObjQtObject::tryInvokeDynamicMethod(const icu::UnicodeString& name, const Value* args,
                                         int argCount, Value& out)
{
    const std::string m = toUTF8StdString(name);

    // valid() is the one operation that works on a destroyed handle.
    if (m == "valid" && argCount == 0) {
        out = Value::boolVal(qobj.data() != nullptr);
        return true;
    }

    QObject* o = qobj.data();
    if (!o)
        throw std::runtime_error("qt: method '" + m + "' on a destroyed Qt object");

    if (argCount > 10)
        throw std::runtime_error("qt: method '" + m + "' called with too many arguments (max 10)");

    // A real Qt meta-method (Q_INVOKABLE / QML function / slot) wins over intrinsics.
    const QMetaObject* mo = o->metaObject();
    const QByteArray want = QByteArray::fromStdString(m);
    for (int i = 0; i < mo->methodCount(); ++i) {
        QMetaMethod mm = mo->method(i);
        if (mm.methodType() == QMetaMethod::Signal || mm.methodType() == QMetaMethod::Constructor)
            continue;
        if (mm.name() == want && mm.parameterCount() == argCount) {
            invokeMetaMethod(o, mm, args, argCount, out);
            return true;
        }
    }

    // Item intrinsics (after Qt-method miss).
    if (m == "find" && argCount == 1) {
        if (!isString(args[0]))
            throw std::runtime_error("qt: find(name) expects a string objectName");
        QString nm = QString::fromStdString(toUTF8StdString(asStringObj(args[0])->s));
        out = qtObjectValue(o->findChild<QObject*>(nm));
        return true;
    }
    if (m == "find_all" && argCount == 1) {
        if (!isString(args[0]))
            throw std::runtime_error("qt: find_all(name) expects a string objectName");
        QString nm = QString::fromStdString(toUTF8StdString(asStringObj(args[0])->s));
        Value list = Value::objVal(newListObj());
        ObjList* l = asList(list);
        const QList<QObject*> found = o->findChildren<QObject*>(nm);
        for (QObject* c : found)
            l->append(qtObjectValue(c));
        out = list;
        return true;
    }

    throw std::runtime_error("qt: no method '" + m + "' on " + mo->className());
}

// ============================================================
// Obj overrides: share-on-clone, no serialization
// ============================================================

unique_ptr<Obj, UnreleasedObj> ObjQtObject::clone(roxal::ptr<CloneContext> ctx) const
{
    (void)ctx; // a Qt handle is an opaque reference; cloning shares the same handle
    return unique_ptr<Obj, UnreleasedObj>(const_cast<ObjQtObject*>(this));
}

void ObjQtObject::write(std::ostream&, roxal::ptr<SerializationContext>) const
{
    throw std::runtime_error("qt: Qt object handles cannot be serialized");
}

void ObjQtObject::read(std::istream&, roxal::ptr<SerializationContext>)
{
    throw std::runtime_error("qt: Qt object handles cannot be deserialized");
}

// ============================================================
// Construction helpers
// ============================================================

unique_ptr<ObjQtObject, UnreleasedObj> roxal::newQtObjectObj(QObject* obj)
{
    #ifdef DEBUG_BUILD
    return newObj<ObjQtObject>(__func__, __FILE__, __LINE__, obj);
    #else
    return newObj<ObjQtObject>(obj);
    #endif
}

Value roxal::qtObjectValue(QObject* obj)
{
    if (!obj)
        return Value::nilVal();
    return Value::objVal(newQtObjectObj(obj));
}

#endif // ROXAL_ENABLE_QT
