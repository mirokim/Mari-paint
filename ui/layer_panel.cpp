// Mari Paint — 레이어 패널 구현 (ui/layer_panel.hpp)
#include "layer_panel.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QSlider>
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

QIcon checkerIcon(const QImage& content) {
    QPixmap pm(kThumbW, kThumbH);
    QPainter p(&pm);
    p.fillRect(pm.rect(), QColor(200, 200, 200));
    for (int y = 0; y < kThumbH; y += 6) {
        for (int x = (y / 6 % 2) * 6; x < kThumbW; x += 12) {
            p.fillRect(x, y, 6, 6, QColor(160, 160, 160));
        }
    }
    if (!content.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(pm.rect(), content);
    }
    p.setPen(QColor(0, 0, 0, 120));
    p.drawRect(0, 0, kThumbW - 1, kThumbH - 1);
    return QIcon(pm);
}

} // namespace

LayerPanel::LayerPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* top = new QHBoxLayout();
    blend_ = new QComboBox(this);
    for (const BlendMode m : kBlendModes) {
        blend_->addItem(QString::fromLatin1(blendModeName(m)), static_cast<int>(m));
    }
    top->addWidget(blend_, 1);
    lock_ = new QToolButton(this);
    lock_->setText("🔒");
    lock_->setCheckable(true);
    lock_->setToolTip("잠금 — 잠긴 레이어에는 그려지지 않는다");
    alphaLock_ = new QToolButton(this);
    alphaLock_->setText("α");
    alphaLock_->setCheckable(true);
    alphaLock_->setToolTip("알파 잠금 — 이미 칠한 자리에만 칠해진다");
    top->addWidget(lock_);
    top->addWidget(alphaLock_);
    layout->addLayout(top);

    auto* opRow = new QHBoxLayout();
    opRow->addWidget(new QLabel("불투명", this));
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(0, 100);
    opacity_->setValue(100);
    opRow->addWidget(opacity_, 1);
    layout->addLayout(opRow);

    list_ = new QListWidget(this);
    list_->setIconSize(QSize(kThumbW, kThumbH));
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setDefaultDropAction(Qt::MoveAction);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_, 1);

    auto* buttons = new QHBoxLayout();
    const auto btn = [&](const char* text, const char* tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        connect(b, &QToolButton::clicked, this, fn);
        buttons->addWidget(b);
        return b;
    };
    btn("+", "새 레이어 (Insert)", [this] { addLayer(); });
    btn("▲", "위로", [this] { moveActive(+1); });
    btn("▼", "아래로", [this] { moveActive(-1); });
    btn("−", "레이어 삭제", [this] { removeLayer(); });
    buttons->addStretch(1);
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
        if (const LayerPtr l = activeLayer()) {
            l->setOpacity(static_cast<f32>(v) / 100.0f);
            markDirty();
        }
    });
    connect(lock_, &QToolButton::toggled, this, [this](bool on) {
        if (busy_) return;
        if (const LayerPtr l = activeLayer()) {
            l->setLocked(on);
        }
    });
    connect(alphaLock_, &QToolButton::toggled, this, [this](bool on) {
        if (busy_) return;
        if (const LayerPtr l = activeLayer()) {
            l->setAlphaLocked(on);
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
    emit layersChanged();
}

QIcon LayerPanel::thumbnailFor(const Layer& l) const {
    if (doc_ == nullptr || l.tiles() == nullptr) {
        return checkerIcon(QImage());
    }
    std::vector<u8> px;
    Rect area{};
    const Result<void> r = doc_->exportLayerPixels(l.id(), px, area);
    if (!r.ok() || area.isEmpty()) {
        return checkerIcon(QImage());
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
    return checkerIcon(full.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
            auto* item = new QListWidgetItem(thumbnailFor(l), QString::fromStdString(l.name()), list_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable |
                           Qt::ItemIsDragEnabled);
            item->setCheckState(l.visible() ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(l.id()));
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
        if (static_cast<LayerId>(item->data(Qt::UserRole).toULongLong()) == id) {
            if (const LayerPtr l = doc_->layers().find(id)) {
                busy_ = true;
                item->setIcon(thumbnailFor(*l));
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
    lock_->setEnabled(has);
    alphaLock_->setEnabled(has);
    if (has) {
        const int idx = blend_->findData(static_cast<int>(l->blendMode()));
        blend_->setCurrentIndex(idx < 0 ? 0 : idx);
        opacity_->setValue(static_cast<int>(l->opacity() * 100.0f + 0.5f));
        lock_->setChecked(l->locked());
        alphaLock_->setChecked(l->alphaLocked());
    }
    busy_ = false;
}

void LayerPanel::onRowChanged(int row) {
    if (busy_ || row < 0 || doc_ == nullptr) return;
    const auto id = static_cast<LayerId>(list_->item(row)->data(Qt::UserRole).toULongLong());
    (void)doc_->layers().setActiveLayer(id);
    syncControlsToActive();
    emit activeLayerChanged(id);
}

void LayerPanel::onItemChanged(QListWidgetItem* item) {
    if (busy_ || item == nullptr || doc_ == nullptr) return;
    const auto id = static_cast<LayerId>(item->data(Qt::UserRole).toULongLong());
    const LayerPtr l = doc_->layers().find(id);
    if (l == nullptr) return;
    const bool vis = item->checkState() == Qt::Checked;
    const std::string name = item->text().toStdString();
    bool pixels = false;
    if (l->visible() != vis) {
        l->setVisible(vis);
        pixels = true;
    }
    if (!name.empty() && l->name() != name) {
        l->setName(name);
        doc_->markDirty();
    }
    if (pixels) {
        markDirty();
    }
}

void LayerPanel::onRowsMoved() {
    if (busy_ || doc_ == nullptr) return;
    // 목록 순서(위→아래)를 core 순서(아래→위)로 되돌려 하나씩 맞춘다.
    const int n = list_->count();
    busy_ = true;
    for (int row = 0; row < n; ++row) {
        const auto id = static_cast<LayerId>(list_->item(row)->data(Qt::UserRole).toULongLong());
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
    emit activeLayerChanged(made.value()->id());
}

void LayerPanel::removeLayer() {
    if (doc_ == nullptr || doc_->layers().roots().size() <= 1) return;
    const LayerId id = doc_->layers().activeLayer();
    if (!doc_->layers().remove(id).ok()) return;
    if (!doc_->layers().roots().empty()) {
        (void)doc_->layers().setActiveLayer(doc_->layers().roots().back()->id());
    }
    refresh();
    markDirty();
    emit activeLayerChanged(doc_->layers().activeLayer());
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
