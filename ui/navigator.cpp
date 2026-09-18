// Mari Paint — 내비게이터 구현 (ui/navigator.hpp)
#include "navigator.hpp"

#include "canvas_widget.hpp"

#include <QMouseEvent>
#include <QPainter>

namespace mari::ui {

Navigator::Navigator(CanvasWidget* canvas, QWidget* parent) : QWidget(parent), canvas_(canvas) {
    setMinimumHeight(120);
    connect(canvas_, &CanvasWidget::viewChanged, this, qOverload<>(&QWidget::update));
}

void Navigator::refreshImage() {
    const QImage& backing = canvas_->backingImage();
    if (backing.isNull()) {
        thumb_ = QImage();
    } else {
        // 축소본은 논리 크기의 2배로 만들어 150% 에서도 흐리지 않게.
        thumb_ = backing.scaled(size() * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    update();
}

QRectF Navigator::imageRect() const {
    if (thumb_.isNull()) return QRectF();
    const QSizeF s = QSizeF(thumb_.size()).scaled(QSizeF(size()) - QSizeF(8, 8), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - s.width()) / 2.0, (height() - s.height()) / 2.0), s);
}

void Navigator::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x23, 0x23, 0x23));
    const QRectF ir = imageRect();
    if (ir.isEmpty()) return;
    // 체커 + 축소본
    p.fillRect(ir, QColor(0xc8, 0xc8, 0xc8));
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(ir, thumb_);
    // 보이는 영역: 캔버스 좌표 → 축소본 좌표
    const QImage& backing = canvas_->backingImage();
    if (backing.isNull()) return;
    const QRectF vis = canvas_->visibleCanvasRect();
    const qreal sx = ir.width() / backing.width();
    const qreal sy = ir.height() / backing.height();
    const QRectF vr(ir.left() + vis.left() * sx, ir.top() + vis.top() * sy, vis.width() * sx, vis.height() * sy);
    p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(vr.intersected(ir.adjusted(-1, -1, 1, 1)));
}

void Navigator::moveViewTo(const QPointF& widgetPos) {
    const QRectF ir = imageRect();
    const QImage& backing = canvas_->backingImage();
    if (ir.isEmpty() || backing.isNull()) return;
    const qreal cx = (widgetPos.x() - ir.left()) / ir.width() * backing.width();
    const qreal cy = (widgetPos.y() - ir.top()) / ir.height() * backing.height();
    canvas_->centerOn(QPointF(cx, cy));
}

void Navigator::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) moveViewTo(e->position());
}

void Navigator::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) moveViewTo(e->position());
}

} // namespace mari::ui
