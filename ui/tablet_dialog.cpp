// Mari Paint — 태블릿 설정 구현 (ui/tablet_dialog.hpp)
#include "tablet_dialog.hpp"

#include "canvas_widget.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mari::ui {

// ── PressureCurveWidget ──────────────────────────────────────────────────

PressureCurveWidget::PressureCurveWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(260, 260);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    reset();
}

void PressureCurveWidget::reset() {
    pts_ = {{0.0, 0.0}, {0.25, 0.25}, {0.5, 0.5}, {0.75, 0.75}, {1.0, 1.0}};
    update();
    Q_EMIT curveChanged();
}

void PressureCurveWidget::setPoints(std::vector<QPointF> pts) {
    if (pts.size() < 2) return;
    pts_ = std::move(pts);
    update();
    Q_EMIT curveChanged();
}

float PressureCurveWidget::evaluate(float x) const {
    x = std::clamp(x, 0.0f, 1.0f);
    for (std::size_t i = 1; i < pts_.size(); ++i) {
        const QPointF& a = pts_[i - 1];
        const QPointF& b = pts_[i];
        if (x <= b.x()) {
            const qreal span = std::max(1e-6, b.x() - a.x());
            const qreal t = (x - a.x()) / span;
            return static_cast<float>(std::clamp(a.y() + (b.y() - a.y()) * t, 0.0, 1.0));
        }
    }
    return static_cast<float>(pts_.back().y());
}

std::array<float, 256> PressureCurveWidget::lut() const {
    std::array<float, 256> out{};
    for (int i = 0; i < 256; ++i) {
        out[static_cast<std::size_t>(i)] = evaluate(static_cast<float>(i) / 255.0f);
    }
    return out;
}

void PressureCurveWidget::setLivePressure(float p) {
    live_ = p;
    update();
}

QRectF PressureCurveWidget::plotRect() const {
    return QRectF(rect()).adjusted(12, 12, -12, -12);
}
QPointF PressureCurveWidget::toWidget(const QPointF& p) const {
    const QRectF r = plotRect();
    return {r.left() + p.x() * r.width(), r.bottom() - p.y() * r.height()};
}
QPointF PressureCurveWidget::toCurve(const QPointF& w) const {
    const QRectF r = plotRect();
    return {std::clamp((w.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - w.y()) / r.height(), 0.0, 1.0)};
}

void PressureCurveWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(r, QColor(0x1e, 0x1e, 0x1e));
    p.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1));
    for (int i = 1; i < 4; ++i) {
        const qreal x = r.left() + r.width() * i / 4.0;
        const qreal y = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1, Qt::DashLine));
    p.drawLine(toWidget({0, 0}), toWidget({1, 1}));
    p.setPen(QPen(QColor(0x3d, 0x7b, 0xd9), 2));
    for (std::size_t i = 1; i < pts_.size(); ++i) {
        p.drawLine(toWidget(pts_[i - 1]), toWidget(pts_[i]));
    }
    p.setPen(QPen(Qt::white, 1));
    p.setBrush(QColor(0x3d, 0x7b, 0xd9));
    for (const QPointF& pt : pts_) {
        p.drawEllipse(toWidget(pt), 5, 5);
    }
    if (live_ >= 0.0f) {
        p.setPen(QPen(QColor(0xff, 0x8a, 0x3d), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(toWidget({live_, evaluate(live_)}), 7, 7);
    }
    p.setPen(QColor(0x9a, 0x9a, 0x9a));
    p.drawText(QRectF(r.left(), r.bottom() + 1, r.width(), 12), Qt::AlignHCenter, "입력 압력 →");
}

void PressureCurveWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    for (std::size_t i = 0; i < pts_.size(); ++i) {
        if (QLineF(toWidget(pts_[i]), e->position()).length() <= 10) {
            drag_ = static_cast<int>(i);
            return;
        }
    }
}

void PressureCurveWidget::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0) return;
    QPointF c = toCurve(e->position());
    const auto i = static_cast<std::size_t>(drag_);
    if (i == 0) c.setX(0.0);
    else if (i == pts_.size() - 1) c.setX(1.0);
    else c.setX(std::clamp(c.x(), pts_[i - 1].x() + 0.02, pts_[i + 1].x() - 0.02));
    pts_[i] = c;
    update();
    Q_EMIT curveChanged();
}

void PressureCurveWidget::mouseReleaseEvent(QMouseEvent*) {
    drag_ = -1;
}

// ── TabletDialog ─────────────────────────────────────────────────────────

TabletDialog::TabletDialog(CanvasWidget* canvas, QWidget* parent) : QDialog(parent), canvas_(canvas) {
    setWindowTitle("태블릿 — 압력 곡선 · 테스터");
    resize(360, 460);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("전역 압력 곡선 — 점을 끌어 옮긴다. 붓 프리셋의 압력 다이내믹 앞에 적용된다.", this));
    curve_ = new PressureCurveWidget(this);
    layout->addWidget(curve_, 1);

    QSettings settings;
    const QVariantList saved = settings.value("tablet/pressureCurve").toList();
    if (saved.size() >= 4 && saved.size() % 2 == 0) {
        std::vector<QPointF> pts;
        for (int i = 0; i + 1 < saved.size(); i += 2) {
            pts.emplace_back(saved[i].toDouble(), saved[i + 1].toDouble());
        }
        curve_->setPoints(std::move(pts));
    }
    connect(curve_, &PressureCurveWidget::curveChanged, this, [this] {
        canvas_->setPressureCurve(curve_->lut());
        QVariantList flat;
        for (const QPointF& pt : curve_->points()) {
            flat << pt.x() << pt.y();
        }
        QSettings().setValue("tablet/pressureCurve", flat);
    });
    canvas_->setPressureCurve(curve_->lut());

    readout_ = new QLabel(this);
    readout_->setTextFormat(Qt::PlainText);
    layout->addWidget(readout_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* resetBtn = buttons->addButton("초기화", QDialogButtonBox::ResetRole);
    connect(resetBtn, &QPushButton::clicked, curve_, &PressureCurveWidget::reset);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, &TabletDialog::refresh);
    timer->start();
    refresh();
}

void TabletDialog::refresh() {
    const PenReadout& r = canvas_->lastPen();
    const LatencyStats& lat = canvas_->latency();
    curve_->setLivePressure(r.valid ? r.rawPressure : -1.0f);
    readout_->setText(QString("입력: %1\n압력 %2 → 곡선 뒤 %3   기울기 X %4  Y %5   회전 %6\n"
                              "펜→화면: 마지막 %7 ms · 평균 %8 · 최대 %9 · >16ms %10/%11")
                          .arg(r.valid ? (r.pen ? "펜 (windows-ink)" : "마우스/터치") : "—")
                          .arg(r.rawPressure, 0, 'f', 3)
                          .arg(r.curvedPressure, 0, 'f', 3)
                          .arg(r.tiltX, 0, 'f', 2)
                          .arg(r.tiltY, 0, 'f', 2)
                          .arg(r.rotation, 0, 'f', 0)
                          .arg(lat.lastMs, 0, 'f', 1)
                          .arg(lat.avgMs, 0, 'f', 1)
                          .arg(lat.maxMs, 0, 'f', 1)
                          .arg(lat.over16)
                          .arg(lat.samples));
}

} // namespace mari::ui
