// Mari Paint — 레이어 패널 구현 (ui/layer_panel.hpp)
#include "layer_panel.hpp"

#include "icons.hpp"
#include "layer_row_delegate.hpp"

#include <mari/app/layer_commands.hpp>
#include <mari/core/layer_ops.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <vector>

namespace mari::ui {

/// 드롭 뒤처리를 알려 주는 트리. 래스터 레이어 "위에" 떨어뜨리는 건(=자식으로) 막는다.
class LayerTreeWidget final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> onDropped;

protected:
    void dropEvent(QDropEvent* e) override {
        if (dropIndicatorPosition() == QAbstractItemView::OnItem) {
            QTreeWidgetItem* target = itemAt(e->position().toPoint());
            if (target == nullptr || !target->data(0, kGroupRole).toBool()) {
                e->ignore();
                return;
            }
        }
        QTreeWidget::dropEvent(e);
        if (onDropped) onDropped();
    }
};

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
    clip_ = iconButton(this, "arrow-bar-to-down", "아래 레이어에서 클리핑 — 아래 레이어가 칠해진 곳에만 보인다");
    clip_->setCheckable(true);
    top->addWidget(clip_);
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

    tree_ = new LayerTreeWidget(this);
    tree_->setColumnCount(1);
    tree_->setHeaderHidden(true);
    tree_->setIndentation(14);
    tree_->setRootIsDecorated(true);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setDropIndicatorShown(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(true);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->onDropped = [this] { onDropped(); };
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &LayerPanel::showContextMenu);
    auto* delegate = new LayerRowDelegate(tree_);
    tree_->setItemDelegate(delegate);
    layout->addWidget(tree_, 1);
    connect(delegate, &LayerRowDelegate::visibilityToggled, this, [this](const QModelIndex& idx) {
        if (doc_ == nullptr) return;
        QTreeWidgetItem* item = tree_->itemFromIndex(idx);
        if (item == nullptr) return;
        if (const LayerPtr l = doc_->layers().find(idOf(item))) {
            l->setVisible(!l->visible());
            busy_ = true;
            item->setData(0, kVisibleRole, l->visible());
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
    btn("square-plus", "새 레이어 (Ctrl+Shift+N)", [this] { addLayer(); });
    btn("folder-plus", "새 그룹 (Ctrl+G 는 활성 레이어를 그룹으로)", [this] { addGroup(); });
    btn("copy", "레이어 복제 (Ctrl+J)", [this] { duplicateLayer(); });
    btn("arrow-merge", "아래와 병합 (Ctrl+E)", [this] { Q_EMIT mergeDownRequested(); });
    btn("mask", "레이어 마스크 추가 (선택이 있으면 선택에서)", [this] { addMask(true); });
    buttons->addSpacing(6);
    btn("arrow-up", "위로 (Ctrl+])", [this] { moveActive(+1); });
    btn("arrow-down", "아래로 (Ctrl+[)", [this] { moveActive(-1); });
    buttons->addStretch(1);
    btn("trash", "레이어 삭제", [this] { removeLayer(); });
    layout->addLayout(buttons);

    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) { onCurrentChanged(cur); });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int) { onItemChanged(it); });
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
    const auto flag = [this](QToolButton* b, int role, auto setter, bool repaint) {
        connect(b, &QToolButton::toggled, this, [this, role, setter, repaint](bool on) {
            if (busy_) return;
            if (const LayerPtr l = activeLayer()) {
                setter(*l, on);
                if (QTreeWidgetItem* item = tree_->currentItem()) {
                    busy_ = true;
                    item->setData(0, role, on);
                    busy_ = false;
                }
                if (repaint) markDirty();
            }
        });
    };
    flag(lock_, kLockedRole, [](Layer& l, bool on) { l.setLocked(on); }, false);
    flag(alphaLock_, kAlphaRole, [](Layer& l, bool on) { l.setAlphaLocked(on); }, false);
    flag(clip_, kClipRole, [](Layer& l, bool on) { l.setClipToBelow(on); }, true);
}

void LayerPanel::showContextMenu(const QPoint& pos) {
    if (doc_ == nullptr) return;
    if (QTreeWidgetItem* item = tree_->itemAt(pos)) tree_->setCurrentItem(item);
    const LayerPtr l = activeLayer();
    if (!l) return;
    const bool raster = l->kind() == LayerKind::Raster;
    QMenu menu(this);
    menu.addAction(themedIcon("square-plus", 16), "새 레이어", [this] { addLayer(); });
    menu.addAction(themedIcon("folder-plus", 16), "새 그룹", [this] { addGroup(); });
    menu.addAction(themedIcon("folder", 16), "그룹으로 묶기", [this] { groupActive(); });
    if (l->kind() == LayerKind::Group) menu.addAction("그룹 풀기", [this] { ungroupActive(); });
    menu.addAction(themedIcon("copy", 16), "복제", [this] { duplicateLayer(); });
    menu.addAction(themedIcon("arrow-merge", 16), "아래와 병합", [this] { Q_EMIT mergeDownRequested(); })
        ->setEnabled(raster);
    menu.addSeparator();
    QMenu* mask = menu.addMenu(themedIcon("mask", 16), "레이어 마스크");
    mask->setEnabled(raster);
    const bool hasMask = l->mask() != nullptr;
    const bool hasSel = !doc_->selectionMask().isAll();
    mask->addAction("전부 보이기", [this] { addMask(false); });
    mask->addAction("선택 영역 보이기", [this] { addMask(true); })->setEnabled(hasSel);
    mask->addSeparator();
    mask->addAction("선택을 마스크에 보이기", [this] { paintMaskWithSelection(true); })->setEnabled(hasSel);
    mask->addAction("선택을 마스크에서 가리기", [this] { paintMaskWithSelection(false); })->setEnabled(hasSel);
    mask->addSeparator();
    mask->addAction("마스크 적용", [this] { applyMask(); })->setEnabled(hasMask);
    mask->addAction("마스크 삭제", [this] { removeMask(); })->setEnabled(hasMask);
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
    QAction* clipAct = menu.addAction(themedIcon("arrow-bar-to-down", 16), "아래 레이어에서 클리핑");
    clipAct->setCheckable(true);
    clipAct->setChecked(l->clipToBelow());
    connect(clipAct, &QAction::toggled, this, [this, l](bool on) { l->setClipToBelow(on); refresh(); markDirty(); });
    QAction* alpha = menu.addAction(themedIcon("square-half", 16), "알파 잠금");
    alpha->setCheckable(true);
    alpha->setChecked(l->alphaLocked());
    alpha->setEnabled(raster);
    connect(alpha, &QAction::toggled, this, [this, l](bool on) { l->setAlphaLocked(on); refresh(); });
    menu.addSeparator();
    menu.addAction("이름 바꾸기", [this] { if (QTreeWidgetItem* it = tree_->currentItem()) tree_->editItem(it); });
    menu.addAction(themedIcon("stack-2", 16), "이미지 평탄화", [this] { flattenAll(); });
    menu.addAction(themedIcon("trash", 16), "삭제", [this] { removeLayer(); });
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void LayerPanel::setDocument(app::Document* doc) {
    doc_ = doc;
    refresh();
}

LayerPtr LayerPanel::activeLayer() const {
    if (doc_ == nullptr) return nullptr;
    return doc_->layers().find(doc_->layers().activeLayer());
}

LayerId LayerPanel::idOf(const QTreeWidgetItem* item) {
    return item == nullptr ? kInvalidLayerId : static_cast<LayerId>(item->data(0, kLayerIdRole).toULongLong());
}

void LayerPanel::markDirty() {
    if (doc_ != nullptr) {
        doc_->markDirty();
    }
    Q_EMIT layersChanged();
}

void LayerPanel::afterCommand(const Result<void>& r) {
    if (!r.ok()) return;
    refresh();
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

void LayerPanel::fillItem(QTreeWidgetItem& item, const Layer& l) const {
    const bool group = l.kind() == LayerKind::Group;
    item.setText(0, QString::fromStdString(l.name()));
    if (!group) item.setData(0, Qt::DecorationRole, QVariant::fromValue(thumbnailFor(l)));
    item.setData(0, kLayerIdRole, QVariant::fromValue<qulonglong>(l.id()));
    item.setData(0, kVisibleRole, l.visible());
    item.setData(0, kLockedRole, l.locked());
    item.setData(0, kAlphaRole, l.alphaLocked());
    item.setData(0, kClipRole, l.clipToBelow());
    item.setData(0, kGroupRole, group);
    item.setData(0, kMaskRole, l.mask() != nullptr);
    Qt::ItemFlags f = item.flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    // 래스터는 자식을 못 받는다(드롭 이벤트에서도 막지만 드롭 표시기부터 안 뜨게).
    f = group ? (f | Qt::ItemIsDropEnabled) : (f & ~Qt::ItemIsDropEnabled);
    item.setFlags(f);
}

void LayerPanel::buildItems(QTreeWidgetItem* parent, const std::vector<LayerPtr>& list, LayerId active,
                            QTreeWidgetItem*& activeItem) {
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const Layer& l = **it;
        auto* item = parent == nullptr ? new QTreeWidgetItem(tree_) : new QTreeWidgetItem(parent);
        fillItem(*item, l);
        if (l.id() == active) activeItem = item;
        if (l.kind() == LayerKind::Group) {
            buildItems(item, l.children(), active, activeItem);
            item->setExpanded(true);
        }
    }
}

void LayerPanel::refresh() {
    busy_ = true;
    tree_->clear();
    if (doc_ != nullptr) {
        QTreeWidgetItem* activeItem = nullptr;
        buildItems(nullptr, doc_->layers().roots(), doc_->layers().activeLayer(), activeItem);
        if (activeItem != nullptr) {
            tree_->setCurrentItem(activeItem);
            tree_->scrollToItem(activeItem);
        }
    }
    busy_ = false;
    syncControlsToActive();
}

QTreeWidgetItem* LayerPanel::itemFor(LayerId id) const {
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if (idOf(*it) == id) return *it;
    }
    return nullptr;
}

void LayerPanel::refreshThumbnail(LayerId id) {
    if (doc_ == nullptr) return;
    QTreeWidgetItem* item = itemFor(id);
    const LayerPtr l = doc_->layers().find(id);
    if (item == nullptr || l == nullptr || l->kind() == LayerKind::Group) return;
    busy_ = true;
    item->setData(0, Qt::DecorationRole, QVariant::fromValue(thumbnailFor(*l)));
    busy_ = false;
}

void LayerPanel::syncControlsToActive() {
    busy_ = true;
    const LayerPtr l = activeLayer();
    const bool has = l != nullptr;
    const bool raster = has && l->kind() == LayerKind::Raster;
    blend_->setEnabled(has);
    opacity_->setEnabled(has);
    opacitySpin_->setEnabled(has);
    lock_->setEnabled(has);
    alphaLock_->setEnabled(raster);
    clip_->setEnabled(has);
    if (has) {
        clip_->setChecked(l->clipToBelow());
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

void LayerPanel::onCurrentChanged(QTreeWidgetItem* cur) {
    if (busy_ || cur == nullptr || doc_ == nullptr) return;
    const LayerId id = idOf(cur);
    (void)doc_->layers().setActiveLayer(id);
    syncControlsToActive();
    Q_EMIT activeLayerChanged(id);
}

void LayerPanel::onItemChanged(QTreeWidgetItem* item) {
    if (busy_ || item == nullptr || doc_ == nullptr) return;
    const LayerPtr l = doc_->layers().find(idOf(item));
    if (l == nullptr) return;
    const std::string name = item->text(0).toStdString();
    if (!name.empty() && l->name() != name) {
        l->setName(name);
        doc_->markDirty();
    }
}

void LayerPanel::onDropped() {
    if (busy_ || doc_ == nullptr) return;
    LayerTree& tree = doc_->layers();
    busy_ = true;
    // 트리를 위에서 아래로 훑으며 core 순서(아래→위)로 하나씩 맞춘다. 부모 먼저.
    const std::function<void(QTreeWidgetItem*, LayerId)> walk = [&](QTreeWidgetItem* parentItem, LayerId parentId) {
        const int n = parentItem == nullptr ? tree_->topLevelItemCount() : parentItem->childCount();
        for (int row = 0; row < n; ++row) {
            QTreeWidgetItem* it = parentItem == nullptr ? tree_->topLevelItem(row) : parentItem->child(row);
            const LayerId id = idOf(it);
            (void)tree.move(id, parentId, n - 1 - row);
            if (it->data(0, kGroupRole).toBool()) walk(it, id);
        }
    };
    walk(nullptr, kInvalidLayerId);
    busy_ = false;
    refresh();
    markDirty();
}

void LayerPanel::locateActive(LayerId& parent, int& index) const {
    parent = kInvalidLayerId;
    index = -1;
    if (doc_ == nullptr) return;
    const LayerId active = doc_->layers().activeLayer();
    const std::function<bool(const std::vector<LayerPtr>&, LayerId)> find =
        [&](const std::vector<LayerPtr>& list, LayerId p) {
            for (usize i = 0; i < list.size(); ++i) {
                if (list[i]->id() == active) {
                    parent = p;
                    index = static_cast<int>(i);
                    return true;
                }
                if (list[i]->kind() == LayerKind::Group && find(list[i]->children(), list[i]->id())) return true;
            }
            return false;
        };
    (void)find(doc_->layers().roots(), kInvalidLayerId);
}

void LayerPanel::addLayer() {
    if (doc_ == nullptr) return;
    const std::string name = "레이어 " + std::to_string(doc_->layers().roots().size() + 1);
    // 활성 레이어 바로 위(같은 부모)에 넣는다(페인팅 앱 관례).
    LayerId parent = kInvalidLayerId;
    int index = -1;
    locateActive(parent, index);
    const Result<LayerPtr> made = doc_->layers().addRaster(name, parent, index < 0 ? -1 : index + 1);
    if (!made.ok()) return;
    (void)doc_->layers().setActiveLayer(made.value()->id());
    doc_->markDirty();
    refresh();
    Q_EMIT activeLayerChanged(made.value()->id());
}

void LayerPanel::addGroup() {
    if (doc_ == nullptr) return;
    LayerId parent = kInvalidLayerId;
    int index = -1;
    locateActive(parent, index);
    const Result<LayerPtr> made = doc_->layers().addGroup("그룹", parent, index < 0 ? -1 : index + 1);
    if (!made.ok()) return;
    (void)doc_->layers().setActiveLayer(made.value()->id());
    doc_->markDirty();
    refresh();
    Q_EMIT activeLayerChanged(made.value()->id());
}

void LayerPanel::groupActive() {
    if (doc_ == nullptr) return;
    const LayerPtr l = activeLayer();
    if (!l) return;
    LayerId parent = kInvalidLayerId;
    int index = -1;
    locateActive(parent, index);
    const Result<LayerPtr> made = doc_->layers().addGroup("그룹", parent, index < 0 ? -1 : index + 1);
    if (!made.ok()) return;
    (void)doc_->layers().move(l->id(), made.value()->id(), 0);
    (void)doc_->layers().setActiveLayer(l->id());
    doc_->markDirty();
    refresh();
    Q_EMIT layersChanged();
}

void LayerPanel::ungroupActive() {
    if (doc_ == nullptr) return;
    const LayerPtr g = activeLayer();
    if (!g || g->kind() != LayerKind::Group) return;
    LayerId parent = kInvalidLayerId;
    int index = -1;
    locateActive(parent, index);
    if (index < 0) return;
    const std::vector<LayerPtr> kids = g->children(); // 복사 — 옮기면서 원본이 바뀐다
    for (usize i = 0; i < kids.size(); ++i) {
        (void)doc_->layers().move(kids[i]->id(), parent, index + static_cast<int>(i));
    }
    (void)doc_->layers().remove(g->id());
    if (!kids.empty()) (void)doc_->layers().setActiveLayer(kids.back()->id());
    doc_->markDirty();
    refresh();
    Q_EMIT layersChanged();
    Q_EMIT activeLayerChanged(doc_->layers().activeLayer());
}

void LayerPanel::selectAdjacent(int delta) {
    if (doc_ == nullptr) return;
    QTreeWidgetItem* cur = tree_->currentItem();
    if (cur == nullptr) return;
    QTreeWidgetItem* next = delta > 0 ? tree_->itemAbove(cur) : tree_->itemBelow(cur);
    if (next != nullptr) tree_->setCurrentItem(next);
}

void LayerPanel::toggleClipActive() {
    if (const LayerPtr l = activeLayer()) {
        l->setClipToBelow(!l->clipToBelow());
        refresh();
        markDirty();
    }
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
    if (doc_ == nullptr) return;
    const LayerId id = doc_->layers().activeLayer();
    LayerId parent = kInvalidLayerId;
    int index = -1;
    locateActive(parent, index);
    if (parent == kInvalidLayerId && doc_->layers().roots().size() <= 1) return; // 마지막 루트는 남긴다
    if (!app::removeLayerUndoable(*doc_, id).ok()) return;
    // 같은 형제 목록에서 바로 아래(없으면 부모/맨 위)를 활성으로.
    const std::vector<LayerPtr>& sib =
        parent == kInvalidLayerId ? doc_->layers().roots() : doc_->layers().find(parent)->children();
    LayerId next = parent;
    if (!sib.empty()) next = sib[static_cast<usize>(std::max(0, std::min(index - 1, static_cast<int>(sib.size()) - 1)))]->id();
    if (next == kInvalidLayerId && !doc_->layers().roots().empty()) next = doc_->layers().roots().back()->id();
    (void)doc_->layers().setActiveLayer(next);
    refresh();
    markDirty();
    Q_EMIT activeLayerChanged(doc_->layers().activeLayer());
}

void LayerPanel::moveActive(int delta) {
    if (doc_ == nullptr) return;
    LayerId parent = kInvalidLayerId;
    int index = -1;
    locateActive(parent, index);
    if (index < 0) return;
    const std::vector<LayerPtr>& sib =
        parent == kInvalidLayerId ? doc_->layers().roots() : doc_->layers().find(parent)->children();
    const int target = index + delta;
    if (target < 0 || target >= static_cast<int>(sib.size())) return;
    (void)doc_->layers().move(doc_->layers().activeLayer(), parent, target);
    refresh();
    markDirty();
}

void LayerPanel::flattenAll() {
    if (doc_ == nullptr) return;
    const Result<LayerId> r = app::flattenAll(*doc_);
    if (!r.ok()) return;
    refresh();
    Q_EMIT layersChanged();
    Q_EMIT activeLayerChanged(r.value());
}

void LayerPanel::addMask(bool fromSelection) {
    if (doc_ == nullptr) return;
    afterCommand(app::addLayerMask(*doc_, doc_->layers().activeLayer(), fromSelection));
}
void LayerPanel::applyMask() {
    if (doc_ == nullptr) return;
    afterCommand(app::applyLayerMask(*doc_, doc_->layers().activeLayer()));
}
void LayerPanel::removeMask() {
    if (doc_ == nullptr) return;
    afterCommand(app::removeLayerMask(*doc_, doc_->layers().activeLayer()));
}
void LayerPanel::paintMaskWithSelection(bool reveal) {
    if (doc_ == nullptr) return;
    afterCommand(app::paintMaskWithSelection(*doc_, doc_->layers().activeLayer(), reveal ? u8{255} : u8{0}));
}

} // namespace mari::ui
