// Mari Paint — 색 패널 구현 (ui/color_panel.hpp)
#include "color_panel.hpp"

#include "color_wheel.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace mari::ui {

namespace {

constexpr int kRecentMax = 12;

QString swatchStyle(const QColor& c, int size) {
    return QString("QToolButton { background: %1; border: 1px solid #555; min-width: %2px; min-height: %2px; "
                   "max-width: %2px; max-height: %2px; }")
        .arg(c.name())
        .arg(size);
}

} // namespace

ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    wheel_ = new ColorWheel(this);
    wheel_->setColor(fg_);
    layout->addWidget(wheel_, 1);
    connect(wheel_, &ColorWheel::colorChanged, this, [this](const QColor& c) {
        if (updating_) return;
        fg_ = c;
        updateFgBgButtons();
        emit foregroundChanged(fg_);
    });

    auto* row = new QHBoxLayout();
    fgButton_ = new QToolButton(this);
    fgButton_->setToolTip("전경색 (X: 교환 · D: 기본)");
    bgButton_ = new QToolButton(this);
    bgButton_->setToolTip("배경색");
    connect(bgButton_, &QToolButton::clicked, this, &ColorPanel::swap);
    row->addWidget(fgButton_);
    row->addWidget(bgButton_);
    hex_ = new QLineEdit(this);
    hex_->setMaxLength(7);
    hex_->setPlaceholderText("#rrggbb");
    hex_->setFixedWidth(80);
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
    fgButton_->setStyleSheet(swatchStyle(fg_, 32));
    bgButton_->setStyleSheet(swatchStyle(bg_, 32));
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
    int i = 0;
    for (const QColor& c : recent_) {
        auto* b = new QToolButton(recentBox_);
        b->setStyleSheet(swatchStyle(c, 18));
        b->setToolTip(c.name());
        connect(b, &QToolButton::clicked, this, [this, c] { setForeground(c); });
        grid->addWidget(b, i / 6, i % 6);
        ++i;
    }
}

} // namespace mari::ui
