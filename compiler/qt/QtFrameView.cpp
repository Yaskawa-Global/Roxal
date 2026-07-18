#ifdef ROXAL_ENABLE_QT

#include "QtFrameView.h"

#include <QJSValue>
#include <QPixmap>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QtCore/private/qmetaobjectbuilder_p.h>   // moc-free runtime metaobject

#include <cstring>

using namespace roxal;

// ============================================================
// Meta-object (shared by all instances, built once, lives for the process —
// like a moc-generated staticMetaObject).
//
// Local method order (signals first, as moc lays them out):
//   0  frameChanged()            [signal]
//   1  present(QVariant)
// Local property order:
//   0  frame            QVariant  read/write, notify frameChanged
//   1  framesPresented  int       read-only,  notify frameChanged
// ============================================================

namespace {

constexpr int kLocalMethods    = 2;
constexpr int kLocalProperties = 2;

const QMetaObject* frameViewMeta()
{
    static QMetaObject* meta = [] {
        QMetaObjectBuilder b;
        b.setClassName("QtFrameView");
        b.setSuperClass(&QQuickItem::staticMetaObject);
        QMetaMethodBuilder sig = b.addSignal("frameChanged()");
        b.addMethod("present(QVariant)");
        QMetaPropertyBuilder frameProp = b.addProperty("frame", "QVariant");
        frameProp.setNotifySignal(sig);
        QMetaPropertyBuilder countProp = b.addProperty("framesPresented", "int");
        countProp.setWritable(false);
        countProp.setNotifySignal(sig);
        return b.toMetaObject();
    }();
    return meta;
}

} // namespace

const QMetaObject* QtFrameView::metaObject() const
{
    return frameViewMeta();
}

void* QtFrameView::qt_metacast(const char* clname)
{
    if (clname && std::strcmp(clname, "QtFrameView") == 0)
        return static_cast<void*>(this);
    return QQuickItem::qt_metacast(clname);
}

int QtFrameView::qt_metacall(QMetaObject::Call c, int id, void** a)
{
    id = QQuickItem::qt_metacall(c, id, a);
    if (id < 0)
        return id;

    switch (c) {
    case QMetaObject::InvokeMetaMethod:
        if (id == 0)                       // frameChanged() — emit request (queued/invokeMethod)
            emitFrameChanged();
        else if (id == 1)                  // present(QVariant)
            present(*reinterpret_cast<QVariant*>(a[1]));
        id -= kLocalMethods;
        break;
    case QMetaObject::RegisterMethodArgumentMetaType:
        id -= kLocalMethods;
        break;
    case QMetaObject::ReadProperty:
        if (id == 0)
            *reinterpret_cast<QVariant*>(a[0]) = frame();
        else if (id == 1)
            *reinterpret_cast<int*>(a[0]) = m_framesPresented;
        id -= kLocalProperties;
        break;
    case QMetaObject::WriteProperty:
        if (id == 0)
            setFrame(*reinterpret_cast<QVariant*>(a[0]));
        id -= kLocalProperties;            // framesPresented is read-only: ignore writes
        break;
    case QMetaObject::ResetProperty:
    case QMetaObject::BindableProperty:
    case QMetaObject::RegisterPropertyMetaType:
        id -= kLocalProperties;
        break;
    default:
        break;
    }
    return id;
}

void QtFrameView::emitFrameChanged()
{
    QMetaObject::activate(this, frameViewMeta(), 0, nullptr);
}

// ============================================================
// Frame handling / rendering
// ============================================================

QtFrameView::QtFrameView(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // `smooth` selects the texture filtering (see updatePaintNode) — repaint on change.
    connect(this, &QQuickItem::smoothChanged, this, [this] { update(); });
}

QVariant QtFrameView::frame() const
{
    return m_image.isNull() ? QVariant() : QVariant::fromValue(m_image);
}

void QtFrameView::setFrame(const QVariant& frame)
{
    QVariant v = frame;
    // A QML-side write (`fb.frame = someVar`) can arrive as a QJSValue wrapper.
    if (v.metaType() == QMetaType::fromType<QJSValue>())
        v = v.value<QJSValue>().toVariant();

    if (!v.isValid() || v.isNull()) {
        m_image = QImage();
    } else if (v.typeId() == QMetaType::QImage) {
        m_image = v.value<QImage>();
    } else if (v.typeId() == QMetaType::QPixmap) {
        m_image = v.value<QPixmap>().toImage();
    } else {
        qWarning("FrameView.frame: expected an image (a uint8 [H, W, C] tensor from Roxal, "
                 "or a QImage/QPixmap), got %s", v.typeName());
        return;
    }

    if (!m_image.isNull())
        ++m_framesPresented;
    m_textureDirty = true;
    setImplicitSize(m_image.width(), m_image.height());
    emitFrameChanged();
    update();
}

QSGNode* QtFrameView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (m_image.isNull()) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        m_textureDirty = true;  // fresh node (first frame, or scene-graph invalidation)
    }
    if (m_textureDirty) {
        // setTexture deletes the previous texture (ownsTexture). The texture is
        // source-frame sized; scaling to the item rect happens in the scene graph.
        node->setTexture(window()->createTextureFromImage(m_image));
        m_textureDirty = false;
    }
    node->setFiltering(smooth() ? QSGTexture::Linear : QSGTexture::Nearest);
    node->setRect(boundingRect());
    return node;
}

#endif // ROXAL_ENABLE_QT
