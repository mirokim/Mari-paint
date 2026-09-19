// Mari Paint — 색 패널 구현 (ui/color_panel.hpp)
#include "color_panel.hpp"

#include "color_modes.hpp"
#include "color_wheel.hpp"
#include "icons.hpp"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStackedWidget>
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

    // 선택 방식 콤보 + 스택. 어느 방식이든 fg_ 하나를 바꾸고, 나머지 방식엔 setColor 로 밀어 넣는다.
    modeCombo_ = new QComboBox(this);
    modeCombo_->addItems({"색상환", "사각형", "RGB 슬라이더", "HSV 슬라이더", "HSL 슬라이더"});
    modeCombo_->setToolTip("색 선택 방식");
    layout->addWidget(modeCombo_);

    stack_ = new QStackedWidget(this);
    stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    wheel_ = new ColorWheel(this);
    box_ = new ColorBox(this);
    rgb_ = new ColorSliders(ColorSliders::Space::RGB, this);
    hsv_ = new ColorSliders(ColorSliders::Space::HSV, this);
    hsl_ = new ColorSliders(ColorSliders::Space::HSL, this);
    stack_->addWidget(wheel_); // 남는 공간은 위젯이 알아서 가운데 원으로 쓴다
    stack_->addWidget(box_);
    auto slidersHost = [&](ColorSliders* w) -> QWidget* {
        auto* host = new QWidget(this);
        auto* l = new QVBoxLayout(host);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(w);
        l->addStretch(1);
        return host;
    };
    stack_->addWidget(slidersHost(rgb_));
    stack_->addWidget(slidersHost(hsv_));
    stack_->addWidget(slidersHost(hsl_));
    layout->addWidget(stack_, 1);

    auto onPicked = [this](const QColor& c) {
        if (updating_) return;
        fg_ = c;
        updating_ = true;
        pushToPickers();
        updating_ = false;
        updateFgBgButtons();
        Q_EMIT foregroundChanged(fg_);
    };
    connect(wheel_, &ColorWheel::colorChanged, this, onPicked);
    connect(box_, &ColorBox::colorChanged, this, onPicked);
    connect(rgb_, &ColorSliders::colorChanged, this, onPicked);
    connect(hsv_, &ColorSliders::colorChanged, this, onPicked);
    connect(hsl_, &ColorSliders::colorChanged, this, onPicked);
    connect(modeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        setMode(static_cast<Mode>(i));
        QSettings().setValue("color/mode", i);
    });
    pushToPickers();
    setMode(static_cast<Mode>(std::clamp(QSettings().value("color/mode", 0).toInt(), 0, 4)));

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

void ColorPanel::setMode(Mode m) {
    const int i = static_cast<int>(m);
    if (modeCombo_->currentIndex() != i) modeCombo_->setCurrentIndex(i);
    stack_->setCurrentIndex(i);
}

void ColorPanel::pushToPickers() {
    // 지금 보이는 것만이 아니라 전부 맞춰 둔다 — 방식을 바꿔도 색이 튀지 않게.
    wheel_->setColor(fg_);
    box_->setColor(fg_);
    rgb_->setColor(fg_);
    hsv_->setColor(fg_);
    hsl_->setColor(fg_);
}

void ColorPanel::setForeground(const QColor& c) {
    if (!c.isValid()) return;
    fg_ = c;
    updating_ = true;
    pushToPickers();
    updating_ = false;
    updateFgBgButtons();
    Q_EMIT foregroundChanged(fg_);
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
