// Mari Paint — 색상환 구현 (ui/color_wheel.hpp)
#include "color_wheel.hpp"

#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace mari::ui {

namespace {
constexpr qreal kRingWidthRatio = 0.14; // 링 두께 / 전체 지름
constexpr qreal kPi = 3.14159265358979323846;
} // namespace

ColorWheel::ColorWheel(QWidget* parent) : QWidget(parent) {
    setMinimumSize(120, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rebuildSquare();
}

void ColorWheel::setColor(const QColor& c) {
    const QColor hsv = c.toHsv();
    qreal h = hsv.hsvHueF();
    if (h < 0.0) {
        h = h_; // 무채색이면 휴는 유지한다(검정으로 갔다가 돌아와도 휴가 안 튄다)
    }
    const bool hueChanged = std::abs(h - h_) > 1e-6;
    h_ = h;
    s_ = hsv.hsvSaturationF();
    v_ = hsv.valueF();
    if (hueChanged) {
        rebuildSquare();
    }
    update();
}

QRectF ColorWheel::ringRect() const {
    const qreal d = std::min(width(), height()) - 2.0;
    return QRectF((width() - d) / 2.0, (height() - d) / 2.0, d, d);
}

QRectF ColorWheel::squareRect() const {
    const QRectF r = ringRect();
    const qreal inner = r.width() * (1.0 - 2.0 * kRingWidthRatio);
    const qreal side = inner / std::sqrt(2.0) * 0.96;
    return QRectF(r.center().x() - side / 2.0, r.center().y() - side / 2.0, side, side);
}

void ColorWheel::rebuildSquare() {
    // x = 채도(0→1), y = 명도(1→0). 휴가 바뀔 때만 만든다.
    constexpr int n = 128;
    square_ = QImage(n, n, QImage::Format_RGB32);
    for (int y = 0; y < n; ++y) {
        auto* row = reinterpret_cast<QRgb*>(square_.scanLine(y));
        const qreal v = 1.0 - static_cast<qreal>(y) / (n - 1);
        for (int x = 0; x < n; ++x) {
            const qreal s = static_cast<qreal>(x) / (n - 1);
            row[x] = QColor::fromHsvF(h_, s, v).rgb();
        }
    }
}

void ColorWheel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
}

void ColorWheel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = ringRect();
    const qreal ringW = r.width() * kRingWidthRatio;

    // 휴 링: 원뿔 그라디언트를 두꺼운 펜으로 그린다.
    QConicalGradient g(r.center(), 0.0);
    for (int i = 0; i <= 12; ++i) {
        g.setColorAt(i / 12.0, QColor::fromHsvF(std::fmod(1.0 - i / 12.0 + 1.0, 1.0), 1.0, 1.0));
    }
    // QConicalGradient 는 반시계 방향이라 휴를 뒤집어 넣었다. 마우스 쪽도 같은 규약을 쓴다.
    QPen ringPen(QBrush(g), ringW);
    p.setPen(ringPen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r.adjusted(ringW / 2, ringW / 2, -ringW / 2, -ringW / 2));

    // 휴 마커
    const qreal ang = h_ * 2.0 * kPi;
    const qreal rad = r.width() / 2.0 - ringW / 2.0;
    const QPointF hm(r.center().x() + std::cos(ang) * rad, r.center().y() - std::sin(ang) * rad);
    p.setPen(QPen(Qt::white, 2));
    p.drawEllipse(hm, ringW * 0.32, ringW * 0.32);
    p.setPen(QPen(Qt::black, 1));
    p.drawEllipse(hm, ringW * 0.32 + 1, ringW * 0.32 + 1);

    // SV 사각형
    const QRectF sq = squareRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(sq, square_);
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(sq);

    // SV 마커
    const QPointF sm(sq.left() + s_ * sq.width(), sq.top() + (1.0 - v_) * sq.height());
    p.setPen(QPen(v_ > 0.5 && s_ < 0.6 ? Qt::black : Qt::white, 1.5));
    p.drawEllipse(sm, 5.0, 5.0);
}

void ColorWheel::handle(const QPointF& pt) {
    const QRectF r = ringRect();
    const QRectF sq = squareRect();
    if (drag_ == Drag::Ring) {
        const QPointF d = pt - r.center();
        qreal h = std::atan2(-d.y(), d.x()) / (2.0 * kPi);
        if (h < 0.0) h += 1.0;
        h_ = h;
        rebuildSquare();
    } else if (drag_ == Drag::Square) {
        s_ = std::clamp((pt.x() - sq.left()) / sq.width(), 0.0, 1.0);
        v_ = std::clamp(1.0 - (pt.y() - sq.top()) / sq.height(), 0.0, 1.0);
    }
    update();
    Q_EMIT colorChanged(color());
}

void ColorWheel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const QPointF pt = e->position();
    const QRectF r = ringRect();
    const qreal dist = QLineF(r.center(), pt).length();
    const qreal outer = r.width() / 2.0;
    const qreal inner = outer * (1.0 - 2.0 * kRingWidthRatio);
    if (squareRect().adjusted(-4, -4, 4, 4).contains(pt)) {
        drag_ = Drag::Square;
    } else if (dist <= outer + 4 && dist >= inner - 4) {
        drag_ = Drag::Ring;
    } else {
        return;
    }
    handle(pt);
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ != Drag::None) {
        handle(e->position());
    }
}

void ColorWheel::mouseReleaseEvent(QMouseEvent*) {
    drag_ = Drag::None;
}

} // namespace mari::ui
