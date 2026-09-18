// Mari Paint — 색 패널 구현 (ui/color_panel.hpp)
#include "color_panel.hpp"

#include "color_wheel.hpp"
#include "icons.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace mari::ui {

namespace {

constexpr int kRecentMax = 16;
constexpr int kRecentCols = 8;
constexpr int kRecentPx = 22;

QString swatchStyle(const QColor& c, int size) {
    return QString("QToolButton { background: %1; border: 1px solid #555; border-radius: 2px; min-width: %2px; "
                   "min-height: %2px; max-width: %2px; max-height: %2px; }")
        .arg(c.name())
        .arg(size);
}

} // namespace

// 전경/배경 겹친 두 사각형(PS·Krita·CSP 관례). 클릭하면 교환.
class FgBgSwatch final : public QWidget {
public:
    explicit FgBgSwatch(QWidget* parent) : QWidget(parent) {
        setFixedSize(48, 48);
        setToolTip("전경색 / 배경색 — 클릭: 교환 (X) · D: 기본");
        setCursor(Qt::PointingHandCursor);
    }
    void set(const QColor& fg, const QColor& bg) {
        fg_ = fg;
        bg_ = bg;
        update();
    }
    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
        p.setBrush(bg_);
        p.drawRoundedRect(QRectF(13.5, 13.5, 33, 33), 2, 2);
        p.setBrush(fg_);
        p.drawRoundedRect(QRectF(0.5, 0.5, 33, 33), 2, 2);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onClick) onClick();
    }

private:
    QColor fg_{0, 0, 0}, bg_{255, 255, 255};
};

ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    wheel_ = new ColorWheel(this);
    wheel_->setMaximumSize(220, 220);
    wheel_->setColor(fg_);
    layout->addWidget(wheel_, 1, Qt::AlignHCenter);
    connect(wheel_, &ColorWheel::colorChanged, this, [this](const QColor& c) {
        if (updating_) return;
        fg_ = c;
        updateFgBgButtons();
        emit foregroundChanged(fg_);
    });

    auto* row = new QHBoxLayout();
    row->setSpacing(8);
    swatch_ = new FgBgSwatch(this);
    swatch_->onClick = [this] { swap(); };
    row->addWidget(swatch_);
    auto* swapBtn = new QToolButton(this);
    swapBtn->setIcon(themedIcon("arrows-exchange", 14));
    swapBtn->setIconSize(QSize(14, 14));
    swapBtn->setAutoRaise(true);
    swapBtn->setToolTip("전경↔배경 교환 (X)");
    connect(swapBtn, &QToolButton::clicked, this, &ColorPanel::swap);
    row->addWidget(swapBtn);
    hex_ = new QLineEdit(this);
    hex_->setMaxLength(7);
    hex_->setPlaceholderText("#rrggbb");
    hex_->setFixedWidth(84);
    connect(hex_, &QLineEdit::editingFinished, this, [this] {
        const QColor c(hex_->text());
        if (c.isValid()) {
            setForeground(c);
        }
    });
    row->addWidget(hex_);
    row->addStretch(1);
    layout->addLayout(row);

    recentBox_ = new QWidget(this);
    auto* grid = new QGridLayout(recentBox_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);
    layout->addWidget(recentBox_);
    updateFgBgButtons();
    rebuildSwatches();
}

void ColorPanel::setForeground(const QColor& c) {
    if (!c.isValid()) return;
    fg_ = c;
    updating_ = true;
    wheel_->setColor(fg_);
    updating_ = false;
    updateFgBgButtons();
    emit foregroundChanged(fg_);
}

void ColorPanel::swap() {
    std::swap(fg_, bg_);
    setForeground(fg_);
}

void ColorPanel::resetDefaults() {
    bg_ = QColor(255, 255, 255);
    setForeground(QColor(0, 0, 0));
}

void ColorPanel::noteUsed(const QColor& c) {
    recent_.erase(std::remove(recent_.begin(), recent_.end(), c), recent_.end());
    recent_.insert(recent_.begin(), c);
    if (recent_.size() > kRecentMax) {
        recent_.resize(kRecentMax);
    }
    rebuildSwatches();
}

void ColorPanel::updateFgBgButtons() {
    swatch_->set(fg_, bg_);
    if (!hex_->hasFocus()) {
        hex_->setText(fg_.name());
    }
}

void ColorPanel::rebuildSwatches() {
    auto* grid = static_cast<QGridLayout*>(recentBox_->layout());
    while (QLayoutItem* it = grid->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    // 항상 두 줄을 차지한다 — 색이 늘 때마다 도크가 들썩이지 않게.
    for (int i = 0; i < kRecentMax; ++i) {
        auto* b = new QToolButton(recentBox_);
        if (i < static_cast<int>(recent_.size())) {
            const QColor c = recent_[static_cast<std::size_t>(i)];
            b->setStyleSheet(swatchStyle(c, kRecentPx));
            b->setToolTip(c.name());
            connect(b, &QToolButton::clicked, this, [this, c] { setForeground(c); });
        } else {
            b->setStyleSheet(swatchStyle(QColor(0x33, 0x33, 0x33), kRecentPx));
            b->setEnabled(false);
        }
        grid->addWidget(b, i / kRecentCols, i % kRecentCols);
    }
}

} // namespace mari::ui
