// Mari Paint — 작은 도크 구현 (ui/side_panels.hpp)
#include "side_panels.hpp"

#include "icons.hpp"

#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace mari::ui {

// ── HistoryPanel ─────────────────────────────────────────────────────────

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    list_ = new QListWidget(this);
    list_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(list_, 1);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (busy_ || doc_ == nullptr) return;
        const int target = it->data(Qt::UserRole).toInt();          // 그 항목이 "적용된 상태"의 undoCount
        const int now = static_cast<int>(doc_->undoStack().undoCount());
        if (target != now) Q_EMIT jumpRequested(now - target);
    });
}

void HistoryPanel::setDocument(app::Document* doc) {
    doc_ = doc;
    refresh();
}

void HistoryPanel::refresh() {
    busy_ = true;
    list_->clear();
    if (doc_ != nullptr) {
        const auto past = doc_->undoStack().undoEntries();
        const auto future = doc_->undoStack().redoEntries();
        // "붓질 — 14:19:05". 자정을 넘긴 항목은 날짜도 붙인다.
        const QDate today = QDate::currentDate();
        auto label = [&](const UndoStack::HistoryEntry& e) {
            if (e.pushedAtMs == 0) return QString::fromStdString(e.text);
            const QDateTime t = QDateTime::fromMSecsSinceEpoch(e.pushedAtMs);
            const QString when = t.date() == today ? t.toString("HH:mm:ss") : t.toString("MM-dd HH:mm:ss");
            return QString("%1 — %2").arg(QString::fromStdString(e.text), when);
        };
        auto* origin = new QListWidgetItem("(처음)", list_);
        origin->setData(Qt::UserRole, 0);
        for (usize i = 0; i < past.size(); ++i) {
            auto* it = new QListWidgetItem(label(past[i]), list_);
            it->setData(Qt::UserRole, static_cast<int>(i + 1));
        }
        const int now = static_cast<int>(past.size());
        for (usize i = 0; i < future.size(); ++i) {
            auto* it = new QListWidgetItem(label(future[i]), list_);
            it->setData(Qt::UserRole, now + static_cast<int>(i + 1));
            it->setForeground(QColor(0x80, 0x80, 0x80));
        }
        list_->setCurrentRow(now);
        list_->scrollToItem(list_->item(now));
    }
    busy_ = false;
}

// ── PalettePanel ─────────────────────────────────────────────────────────

namespace {
const char* kDefaultPalette[] = {"#000000", "#ffffff", "#7f7f7f", "#c3c3c3", "#880015", "#ed1c24", "#ff7f27", "#fff200",
                                 "#22b14c", "#00a2e8", "#3f48cc", "#a349a4", "#b97a57", "#ffaec9", "#ffc90e", "#efe4b0",
                                 "#b5e61d", "#99d9ea", "#7092be", "#c8bfe7"};
}

PalettePanel::PalettePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* top = new QHBoxLayout();
    top->addStretch(1);
    auto* add = new QToolButton(this);
    add->setIcon(themedIcon("square-plus", 16));
    add->setToolTip("현재 전경색 추가");
    add->setAutoRaise(true);
    top->addWidget(add);
    auto* reset = new QToolButton(this);
    reset->setIcon(themedIcon("rotate", 16));
    reset->setToolTip("기본 팔레트로");
    reset->setAutoRaise(true);
    top->addWidget(reset);
    layout->addLayout(top);
    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(22, 22));
    list_->setGridSize(QSize(28, 28));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setSpacing(1);
    layout->addWidget(list_, 1);
    connect(add, &QToolButton::clicked, this, [this] { addColor(current_); });
    connect(reset, &QToolButton::clicked, this, [this] {
        colors_.clear();
        for (const char* c : kDefaultPalette) colors_.push_back(QColor(c));
        save();
        rebuild();
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { Q_EMIT colorChosen(it->data(Qt::UserRole).value<QColor>()); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (it == nullptr) return;
        const int row = list_->row(it);
        if (row >= 0 && static_cast<usize>(row) < colors_.size()) {
            colors_.erase(colors_.begin() + row);
            save();
            rebuild();
        }
    });
    const QStringList saved = QSettings().value("palette/colors").toStringList();
    if (saved.isEmpty()) for (const char* c : kDefaultPalette) colors_.push_back(QColor(c));
    else for (const QString& c : saved) colors_.push_back(QColor(c));
    rebuild();
}

void PalettePanel::addColor(const QColor& c) {
    colors_.push_back(c);
    save();
    rebuild();
}

void PalettePanel::save() {
    QStringList out;
    for (const QColor& c : colors_) out << c.name();
    QSettings().setValue("palette/colors", out);
}

void PalettePanel::rebuild() {
    list_->clear();
    for (const QColor& c : colors_) {
        QPixmap pm(22, 22);
        pm.fill(c);
        QPainter p(&pm);
        p.setPen(QColor(0, 0, 0, 120));
        p.drawRect(0, 0, 21, 21);
        p.end();
        auto* it = new QListWidgetItem(QIcon(pm), QString(), list_);
        it->setData(Qt::UserRole, c);
        it->setToolTip(c.name());
        it->setSizeHint(QSize(26, 26));
    }
}

// ── ReferencePanel ───────────────────────────────────────────────────────

ReferencePanel::ReferencePanel(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(120);
    setMouseTracking(true);
    setToolTip("더블클릭: 이미지 열기 · 휠: 줌 · 드래그: 이동 · 우클릭/Alt+클릭: 색 따기");
}

void ReferencePanel::open() {
    const QString path = QFileDialog::getOpenFileName(this, "참조 이미지", QString(), "이미지 (*.png *.jpg *.jpeg *.bmp *.webp *.gif)");
    if (path.isEmpty()) return;
    QImage img(path);
    if (img.isNull()) return;
    img_ = img.convertToFormat(QImage::Format_ARGB32);
    needFit_ = true;
    update();
}

void ReferencePanel::clear() {
    img_ = QImage();
    update();
}

void ReferencePanel::fit() {
    if (img_.isNull() || width() < 8 || height() < 8) return;
    zoom_ = std::min(static_cast<double>(width()) / img_.width(), static_cast<double>(height()) / img_.height());
    offset_ = QPointF((width() - img_.width() * zoom_) * 0.5, (height() - img_.height() * zoom_) * 0.5);
    needFit_ = false;
}

void ReferencePanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x22, 0x22, 0x22));
    if (img_.isNull()) {
        p.setPen(QColor(0x80, 0x80, 0x80));
        p.drawText(rect(), Qt::AlignCenter, "더블클릭해서 참조 이미지를 연다");
        return;
    }
    if (needFit_) fit();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.translate(offset_);
    p.scale(zoom_, zoom_);
    p.drawImage(0, 0, img_);
}

void ReferencePanel::wheelEvent(QWheelEvent* e) {
    if (img_.isNull()) return;
    const double k = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    const QPointF at = e->position();
    offset_ = at - (at - offset_) * k;
    zoom_ = std::clamp(zoom_ * k, 0.02, 40.0);
    update();
}

void ReferencePanel::mousePressEvent(QMouseEvent* e) {
    if (img_.isNull()) return;
    const bool pick = e->button() == Qt::RightButton || (e->modifiers() & Qt::AltModifier);
    if (pick) {
        const QPointF ip = (e->position() - offset_) / zoom_;
        const int x = static_cast<int>(ip.x()), y = static_cast<int>(ip.y());
        if (x >= 0 && y >= 0 && x < img_.width() && y < img_.height()) Q_EMIT colorPicked(QColor(img_.pixel(x, y)));
        return;
    }
    dragging_ = true;
    dragLast_ = e->position();
}

void ReferencePanel::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    offset_ += e->position() - dragLast_;
    dragLast_ = e->position();
    update();
}

void ReferencePanel::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void ReferencePanel::mouseDoubleClickEvent(QMouseEvent*) { open(); }

} // namespace mari::ui
