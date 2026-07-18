#pragma once

#ifdef ROXAL_ENABLE_QT

#include <QQuickItem>
#include <QImage>
#include <QVariant>

namespace roxal {

// FrameView: a QML item that displays CPU-generated pixel frames — uint8
// [H, W, C] tensors bridged to QImage by ModuleQtConvert (toQImage). Presenting
// a frame (the `frame` property, or the equivalent present() invokable) swaps
// the image and schedules a repaint; updatePaintNode uploads it as a
// scene-graph texture sized to the SOURCE frame, so scaling to the item rect
// happens in the scene graph — GPU when available, QPainter under
// QT_QUICK_BACKEND=software. No GPU required. `smooth: false` selects
// nearest-neighbor filtering (crisp low-res pixels, e.g. retro framebuffers);
// `smooth: true` (the Qt default) bilinear (camera feeds).
//
// Registered as `FrameView` in `import Roxal` (see ModuleQt::onModuleLoaded).
//
// moc-free like the rest of the module: rendering is a virtual-only override
// (updatePaintNode), and the item's own meta-API — the frame/framesPresented
// properties, present(QVariant) and the frameChanged() notify signal — is a
// QMetaObjectBuilder-built metaobject exposed through the metaObject()/
// qt_metacall() virtuals (the RoxalMethodBridge technique; this is what moc
// would have generated). One consequence: QML resolves these members at
// runtime (JS access, Connections), not in declarative bindings — which is
// fine, frames are presented from Roxal at runtime.
class QtFrameView : public QQuickItem {
public:
    explicit QtFrameView(QQuickItem* parent = nullptr);

    // Hand-rolled meta-object plumbing (no Q_OBJECT). All instances share one
    // process-static metaobject: superclass QQuickItem, signal frameChanged(),
    // method present(QVariant), properties frame / framesPresented.
    const QMetaObject* metaObject() const override;
    void* qt_metacast(const char* clname) override;
    int qt_metacall(QMetaObject::Call c, int id, void** a) override;

    // The current frame as a QImage variant (invalid variant when empty), so a
    // Roxal read (`view.frame`) converts straight back to a uint8 tensor.
    QVariant frame() const;
    // Accepts a QImage/QPixmap variant (a tensor arrives here already converted
    // by toQVariant), or nil/invalid to clear. Anything else warns and is ignored.
    void setFrame(const QVariant& frame);
    void present(const QVariant& frame) { setFrame(frame); }

    // Frames presented so far (nil/clears don't count) — diagnostics and
    // headless test assertions when no grab path is available.
    int framesPresented() const { return m_framesPresented; }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    void emitFrameChanged();

    QImage m_image;              // implicitly shared; render thread reads via updatePaintNode only
    int m_framesPresented = 0;
    bool m_textureDirty = false;
};

} // namespace roxal

#endif // ROXAL_ENABLE_QT
