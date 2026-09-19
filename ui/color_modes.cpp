// Mari Paint — 색 선택 방식들 구현 (ui/color_modes.hpp)
#include "color_modes.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace mari::ui {

namespace {
constexpr int kBarW = 18;  ///< 휴 막대 폭
constexpr int kGap = 8;    ///< 사각형과 막대 사이
constexpr int kStops = 17; ///< 슬라이더 배경 샘플 수(HSV 휴가 매끈하려면 12 이상)

/// 무채색이면 휴가 -1 로 온다. 이전 휴를 지킨다(검정으로 갔다 와도 휴가 안 튄다).
qreal keepHue(qreal h, qreal prev) { return h < 0.0 ? prev : h; }
} // namespace

// ── ColorBox ─────────────────────────────────────────────────────────────

ColorBox::ColorBox(QWidget* parent) : QWidget(parent) {
    setMinimumSize(120, 100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rebuildSquare();
}

void ColorBox::setColor(const QColor& c) {
    const QColor hsv = c.toHsv();
    const qreal h = keepHue(hsv.hsvHueF(), h_);
    const bool hueChanged = std::abs(h - h_) > 1e-6;
    h_ = h;
    s_ = hsv.hsvSaturationF();
    v_ = hsv.valueF();
    if (hueChanged) rebuildSquare();
    update();
}

QRectF ColorBox::squareRect() const {
    return QRectF(0.5, 0.5, width() - kBarW - kGap - 1.0, height() - 1.0);
}

QRectF ColorBox::barRect() const {
    return QRectF(width() - kBarW - 0.5, 0.5, kBarW, height() - 1.0);
}

void ColorBox::rebuildSquare() {
    constexpr int n = 128;
    square_ = QImage(n, n, QImage::Format_RGB32);
    for (int y = 0; y < n; ++y) {
        auto* row = reinterpret_cast<QRgb*>(square_.scanLine(y));
        const qreal v = 1.0 - static_cast<qreal>(y) / (n - 1);
        for (int x = 0; x < n; ++x) row[x] = QColor::fromHsvF(h_, static_cast<qreal>(x) / (n - 1), v).rgb();
    }
}

void ColorBox::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF sq = squareRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(sq, square_);
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(sq);

    // 휴 막대: 위 = 0°(빨강) → 아래 = 360°
    const QRectF bar = barRect();
    QLinearGradient g(bar.topLeft(), bar.bottomLeft());
    for (int i = 0; i <= 12; ++i) g.setColorAt(i / 12.0, QColor::fromHsvF(std::fmod(i / 12.0, 1.0), 1.0, 1.0));
    p.setBrush(g);
    p.drawRoundedRect(bar, 2, 2);
    const qreal hy = bar.top() + h_ * bar.height();
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(bar.left() - 1, hy - 2, bar.width() + 2, 4));
    p.setPen(QPen(Qt::black, 1));
    p.drawRect(QRectF(bar.left() - 2, hy - 3, bar.width() + 4, 6));

    const QPointF sm(sq.left() + s_ * sq.width(), sq.top() + (1.0 - v_) * sq.height());
    p.setPen(QPen(v_ > 0.5 && s_ < 0.6 ? Qt::black : Qt::white, 1.5));
    p.drawEllipse(sm, 5.0, 5.0);
}

void ColorBox::handle(const QPointF& pt) {
    if (drag_ == Drag::Bar) {
        const QRectF bar = barRect();
        h_ = std::clamp((pt.y() - bar.top()) / bar.height(), 0.0, 0.9999);
        rebuildSquare();
    } else if (drag_ == Drag::Square) {
        const QRectF sq = squareRect();
        s_ = std::clamp((pt.x() - sq.left()) / sq.width(), 0.0, 1.0);
        v_ = std::clamp(1.0 - (pt.y() - sq.top()) / sq.height(), 0.0, 1.0);
    }
    update();
    Q_EMIT colorChanged(color());
}

void ColorBox::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const QPointF pt = e->position();
    if (barRect().adjusted(-4, -4, 4, 4).contains(pt)) {
        drag_ = Drag::Bar;
    } else if (squareRect().adjusted(-4, -4, 4, 4).contains(pt)) {
        drag_ = Drag::Square;
    } else {
        return;
    }
    handle(pt);
}

void ColorBox::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ != Drag::None) handle(e->position());
}

void ColorBox::mouseReleaseEvent(QMouseEvent*) { drag_ = Drag::None; }

// ── ChannelSlider ────────────────────────────────────────────────────────

ChannelSlider::ChannelSlider(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    stops_.fill(QColor(0x80, 0x80, 0x80));
}

void ChannelSlider::setValue(int v) {
    v = std::clamp(v, 0, max_);
    if (v == value_) return;
    value_ = v;
    update();
}

void ChannelSlider::setGradient(const std::array<QColor, 17>& stops) {
    stops_ = stops;
    update();
}

void ChannelSlider::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(1, 3, -1, -3);
    QLinearGradient g(r.topLeft(), r.topRight());
    for (int i = 0; i < kStops; ++i) g.setColorAt(static_cast<qreal>(i) / (kStops - 1), stops_[static_cast<std::size_t>(i)]);
    p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
    p.setBrush(g);
    p.drawRoundedRect(r, 3, 3);
    const qreal x = r.left() + (max_ > 0 ? static_cast<qreal>(value_) / max_ : 0.0) * r.width();
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(x - 2, r.top() - 2, 4, r.height() + 4));
    p.setPen(QPen(Qt::black, 1));
    p.drawRect(QRectF(x - 3, r.top() - 3, 6, r.height() + 6));
}

void ChannelSlider::handle(qreal x) {
    const QRectF r = rect().adjusted(1, 3, -1, -3);
    const int v = static_cast<int>(std::lround(std::clamp((x - r.left()) / r.width(), 0.0, 1.0) * max_));
    if (v != value_) {
        value_ = v;
        update();
        Q_EMIT valueChanged(value_);
    }
}

void ChannelSlider::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) handle(e->position().x());
}

void ChannelSlider::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) handle(e->position().x());
}

// ── ColorSliders ─────────────────────────────────────────────────────────

ColorSliders::ColorSliders(Space space, QWidget* parent) : QWidget(parent), space_(space) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 4, 0, 4);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    const char* names[3] = {"R", "G", "B"};
    int maxes[3] = {255, 255, 255};
    if (space == Space::HSV) {
        names[0] = "H"; names[1] = "S"; names[2] = "V";
        maxes[0] = 359; maxes[1] = 100; maxes[2] = 100;
    } else if (space == Space::HSL) {
        names[0] = "H"; names[1] = "S"; names[2] = "L";
        maxes[0] = 359; maxes[1] = 100; maxes[2] = 100;
    }
    for (int i = 0; i < 3; ++i) {
        auto* label = new QLabel(names[i], this);
        label->setFixedWidth(14);
        auto* sl = new ChannelSlider(this);
        sl->setRange(maxes[i]);
        auto* sp = new QSpinBox(this);
        sp->setRange(0, maxes[i]);
        sp->setFixedWidth(56);
        sp->setButtonSymbols(QAbstractSpinBox::NoButtons);
        grid->addWidget(label, i, 0);
        grid->addWidget(sl, i, 1);
        grid->addWidget(sp, i, 2);
        sliders_[static_cast<std::size_t>(i)] = sl;
        spins_[static_cast<std::size_t>(i)] = sp;
        connect(sl, &ChannelSlider::valueChanged, this, [this, i](int v) {
            if (updating_) return;
            updating_ = true;
            spins_[static_cast<std::size_t>(i)]->setValue(v);
            updating_ = false;
            syncFromSliders();
        });
        connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int v) {
            if (updating_) return;
            updating_ = true;
            sliders_[static_cast<std::size_t>(i)]->setValue(v);
            updating_ = false;
            syncFromSliders();
        });
    }
    grid->setColumnStretch(1, 1);
    syncFromColor();
}

QColor ColorSliders::compose(int a, int b, int c) const {
    switch (space_) {
    case Space::RGB: return QColor(a, b, c);
    case Space::HSV: return QColor::fromHsvF(a / 360.0, b / 100.0, c / 100.0);
    case Space::HSL: return QColor::fromHslF(a / 360.0, b / 100.0, c / 100.0);
    }
    return QColor();
}

void ColorSliders::setColor(const QColor& c) {
    if (!c.isValid()) return;
    color_ = c;
    syncFromColor();
}

void ColorSliders::syncFromColor() {
    int v[3] = {0, 0, 0};
    switch (space_) {
    case Space::RGB:
        v[0] = color_.red(); v[1] = color_.green(); v[2] = color_.blue();
        break;
    case Space::HSV: {
        const QColor h = color_.toHsv();
        // 무채색이면 휴 슬라이더는 그대로 둔다(지금 값 유지)
        v[0] = h.hsvHue() < 0 ? sliders_[0]->value() : h.hsvHue();
        v[1] = static_cast<int>(std::lround(h.hsvSaturationF() * 100.0));
        v[2] = static_cast<int>(std::lround(h.valueF() * 100.0));
        break;
    }
    case Space::HSL: {
        const QColor h = color_.toHsl();
        v[0] = h.hslHue() < 0 ? sliders_[0]->value() : h.hslHue();
        v[1] = static_cast<int>(std::lround(h.hslSaturationF() * 100.0));
        v[2] = static_cast<int>(std::lround(h.lightnessF() * 100.0));
        break;
    }
    }
    updating_ = true;
    for (int i = 0; i < 3; ++i) {
        sliders_[static_cast<std::size_t>(i)]->setValue(v[i]);
        spins_[static_cast<std::size_t>(i)]->setValue(v[i]);
    }
    updating_ = false;
    repaintGradients();
}

void ColorSliders::syncFromSliders() {
    color_ = compose(sliders_[0]->value(), sliders_[1]->value(), sliders_[2]->value());
    repaintGradients();
    Q_EMIT colorChanged(color_);
}

void ColorSliders::repaintGradients() {
    const int cur[3] = {sliders_[0]->value(), sliders_[1]->value(), sliders_[2]->value()};
    const int maxes[3] = {space_ == Space::RGB ? 255 : 359, space_ == Space::RGB ? 255 : 100,
                          space_ == Space::RGB ? 255 : 100};
    for (int ch = 0; ch < 3; ++ch) {
        std::array<QColor, 17> stops{};
        for (int i = 0; i < kStops; ++i) {
            int v[3] = {cur[0], cur[1], cur[2]};
            v[ch] = static_cast<int>(std::lround(static_cast<qreal>(i) / (kStops - 1) * maxes[ch]));
            stops[static_cast<std::size_t>(i)] = compose(v[0], v[1], v[2]);
        }
        sliders_[static_cast<std::size_t>(ch)]->setGradient(stops);
    }
}

} // namespace mari::ui
