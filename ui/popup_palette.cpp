// Mari Paint — 온캔버스 팝업 팔레트 구현 (ui/popup_palette.hpp)
#include "popup_palette.hpp"

#include "color_wheel.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

namespace mari::ui {

namespace {
constexpr int kWheel = 150;
constexpr int kSwatch = 22;
} // namespace

PopupPalette::PopupPalette(QWidget* parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);

    wheel_ = new ColorWheel(this);
    wheel_->setFixedSize(kWheel, kWheel);
    layout->addWidget(wheel_);
    connect(wheel_, &ColorWheel::colorChanged, this, [this](const QColor& c) { Q_EMIT colorChosen(c); });

    auto* right = new QVBoxLayout();
    right->setSpacing(6);
    auto* title = new QLabel("붓", this);
    right->addWidget(title);
    brushBox_ = new QWidget(this);
    auto* bl = new QVBoxLayout(brushBox_);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(2);
    right->addWidget(brushBox_);
    auto* rtitle = new QLabel("최근 색", this);
    right->addWidget(rtitle);
    recentBox_ = new QWidget(this);
    auto* rl = new QGridLayout(recentBox_);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(2);
    right->addWidget(recentBox_);
    right->addStretch(1);
    layout->addLayout(right);
}

void PopupPalette::setBrushes(const std::vector<brush::MariBrushPreset>& brushes, int current) {
    auto* bl = static_cast<QVBoxLayout*>(brushBox_->layout());
    while (QLayoutItem* it = bl->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    for (int i = 0; i < static_cast<int>(brushes.size()); ++i) {
        auto* b = new QPushButton(QString::fromStdString(brushes[static_cast<std::size_t>(i)].name), brushBox_);
        b->setCheckable(true);
        b->setChecked(i == current);
        b->setMinimumWidth(120);
        connect(b, &QPushButton::clicked, this, [this, i] {
            Q_EMIT brushChosen(i);
            hide();
        });
        bl->addWidget(b);
    }
}

void PopupPalette::setRecentColors(const std::vector<QColor>& colors) {
    auto* rl = static_cast<QGridLayout*>(recentBox_->layout());
    while (QLayoutItem* it = rl->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    for (int i = 0; i < static_cast<int>(colors.size()) && i < 12; ++i) {
        const QColor c = colors[static_cast<std::size_t>(i)];
        auto* b = new QToolButton(recentBox_);
        b->setFixedSize(kSwatch, kSwatch);
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #555; border-radius: 2px; }").arg(c.name()));
        connect(b, &QToolButton::clicked, this, [this, c] {
            Q_EMIT colorChosen(c);
            hide();
        });
        rl->addWidget(b, i / 6, i % 6);
    }
}

void PopupPalette::setColor(const QColor& c) {
    wheel_->setColor(c);
}

void PopupPalette::popupAt(const QPoint& globalCenter) {
    adjustSize();
    QPoint tl = globalCenter - QPoint(kWheel / 2 + 12, kWheel / 2 + 12);
    if (const QScreen* s = screen()) {
        const QRect avail = s->availableGeometry();
        tl.setX(std::clamp(tl.x(), avail.left(), avail.right() - width()));
        tl.setY(std::clamp(tl.y(), avail.top(), avail.bottom() - height()));
    }
    move(tl);
    show();
}

void PopupPalette::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0x1e, 0x1e, 0x1e), 1));
    p.setBrush(QColor(0x2b, 0x2b, 0x2b, 235));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
}

} // namespace mari::ui
