// Mari Paint — 브러시 패널 구현 (ui/brush_panel.hpp)
#include "brush_panel.hpp"

#include "brush_preview.hpp"
#include "icons.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

namespace mari::ui {

namespace {
constexpr int kPreviewW = 112;
constexpr int kPreviewH = 44;
constexpr int kIndexRole = Qt::UserRole;
} // namespace

BrushPanel::BrushPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* top = new QHBoxLayout();
    search_ = new QLineEdit(this);
    search_->setPlaceholderText("브러시 찾기…");
    search_->setClearButtonEnabled(true);
    top->addWidget(search_, 1);
    const auto btn = [&](const char* icon, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setIcon(themedIcon(icon, 16));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFixedSize(26, 26);
        connect(b, &QToolButton::clicked, this, fn);
        top->addWidget(b);
        return b;
    };
    btn("square-plus", "새 브러시(현재 브러시를 바탕으로)", [this] { Q_EMIT newRequested(); });
    btn("folder-open", "브러시 가져오기… (.abr · .sut · .mbp)", [this] { Q_EMIT importRequested(); });
    layout->addLayout(top);

    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(kPreviewW, kPreviewH));
    list_->setGridSize(QSize(kPreviewW + 12, kPreviewH + 30));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setUniformItemSizes(true);
    list_->setWordWrap(false);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setSpacing(2);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);

    connect(search_, &QLineEdit::textChanged, this, &BrushPanel::applyFilter);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* cur, QListWidgetItem*) {
        if (busy_ || cur == nullptr) return;
        current_ = indexOf(cur);
        Q_EMIT brushSelected(current_);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { Q_EMIT editRequested(indexOf(it)); });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (it != nullptr) list_->setCurrentItem(it);
        const int i = it != nullptr ? indexOf(it) : -1;
        const bool builtin = i >= 0 && static_cast<usize>(i) < builtinCount_;
        QMenu menu(this);
        if (i >= 0) {
            menu.addAction(themedIcon("settings", 16), builtin ? "편집 (사본으로 저장)…" : "편집…", [this, i] { Q_EMIT editRequested(i); });
            menu.addAction(themedIcon("copy", 16), "복제", [this, i] { Q_EMIT duplicateRequested(i); });
            QAction* del = menu.addAction(themedIcon("trash", 16), "삭제", [this, i] { Q_EMIT deleteRequested(i); });
            del->setEnabled(!builtin);
            menu.addSeparator();
        }
        menu.addAction(themedIcon("square-plus", 16), "새 브러시", [this] { Q_EMIT newRequested(); });
        menu.addAction(themedIcon("folder-open", 16), "가져오기…", [this] { Q_EMIT importRequested(); });
        menu.exec(list_->viewport()->mapToGlobal(pos));
    });
}

int BrushPanel::indexOf(const QListWidgetItem* item) const {
    return item == nullptr ? -1 : item->data(kIndexRole).toInt();
}

void BrushPanel::setBrushes(const std::vector<brush::MariBrushPreset>& brushes, usize builtinCount, int current) {
    brushes_ = brushes;
    builtinCount_ = builtinCount;
    current_ = current;
    rebuild();
}

void BrushPanel::setPreviewColor(QColor fg) {
    if (fg == fg_) return;
    fg_ = fg;
    rebuild();
}

void BrushPanel::rebuild() {
    busy_ = true;
    list_->clear();
    const qreal dpr = devicePixelRatioF();
    for (usize i = 0; i < brushes_.size(); ++i) {
        const brush::MariBrushPreset& p = brushes_[i];
        QImage img = renderBrushPreview(p, QSize(kPreviewW, kPreviewH) * dpr, fg_, QColor(0xf2, 0xf2, 0xf2));
        img.setDevicePixelRatio(dpr);
        auto* it = new QListWidgetItem(QIcon(QPixmap::fromImage(img)), QString::fromStdString(p.name), list_);
        it->setData(kIndexRole, static_cast<int>(i));
        it->setToolTip(QString("%1\n%2 · %3 px · 간격 %4%")
                           .arg(QString::fromStdString(p.name),
                                i < builtinCount_ ? "내장" : QString::fromStdString(p.sourceFormat))
                           .arg(static_cast<double>(p.tip.diameter), 0, 'f', 1)
                           .arg(static_cast<int>(p.spacing * 100.0f + 0.5f)));
        it->setSizeHint(QSize(kPreviewW + 8, kPreviewH + 26));
        if (static_cast<int>(i) == current_) list_->setCurrentItem(it);
    }
    busy_ = false;
    applyFilter(search_->text());
}

void BrushPanel::setCurrent(int index) {
    if (index == current_) return;
    current_ = index;
    busy_ = true;
    for (int i = 0; i < list_->count(); ++i) {
        if (indexOf(list_->item(i)) == index) list_->setCurrentItem(list_->item(i));
    }
    busy_ = false;
}

void BrushPanel::applyFilter(const QString& text) {
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        it->setHidden(!text.isEmpty() && !it->text().contains(text, Qt::CaseInsensitive));
    }
}

} // namespace mari::ui
