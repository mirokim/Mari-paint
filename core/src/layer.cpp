// Mari Paint — 레이어/레이어 트리 구현. 선언은 include/mari/core/layer.hpp, layer_ops.hpp.
//
// 순서 규약: **인덱스 0 = 가장 아래.** 합성은 0부터 위로 올라간다.
#include <mari/core/compositor.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/layer_ops.hpp>

#include <algorithm>
#include <unordered_map>

namespace mari {
namespace {

/// Layer 의 기본 구현. 래스터와 그룹을 한 클래스로 다룬다(그룹은 tiles_ 가 null).
class LayerImpl final : public Layer {
public:
    LayerImpl(LayerId id, LayerKind kind, std::string name)
        : id_(id), kind_(kind), name_(std::move(name)) {}

    LayerId id() const override { return id_; }
    LayerKind kind() const override { return kind_; }

    const std::string& name() const override { return name_; }
    void setName(std::string n) override { name_ = std::move(n); }

    f32 opacity() const override { return opacity_; }
    void setOpacity(f32 v) override { opacity_ = std::clamp(v, 0.0f, 1.0f); }

    BlendMode blendMode() const override { return blend_; }
    void setBlendMode(BlendMode m) override { blend_ = m; }

    bool visible() const override { return visible_; }
    void setVisible(bool v) override { visible_ = v; }

    bool locked() const override { return locked_; }
    void setLocked(bool v) override { locked_ = v; }

    bool alphaLocked() const override { return alphaLocked_; }
    void setAlphaLocked(bool v) override { alphaLocked_ = v; }

    TileMap* tiles() override { return tiles_.get(); }
    const TileMap* tiles() const override { return tiles_.get(); }

    const TileMap* mask() const override { return mask_.get(); }
    void setMask(TileMapPtr m) override { mask_ = std::move(m); }

    Rect bounds() const override {
        if (kind_ == LayerKind::Raster)
            return tiles_ ? tiles_->bounds() : Rect{};
        Rect r{};
        for (const auto& c : children_)
            r = r.united(c->bounds());
        return r;
    }

    const std::vector<LayerPtr>& children() const override { return children_; }

    // ── 구현 내부용 (인터페이스 밖) ───────────────────────────────────────
    std::vector<LayerPtr>& childrenMutable() { return children_; }
    const TileMapPtr& tilesPtr() const { return tiles_; }
    const TileMapPtr& maskPtr() const { return mask_; }
    void setTilesPtr(TileMapPtr t) { tiles_ = std::move(t); }

    /// 속성만 베낀다(자식·id 제외). 복제에 쓴다.
    void copyPropertiesFrom(const LayerImpl& o) {
        opacity_ = o.opacity_;
        blend_ = o.blend_;
        visible_ = o.visible_;
        locked_ = o.locked_;
        alphaLocked_ = o.alphaLocked_;
    }

private:
    LayerId id_;
    LayerKind kind_;
    std::string name_;
    f32 opacity_ = 1.0f;
    BlendMode blend_ = BlendMode::Normal;
    bool visible_ = true;
    bool locked_ = false;
    bool alphaLocked_ = false;
    TileMapPtr tiles_;
    TileMapPtr mask_;
    std::vector<LayerPtr> children_;
};

class LayerTreeImpl final : public LayerTree {
public:
    explicit LayerTreeImpl(Size s) : canvas_(s) {}

    Size canvasSize() const override { return canvas_; }
    void setCanvasSize(Size s) override { canvas_ = s; }

    const std::vector<LayerPtr>& roots() const override { return roots_; }

    LayerPtr find(LayerId id) const override {
        const auto it = index_.find(id);
        return it == index_.end() ? nullptr : it->second;
    }

    Result<LayerPtr> addRaster(std::string name, LayerId parent, int index) override {
        auto tiles = makeTileMap(PixelFormat::RGBA8);
        if (!tiles.ok())
            return tiles.error();
        auto layer = std::make_shared<LayerImpl>(nextId_++, LayerKind::Raster, std::move(name));
        layer->setTilesPtr(std::move(tiles).value());
        return insert(std::move(layer), parent, index);
    }

    Result<LayerPtr> addGroup(std::string name, LayerId parent, int index) override {
        auto layer = std::make_shared<LayerImpl>(nextId_++, LayerKind::Group, std::move(name));
        return insert(std::move(layer), parent, index);
    }

    Result<void> remove(LayerId id) override {
        if (id == kInvalidLayerId)
            return Err("유효하지 않은 레이어 id 다", ErrorCode::InvalidArgument);
        const auto it = index_.find(id);
        if (it == index_.end())
            return Err("그런 레이어가 없다", ErrorCode::NotFound);
        LayerPtr layer = it->second;

        std::vector<LayerPtr>& siblings = siblingsOf(id);
        const auto pos = std::find(siblings.begin(), siblings.end(), layer);
        if (pos == siblings.end())
            return Err("레이어 트리가 깨졌다", ErrorCode::Unknown);
        siblings.erase(pos);

        unindexRecursive(*layer);
        return Ok();
    }

    Result<void> move(LayerId id, LayerId newParent, int index) override {
        if (id == kInvalidLayerId)
            return Err("유효하지 않은 레이어 id 다", ErrorCode::InvalidArgument);
        const auto it = index_.find(id);
        if (it == index_.end())
            return Err("그런 레이어가 없다", ErrorCode::NotFound);
        if (id == newParent)
            return Err("자기 자신 아래로는 못 옮긴다", ErrorCode::InvalidArgument);

        LayerPtr layer = it->second;
        if (newParent != kInvalidLayerId) {
            const auto pit = index_.find(newParent);
            if (pit == index_.end())
                return Err("새 부모 레이어가 없다", ErrorCode::NotFound);
            if (pit->second->kind() != LayerKind::Group)
                return Err("그룹이 아닌 레이어 아래로는 못 옮긴다", ErrorCode::InvalidArgument);
            if (isDescendant(newParent, id))
                return Err("자기 자손 아래로는 못 옮긴다(순환)", ErrorCode::InvalidArgument);
        }

        std::vector<LayerPtr>& from = siblingsOf(id);
        const auto pos = std::find(from.begin(), from.end(), layer);
        if (pos == from.end())
            return Err("레이어 트리가 깨졌다", ErrorCode::Unknown);
        from.erase(pos);

        parents_[id] = newParent;
        std::vector<LayerPtr>& to = childListOf(newParent);
        to.insert(to.begin() + clampIndex(index, to.size()), std::move(layer));
        return Ok();
    }

    LayerId activeLayer() const override { return active_; }

    Result<void> setActiveLayer(LayerId id) override {
        if (id == kInvalidLayerId) {
            active_ = kInvalidLayerId;
            return Ok();
        }
        if (index_.find(id) == index_.end())
            return Err("그런 레이어가 없다", ErrorCode::NotFound);
        active_ = id;
        return Ok();
    }

    Result<void> flatten(const Rect& area, u8* dst, usize dstStride) const override {
        return compositeArea(*this, area, dst, dstStride);
    }

    // ── 구현 내부용 ──────────────────────────────────────────────────────
    LayerId parentOf(LayerId id) const {
        const auto it = parents_.find(id);
        return it == parents_.end() ? kInvalidLayerId : it->second;
    }

    /// 이미 만들어진 레이어를 트리에 꽂는다(복제용).
    Result<LayerPtr> attach(std::shared_ptr<LayerImpl> layer, LayerId parent, int index) {
        return insert(std::move(layer), parent, index);
    }

    LayerId takeId() { return nextId_++; }

    int indexOf(LayerId id) {
        const std::vector<LayerPtr>& sib = siblingsOf(id);
        for (usize i = 0; i < sib.size(); ++i)
            if (sib[i]->id() == id)
                return static_cast<int>(i);
        return -1;
    }

private:
    static usize clampIndex(int index, usize size) {
        if (index < 0 || static_cast<usize>(index) > size)
            return size; // 음수면 맨 위
        return static_cast<usize>(index);
    }

    std::vector<LayerPtr>& childListOf(LayerId parent) {
        if (parent == kInvalidLayerId)
            return roots_;
        auto* g = static_cast<LayerImpl*>(index_[parent].get());
        return g->childrenMutable();
    }

    std::vector<LayerPtr>& siblingsOf(LayerId id) { return childListOf(parentOf(id)); }

    bool isDescendant(LayerId candidate, LayerId ancestor) const {
        LayerId p = parentOf(candidate);
        while (p != kInvalidLayerId) {
            if (p == ancestor)
                return true;
            p = parentOf(p);
        }
        return false;
    }

    Result<LayerPtr> insert(std::shared_ptr<LayerImpl> layer, LayerId parent, int index) {
        if (parent != kInvalidLayerId) {
            const auto pit = index_.find(parent);
            if (pit == index_.end())
                return Err("부모 레이어가 없다", ErrorCode::NotFound);
            if (pit->second->kind() != LayerKind::Group)
                return Err("그룹이 아닌 레이어 아래에는 못 넣는다", ErrorCode::InvalidArgument);
        }
        LayerPtr as_base = std::move(layer);
        indexRecursive(as_base, parent);
        std::vector<LayerPtr>& list = childListOf(parent);
        list.insert(list.begin() + clampIndex(index, list.size()), as_base);
        if (active_ == kInvalidLayerId && as_base->kind() == LayerKind::Raster)
            active_ = as_base->id();
        return as_base;
    }

    void indexRecursive(const LayerPtr& layer, LayerId parent) {
        index_[layer->id()] = layer;
        parents_[layer->id()] = parent;
        for (const auto& c : layer->children())
            indexRecursive(c, layer->id());
    }

    void unindexRecursive(Layer& layer) {
        for (const auto& c : layer.children())
            unindexRecursive(*c);
        index_.erase(layer.id());
        parents_.erase(layer.id());
        if (active_ == layer.id())
            active_ = kInvalidLayerId;
    }

    Size canvas_{};
    std::vector<LayerPtr> roots_;
    std::unordered_map<LayerId, LayerPtr> index_;
    std::unordered_map<LayerId, LayerId> parents_;
    LayerId nextId_ = 1;
    LayerId active_ = kInvalidLayerId;
};

} // namespace

Result<LayerTreePtr> makeLayerTree(Size canvasSize) {
    if (canvasSize.width < 0 || canvasSize.height < 0)
        return Err("캔버스 크기가 음수다", ErrorCode::InvalidArgument);
    return LayerTreePtr{std::make_shared<LayerTreeImpl>(canvasSize)};
}

// ── layer_ops.hpp 구현 ───────────────────────────────────────────────────
namespace {

/// 레이어 한 그루를 통째로 복제한다. **픽셀은 복사하지 않는다** — snapshot() 이 O(1) COW 다.
std::shared_ptr<LayerImpl> cloneLayerTree(LayerTreeImpl& tree, const LayerImpl& src,
                                          std::string name) {
    auto copy = std::make_shared<LayerImpl>(tree.takeId(), src.kind(), std::move(name));
    copy->copyPropertiesFrom(src);
    if (src.tilesPtr())
        copy->setTilesPtr(src.tilesPtr()->snapshot());
    if (src.maskPtr())
        copy->setMask(src.maskPtr()->snapshot());
    for (const auto& child : src.children()) {
        const auto& ci = static_cast<const LayerImpl&>(*child);
        copy->childrenMutable().push_back(cloneLayerTree(tree, ci, ci.name()));
    }
    return copy;
}

} // namespace

Result<LayerPtr> duplicateLayer(LayerTree& tree, LayerId id, std::string newName) {
    auto* impl = dynamic_cast<LayerTreeImpl*>(&tree);
    if (impl == nullptr)
        return Err("이 레이어 트리 구현은 복제를 지원하지 않는다", ErrorCode::Unsupported);
    const LayerPtr src = impl->find(id);
    if (!src)
        return Err("그런 레이어가 없다", ErrorCode::NotFound);

    std::string name = newName.empty() ? (src->name() + " 복사") : std::move(newName);
    auto copy = cloneLayerTree(*impl, static_cast<const LayerImpl&>(*src), std::move(name));
    const int index = impl->indexOf(id);
    return impl->attach(std::move(copy), impl->parentOf(id), index < 0 ? -1 : index + 1);
}

Result<void> setLayerTiles(Layer& layer, TileMapPtr tiles) {
    if (layer.kind() != LayerKind::Raster)
        return Err("그룹 레이어에는 픽셀 저장소가 없다", ErrorCode::InvalidArgument);
    auto* impl = dynamic_cast<LayerImpl*>(&layer);
    if (impl == nullptr)
        return Err("이 레이어 구현은 저장소 교체를 지원하지 않는다", ErrorCode::Unsupported);
    if (!tiles)
        return Err("타일맵이 비었다", ErrorCode::InvalidArgument);
    impl->setTilesPtr(std::move(tiles));
    return Ok();
}

} // namespace mari
