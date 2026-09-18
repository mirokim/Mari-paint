// Mari Paint — 레이어 패널 구현 (ui/layer_panel.hpp)
#include "layer_panel.hpp"

#include "icons.hpp"
#include "layer_row_delegate.hpp"

#include <mari/app/layer_commands.hpp>
#include <mari/core/layer_ops.hpp>

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QAction>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <vector>

namespace mari::ui {

namespace {

constexpr int kThumbW = 48;
constexpr int kThumbH = 36;

// 화면에 보여 줄 블렌드 모드(Erase 는 지우개 스탬프용이라 뺀다).
const BlendMode kBlendModes[] = {
    BlendMode::Normal,     BlendMode::Multiply,   BlendMode::Screen,     BlendMode::Overlay,
    BlendMode::Darken,     BlendMode::Lighten,    BlendMode::ColorDodge, BlendMode::ColorBurn,
    BlendMode::HardLight,  BlendMode::SoftLight,  BlendMode::Difference, BlendMode::Exclusion,
    BlendMode::Hue,        BlendMode::Saturation, BlendMode::Color,      BlendMode::Luminosity,
    BlendMode::Add,        BlendMode::Subtract,
};

QPixmap checkerThumb(const QImage& content, qreal dpr) {
    QPixmap pm(QSize(kThumbW, kThumbH) * dpr);
    pm.setDevicePixelRatio(dpr);
    QPainter p(&pm);
    p.fillRect(QRect(0, 0, kThumbW, kThumbH), QColor(200, 200, 200));
    for (int y = 0; y < kThumbH; y += 6) {
        for (int x = (y / 6 % 2) * 6; x < kThumbW; x += 12) {
            p.fillRect(x, y, 6, 6, QColor(160, 160, 160));
        }
    }
    if (!content.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRect(0, 0, kThumbW, kThumbH), content);
    }
    return pm;
}

QToolButton* iconButton(QWidget* parent, const char* icon, const QString& tip, int px = 16) {
    auto* b = new QToolButton(parent);
    b->setIcon(themedIcon(icon, px));
    b->setIconSize(QSize(px, px));
    b->setToolTip(tip);
    b->setFixedSize(28, 28);
    b->setAutoRaise(true);
    return b;
}

} // namespace

LayerPanel::LayerPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* top = new QHBoxLayout();
    top->setSpacing(4);
    blend_ = new QComboBox(this);
    for (const BlendMode m : kBlendModes) {
        blend_->addItem(QString::fromLatin1(blendModeName(m)), static_cast<int>(m));
    }
    top->addWidget(blend_, 1);
    lock_ = iconButton(this, "lock", "잠금 — 잠긴 레이어에는 그려지지 않는다");
    lock_->setCheckable(true);
    alphaLock_ = iconButton(this, "square-half", "알파 잠금 — 이미 칠한 자리에만 칠해진다");
    alphaLock_->setCheckable(true);
    top->addWidget(lock_);
    top->addWidget(alphaLock_);
    layout->addLayout(top);

    auto* opRow = new QHBoxLayout();
    opRow->setSpacing(6);
    opRow->addWidget(new QLabel("불투명도", this));
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(0, 100);
    opacity_->setValue(100);
    opRow->addWidget(opacity_, 1);
    opacitySpin_ = new QSpinBox(this);
    opacitySpin_->setRange(0, 100);
    opacitySpin_->setSuffix("%");
    opacitySpin_->setFixedWidth(60);
    opacitySpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    opRow->addWidget(opacitySpin_);
    layout->addLayout(opRow);

    list_ = new QListWidget(this);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setDefaultDropAction(Qt::MoveAction);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (doc_ == nullptr) return;
        if (QListWidgetItem* item = list_->itemAt(pos)) list_->setCurrentItem(item);
        const LayerPtr l = activeLayer();
        if (!l) return;
        QMenu menu(this);
        menu.addAction(themedIcon("square-plus", 16), "새 레이어", [this] { addLayer(); });
        menu.addAction(themedIcon("copy", 16), "복제", [this] { duplicateLayer(); });
        menu.addAction(themedIcon("arrow-merge", 16), "아래와 병합", [this] { Q_EMIT mergeDownRequested(); });
        menu.addSeparator();
        QAction* vis = menu.addAction(l->visible() ? "숨기기" : "보이기");
        connect(vis, &QAction::triggered, this, [this, l] {
            l->setVisible(!l->visible());
            refresh();
            markDirty();
        });
        QAction* lock = menu.addAction(themedIcon("lock", 16), "잠금");
        lock->setCheckable(true);
        lock->setChecked(l->locked());
        connect(lock, &QAction::toggled, this, [this, l](bool on) { l->setLocked(on); refresh(); });
        QAction* alpha = menu.addAction(themedIcon("square-half", 16), "알파 잠금");
        alpha->setCheckable(true);
        alpha->setChecked(l->alphaLocked());
        connect(alpha, &QAction::toggled, this, [this, l](bool on) { l->setAlphaLocked(on); refresh(); });
        menu.addSeparator();
        menu.addAction("이름 바꾸기", [this] { if (QListWidgetItem* it = list_->currentItem()) list_->editItem(it); });
        menu.addAction(themedIcon("trash", 16), "삭제", [this] { removeLayer(); });
        menu.exec(list_->viewport()->mapToGlobal(pos));
    });
    auto* delegate = new LayerRowDelegate(list_);
    list_->setItemDelegate(delegate);
    layout->addWidget(list_, 1);
    connect(delegate, &LayerRowDelegate::visibilityToggled, this, [this](const QModelIndex& idx) {
        if (doc_ == nullptr) return;
        QListWidgetItem* item = list_->item(idx.row());
        if (item == nullptr) return;
        const auto id = static_cast<LayerId>(item->data(kLayerIdRole).toULongLong());
        if (const LayerPtr l = doc_->layers().find(id)) {
            l->setVisible(!l->visible());
            busy_ = true;
            item->setData(kVisibleRole, l->visible());
            busy_ = false;
            markDirty();
        }
    });

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(2);
    const auto btn = [&](const char* icon, const QString& tip, auto fn) {
        QToolButton* b = iconButton(this, icon, tip);
        connect(b, &QToolButton::clicked, this, fn);
        buttons->addWidget(b);
        return b;
    };
    btn("square-plus", "새 레이어 (Insert)", [this] { addLayer(); });
    btn("copy", "레이어 복제", [this] { duplicateLayer(); });
    btn("arrow-merge", "아래와 병합 (Ctrl+E)", [this] { Q_EMIT mergeDownRequested(); });
    buttons->addSpacing(6);
    btn("arrow-up", "위로", [this] { moveActive(+1); });
    btn("arrow-down", "아래로", [this] { moveActive(-1); });
    buttons->addStretch(1);
    btn("trash", "레이어 삭제", [this] { removeLayer(); });
    layout->addLayout(buttons);

    connect(list_, &QListWidget::currentRowChanged, this, &LayerPanel::onRowChanged);
    connect(list_, &QListWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(list_->model(), &QAbstractItemModel::rowsMoved, this, [this] { onRowsMoved(); });
    connect(blend_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (busy_ || i < 0) return;
        if (const LayerPtr l = activeLayer()) {
            l->setBlendMode(static_cast<BlendMode>(blend_->itemData(i).toInt()));
            markDirty();
        }
    });
    connect(opacity_, &QSlider::valueChanged, this, [this](int v) {
        if (busy_) return;
        busy_ = true;
        opacitySpin_->setValue(v);
        busy_ = false;
        if (const LayerPtr l = activeLayer()) {
            l->setOpacity(static_cast<f32>(v) / 100.0f);
            markDirty();
        }
    });
    connect(opacitySpin_, &QSpinBox::valueChanged, this, [this](int v) {
        if (busy_) return;
        opacity_->setValue(v);
    });
    connect(lock_, &QToolButton::toggled, this, [this](bool on) {
        if (busy_) return;
        if (const LayerPtr l = activeLayer()) {
            l->setLocked(on);
            if (QListWidgetItem* item = list_->currentItem()) {
                busy_ = true;
                item->setData(kLockedRole, on);
                busy_ = false;
            }
        }
    });
    connect(alphaLock_, &QToolButton::toggled, this, [this](bool on) {
        if (busy_) return;
        if (const LayerPtr l = activeLayer()) {
            l->setAlphaLocked(on);
            if (QListWidgetItem* item = list_->currentItem()) {
                busy_ = true;
                item->setData(kAlphaRole, on);
                busy_ = false;
            }
        }
    });
}

void LayerPanel::setDocument(app::Document* doc) {
    doc_ = doc;
    refresh();
}

LayerPtr LayerPanel::activeLayer() const {
    if (doc_ == nullptr) return nullptr;
    return doc_->layers().find(doc_->layers().activeLayer());
}

void LayerPanel::markDirty() {
    if (doc_ != nullptr) {
        doc_->markDirty();
    }
    Q_EMIT layersChanged();
}

QPixmap LayerPanel::thumbnailFor(const Layer& l) const {
    const qreal dpr = devicePixelRatioF();
    if (doc_ == nullptr || l.tiles() == nullptr) {
        return checkerThumb(QImage(), dpr);
    }
    std::vector<u8> px;
    Rect area{};
    const Result<void> r = doc_->exportLayerPixels(l.id(), px, area);
    if (!r.ok() || area.isEmpty()) {
        return checkerThumb(QImage(), dpr);
    }
    // 레이어 내용을 캔버스 전체 비율의 썸네일에 놓는다(내용 경계만 잘라 보여 주면 위치를 모른다).
    const Size cs = doc_->canvasSize();
    QImage full(cs.width, cs.height, QImage::Format_RGBA8888);
    full.fill(Qt::transparent);
    {
        QImage part(px.data(), area.width, area.height, area.width * 4, QImage::Format_RGBA8888);
        QPainter p(&full);
        p.drawImage(area.x, area.y, part);
    }
    const QSize target = QSize(kThumbW, kThumbH) * dpr;
    return checkerThumb(full.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation), dpr);
}

void LayerPanel::fillItem(QListWidgetItem& item, const Layer& l) const {
    item.setText(QString::fromStdString(l.name()));
    item.setData(Qt::DecorationRole, QVariant::fromValue(thumbnailFor(l)));
    item.setData(kLayerIdRole, QVariant::fromValue<qulonglong>(l.id()));
    item.setData(kVisibleRole, l.visible());
    item.setData(kLockedRole, l.locked());
    item.setData(kAlphaRole, l.alphaLocked());
}

void LayerPanel::refresh() {
    busy_ = true;
    list_->clear();
    if (doc_ != nullptr) {
        const std::vector<LayerPtr>& roots = doc_->layers().roots();
        const LayerId active = doc_->layers().activeLayer();
        int activeRow = -1;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            const Layer& l = **it;
            auto* item = new QListWidgetItem(list_);
            item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
            fillItem(*item, l);
            if (l.id() == active) {
                activeRow = list_->count() - 1;
            }
        }
        if (activeRow >= 0) {
            list_->setCurrentRow(activeRow);
        }
    }
    busy_ = false;
    syncControlsToActive();
}

void LayerPanel::refreshThumbnail(LayerId id) {
    if (doc_ == nullptr) return;
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* item = list_->item(i);
        if (static_cast<LayerId>(item->data(kLayerIdRole).toULongLong()) == id) {
            if (const LayerPtr l = doc_->layers().find(id)) {
                busy_ = true;
                item->setData(Qt::DecorationRole, QVariant::fromValue(thumbnailFor(*l)));
                busy_ = false;
            }
            return;
        }
    }
}

void LayerPanel::syncControlsToActive() {
    busy_ = true;
    const LayerPtr l = activeLayer();
    const bool has = l != nullptr;
    blend_->setEnabled(has);
    opacity_->setEnabled(has);
    opacitySpin_->setEnabled(has);
    lock_->setEnabled(has);
    alphaLock_->setEnabled(has);
    if (has) {
        const int idx = blend_->findData(static_cast<int>(l->blendMode()));
        blend_->setCurrentIndex(idx < 0 ? 0 : idx);
        const int op = static_cast<int>(l->opacity() * 100.0f + 0.5f);
        opacity_->setValue(op);
        opacitySpin_->setValue(op);
        lock_->setChecked(l->locked());
        alphaLock_->setChecked(l->alphaLocked());
    }
    busy_ = false;
}

void LayerPanel::onRowChanged(int row) {
    if (busy_ || row < 0 || doc_ == nullptr) return;
    const auto id = static_cast<LayerId>(list_->item(row)->data(kLayerIdRole).toULongLong());
    (void)doc_->layers().setActiveLayer(id);
    syncControlsToActive();
    Q_EMIT activeLayerChanged(id);
}

void LayerPanel::onItemChanged(QListWidgetItem* item) {
    if (busy_ || item == nullptr || doc_ == nullptr) return;
    const auto id = static_cast<LayerId>(item->data(kLayerIdRole).toULongLong());
    const LayerPtr l = doc_->layers().find(id);
    if (l == nullptr) return;
    const std::string name = item->text().toStdString();
    if (!name.empty() && l->name() != name) {
        l->setName(name);
        doc_->markDirty();
    }
}

void LayerPanel::onRowsMoved() {
    if (busy_ || doc_ == nullptr) return;
    // 목록 순서(위→아래)를 core 순서(아래→위)로 되돌려 하나씩 맞춘다.
    const int n = list_->count();
    busy_ = true;
    for (int row = 0; row < n; ++row) {
        const auto id = static_cast<LayerId>(list_->item(row)->data(kLayerIdRole).toULongLong());
        const int index = n - 1 - row;
        (void)doc_->layers().move(id, kInvalidLayerId, index);
    }
    busy_ = false;
    markDirty();
}

void LayerPanel::addLayer() {
    if (doc_ == nullptr) return;
    const std::string name = "레이어 " + std::to_string(doc_->layers().roots().size() + 1);
    const LayerPtr active = activeLayer();
    // 활성 레이어 바로 위에 넣는다(페인팅 앱 관례).
    int index = -1;
    if (active != nullptr) {
        const auto& roots = doc_->layers().roots();
        for (usize i = 0; i < roots.size(); ++i) {
            if (roots[i]->id() == active->id()) {
                index = static_cast<int>(i) + 1;
                break;
            }
        }
    }
    const Result<LayerPtr> made = doc_->layers().addRaster(name, kInvalidLayerId, index);
    if (!made.ok()) return;
    (void)doc_->layers().setActiveLayer(made.value()->id());
    doc_->markDirty();
    refresh();
    Q_EMIT activeLayerChanged(made.value()->id());
}

void LayerPanel::duplicateLayer() {
    if (doc_ == nullptr) return;
    const LayerId id = doc_->layers().activeLayer();
    const Result<LayerPtr> dup = mari::duplicateLayer(doc_->layers(), id);
    if (!dup.ok()) return;
    (void)doc_->layers().setActiveLayer(dup.value()->id());
    refresh();
    markDirty();
    Q_EMIT activeLayerChanged(dup.value()->id());
}

void LayerPanel::removeLayer() {
    if (doc_ == nullptr || doc_->layers().roots().size() <= 1) return;
    const LayerId id = doc_->layers().activeLayer();
    if (!app::removeLayerUndoable(*doc_, id).ok()) return;
    if (!doc_->layers().roots().empty()) {
        (void)doc_->layers().setActiveLayer(doc_->layers().roots().back()->id());
    }
    refresh();
    markDirty();
    Q_EMIT activeLayerChanged(doc_->layers().activeLayer());
}

void LayerPanel::moveActive(int delta) {
    if (doc_ == nullptr) return;
    const auto& roots = doc_->layers().roots();
    const LayerId id = doc_->layers().activeLayer();
    for (usize i = 0; i < roots.size(); ++i) {
        if (roots[i]->id() == id) {
            const int target = static_cast<int>(i) + delta;
            if (target < 0 || target >= static_cast<int>(roots.size())) return;
            (void)doc_->layers().move(id, kInvalidLayerId, target);
            refresh();
            markDirty();
            return;
        }
    }
}

} // namespace mari::ui
