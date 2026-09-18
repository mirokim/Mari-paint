// Mari Paint — 실행취소가 붙은 레이어 구조 연산 (include/mari/app/layer_commands.hpp)
#include <mari/app/layer_commands.hpp>

#include <mari/app/stroke_entry.hpp>
#include <mari/core/layer_ops.hpp>
#include <mari/ora/image.hpp>
#include <mari/core/undo.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mari::app {

namespace {

/// 공개 API 만으로 부모와 형제 인덱스를 찾는다.
bool locate(const std::vector<LayerPtr>& list, LayerId parent, LayerId id, LayerId& outParent, int& outIndex) {
    for (usize i = 0; i < list.size(); ++i) {
        if (list[i]->id() == id) {
            outParent = parent;
            outIndex = static_cast<int>(i);
            return true;
        }
        if (list[i]->kind() == LayerKind::Group &&
            locate(list[i]->children(), list[i]->id(), id, outParent, outIndex)) {
            return true;
        }
    }
    return false;
}

/// 레이어 하나를 떼어 냈다가(undo 로) 같은 id 로 되돌리는 명령. 픽셀 스냅샷을 같이 들 수 있다.
class DetachLayerCommand final : public UndoCommand {
public:
    DetachLayerCommand(std::string text, LayerTree& tree, LayerPtr layer, LayerId parent, int index,
                       std::unique_ptr<TileSnapshotCommand> pixels)
        : text_(std::move(text)), tree_(tree), layer_(std::move(layer)), parent_(parent), index_(index),
          pixels_(std::move(pixels)) {}

    [[nodiscard]] const std::string& text() const override { return text_; }

    [[nodiscard]] Result<void> undo() override {
        if (pixels_ != nullptr) {
            const Result<void> r = pixels_->undo();
            if (!r.ok()) return r;
        }
        return reattachLayer(tree_, layer_, parent_, index_);
    }
    [[nodiscard]] Result<void> redo() override {
        const Result<void> r = tree_.remove(layer_->id());
        if (!r.ok()) return r;
        if (pixels_ != nullptr) {
            return pixels_->redo();
        }
        return Ok();
    }
    void affectedTiles(DirtyTiles& out) const override {
        if (pixels_ != nullptr) pixels_->affectedTiles(out);
        if (const TileMap* t = layer_->tiles()) t->collectTiles(t->bounds(), out);
    }

private:
    std::string text_;
    LayerTree& tree_;
    LayerPtr layer_; ///< 🔴 떼어 낸 뒤에도 살아 있어야 한다 — 이 명령이 붙잡는다
    LayerId parent_;
    int index_;
    std::unique_ptr<TileSnapshotCommand> pixels_;
};

/// 여러 명령을 하나로. undo 는 역순.
class CompoundCommand final : public UndoCommand {
public:
    explicit CompoundCommand(std::string text) : text_(std::move(text)) {}
    void add(UndoCommandPtr c) { cmds_.push_back(std::move(c)); }
    [[nodiscard]] bool empty() const { return cmds_.empty(); }
    [[nodiscard]] const std::string& text() const override { return text_; }
    [[nodiscard]] Result<void> undo() override {
        for (auto it = cmds_.rbegin(); it != cmds_.rend(); ++it) {
            const Result<void> r = (*it)->undo();
            if (!r.ok()) return r;
        }
        return Ok();
    }
    [[nodiscard]] Result<void> redo() override {
        for (auto& c : cmds_) {
            const Result<void> r = c->redo();
            if (!r.ok()) return r;
        }
        return Ok();
    }
    void affectedTiles(DirtyTiles& out) const override {
        for (const auto& c : cmds_) c->affectedTiles(out);
    }

private:
    std::string text_;
    std::vector<UndoCommandPtr> cmds_;
};

/// undo/redo 를 뒤집는다(레이어 "추가" = 분리의 반대).
class InverseCommand final : public UndoCommand {
public:
    explicit InverseCommand(UndoCommandPtr inner) : inner_(std::move(inner)) {}
    [[nodiscard]] const std::string& text() const override { return inner_->text(); }
    [[nodiscard]] Result<void> undo() override { return inner_->redo(); }
    [[nodiscard]] Result<void> redo() override { return inner_->undo(); }
    void affectedTiles(DirtyTiles& out) const override { inner_->affectedTiles(out); }

private:
    UndoCommandPtr inner_;
};

/// 마스크 교체 명령(before/after 타일맵 포인터 — COW 라 값싸다).
class MaskSwapCommand final : public UndoCommand {
public:
    MaskSwapCommand(std::string text, LayerPtr layer, TileMapPtr before, TileMapPtr after)
        : text_(std::move(text)), layer_(std::move(layer)), before_(std::move(before)), after_(std::move(after)) {}
    [[nodiscard]] const std::string& text() const override { return text_; }
    [[nodiscard]] Result<void> undo() override { layer_->setMask(before_); return Ok(); }
    [[nodiscard]] Result<void> redo() override { layer_->setMask(after_); return Ok(); }
    void affectedTiles(DirtyTiles& out) const override {
        if (const TileMap* t = layer_->tiles()) t->collectTiles(t->bounds(), out);
    }

private:
    std::string text_;
    LayerPtr layer_;
    TileMapPtr before_, after_;
};

/// 트리를 깊이 우선으로 훑는다(부모 → 자식).
void collectAll(const std::vector<LayerPtr>& list, std::vector<LayerPtr>& out) {
    for (const LayerPtr& l : list) {
        out.push_back(l);
        if (l->kind() == LayerKind::Group) collectAll(l->children(), out);
    }
}

/// 마스크 타일맵을 만든다. 🔴 규약: 없는 타일 = 0 = 가림. 그래서 "전부 보임" 도 타일을 다 만든다.
Result<TileMapPtr> makeMask(const Document& doc, bool fromSelection) {
    Result<TileMapPtr> m = makeTileMap(PixelFormat::Gray8);
    if (!m.ok()) return m;
    const Size cs = doc.canvasSize();
    const SelectionMask& sel = doc.selectionMask();
    for (i32 ty = 0; ty <= tileIndexFor(cs.height - 1); ++ty) {
        for (i32 tx = 0; tx <= tileIndexFor(cs.width - 1); ++tx) {
            Result<TilePtr> t = m.value()->writable(TileCoord{tx, ty});
            if (!t.ok()) return t.error();
            u8* p = t.value()->mutablePixels();
            for (i32 y = 0; y < kTileSize; ++y) {
                for (i32 x = 0; x < kTileSize; ++x) {
                    const i32 cx = tileOrigin(tx) + x, cy = tileOrigin(ty) + y;
                    p[static_cast<usize>(y) * t.value()->stride() + static_cast<usize>(x)] =
                        fromSelection ? sel.valueAt(cx, cy) : u8{255};
                }
            }
        }
    }
    return m;
}

} // namespace

Result<LayerId> mergeLayerDown(Document& doc, LayerId upperId) {
    LayerTree& tree = doc.layers();
    const LayerPtr upper = tree.find(upperId);
    if (!upper) return Err("그런 레이어가 없다", ErrorCode::NotFound);
    LayerId parent = kInvalidLayerId;
    int index = -1;
    if (!locate(tree.roots(), kInvalidLayerId, upperId, parent, index)) {
        return Err("레이어 트리가 깨졌다", ErrorCode::Unknown);
    }
    if (index <= 0) return Err("아래에 레이어가 없다", ErrorCode::InvalidArgument);
    const std::vector<LayerPtr>& siblings = parent == kInvalidLayerId ? tree.roots() : tree.find(parent)->children();
    const LayerPtr lower = siblings[static_cast<usize>(index - 1)];
    if (lower->kind() != LayerKind::Raster || upper->kind() != LayerKind::Raster) {
        return Err("래스터 레이어끼리만 병합한다", ErrorCode::Unsupported);
    }

    // 아래 레이어에서 바뀔 타일 = 위 레이어가 가진 타일.
    DirtyTiles coords;
    if (const TileMap* ut = upper->tiles()) ut->collectTiles(ut->bounds(), coords);
    std::unique_ptr<TileSnapshotCommand> pixels = TileSnapshotCommand::begin("아래와 병합", lower->tiles());
    pixels->captureBefore(coords);

    const Result<void> merged = mergeDown(tree, upperId);
    if (!merged.ok()) return merged.error();
    pixels->captureAfter(coords);

    doc.undoStack().push(std::make_unique<DetachLayerCommand>("아래와 병합", tree, upper, parent, index,
                                                              std::move(pixels)));
    doc.markDirty();
    return Ok(lower->id());
}

Result<void> removeLayerUndoable(Document& doc, LayerId id) {
    LayerTree& tree = doc.layers();
    const LayerPtr layer = tree.find(id);
    if (!layer) return Err("그런 레이어가 없다", ErrorCode::NotFound);
    LayerId parent = kInvalidLayerId;
    int index = -1;
    if (!locate(tree.roots(), kInvalidLayerId, id, parent, index)) {
        return Err("레이어 트리가 깨졌다", ErrorCode::Unknown);
    }
    const Result<void> r = tree.remove(id);
    if (!r.ok()) return r;
    doc.undoStack().push(std::make_unique<DetachLayerCommand>("레이어 삭제", tree, layer, parent, index, nullptr));
    doc.markDirty();
    return Ok();
}

Result<u32> fillWithMask(Document& doc, const StrokeSource& src, LayerId layerId, const SelectionMask& mask,
                         Color8 color, bool eraser) {
    if (doc.recordingBroken()) {
        return Err("기록이 고장 나 있다 — 기록 없이 그리지 않는다(docs/06 결정 ④)", ErrorCode::IoError);
    }
    const LayerPtr layer = doc.layers().find(layerId);
    if (!layer || layer->tiles() == nullptr) return Err("래스터 레이어가 아니다", ErrorCode::InvalidArgument);
    if (layer->locked()) return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    const Rect area = mask.bounds().intersected(Rect{0, 0, doc.canvasSize().width, doc.canvasSize().height});
    if (area.isEmpty()) return Ok(u32{0});

    Result<ora::Image8> had = ora::readRegion(*layer->tiles(), area);
    if (!had.ok()) return had.error();
    ora::Image8 img = std::move(had).value();
    const SelectionMask& sel = doc.selectionMask();
    const bool alphaLock = layer->alphaLocked();
    for (i32 y = 0; y < area.height; ++y) {
        u8* px = img.pixels.data() + static_cast<usize>(y) * img.stride();
        for (i32 x = 0; x < area.width; ++x, px += 4) {
            const i32 cx = area.x + x, cy = area.y + y;
            u32 m = mask.valueAt(cx, cy);
            if (!sel.isAll()) m = (m * sel.valueAt(cx, cy) + 127u) / 255u;
            if (m == 0) continue;
            if (eraser) {
                px[3] = static_cast<u8>((px[3] * (255u - m) + 127u) / 255u);
                continue;
            }
            // 소스 오버(straight alpha). 알파 잠금이면 기존 알파를 유지한 채 색만 섞는다.
            const u32 sa = (static_cast<u32>(color.a) * m + 127u) / 255u;
            const u32 da = px[3];
            if (alphaLock) {
                const u32 t = sa;
                px[0] = static_cast<u8>((color.r * t + px[0] * (255u - t) + 127u) / 255u);
                px[1] = static_cast<u8>((color.g * t + px[1] * (255u - t) + 127u) / 255u);
                px[2] = static_cast<u8>((color.b * t + px[2] * (255u - t) + 127u) / 255u);
                continue;
            }
            const u32 oa = sa + (da * (255u - sa) + 127u) / 255u;
            if (oa == 0) { px[0] = px[1] = px[2] = px[3] = 0; continue; }
            const auto ch = [&](u32 s_, u32 d_) -> u8 {
                return static_cast<u8>((s_ * sa + d_ * ((da * (255u - sa) + 127u) / 255u)) / oa);
            };
            px[0] = ch(color.r, px[0]);
            px[1] = ch(color.g, px[1]);
            px[2] = ch(color.b, px[2]);
            px[3] = static_cast<u8>(oa);
        }
    }
    u32 changed = 0;
    const Result<void> w = doc.paintPixels(layerId, area, img.pixels.data(), img.pixels.size(),
                                           eraser ? "지우기" : "채우기", &changed);
    if (!w.ok()) return w.error();
    const Result<void> rec = recordRegionOp(doc, src, eraser ? agent::RegionOpKind::Erase : agent::RegionOpKind::Fill,
                                            area, layerId, eraser, changed);
    if (!rec.ok()) {
        const Result<void> rolled = doc.undo();
        if (!rolled.ok()) {
            return Err(std::string(rec.message()) + " (롤백도 실패: " + rolled.message() + ")", ErrorCode::IoError);
        }
        return rec.error();
    }
    return Ok(changed);
}

Result<LayerId> flattenAll(Document& doc) {
    LayerTree& tree = doc.layers();
    const Size cs = doc.canvasSize();
    // 1. 합성 결과를 먼저 만든다(구조를 바꾸기 전에).
    std::vector<u8> px(static_cast<usize>(cs.width) * static_cast<usize>(cs.height) * 4u);
    const Result<void> f = tree.flatten(Rect{0, 0, cs.width, cs.height}, px.data(), static_cast<usize>(cs.width) * 4u);
    if (!f.ok()) return f.error();
    struct Pos { LayerPtr layer; int index; };
    std::vector<Pos> roots;
    for (usize i = 0; i < tree.roots().size(); ++i) roots.push_back({tree.roots()[i], static_cast<int>(i)});

    auto compound = std::make_unique<CompoundCommand>("평탄화");
    // 2. 루트를 위에서부터 떼어 낸다(자식은 같이 딸려 간다). 실행취소는 역순이라 제자리에 돌아온다.
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        const Result<void> r = tree.remove(it->layer->id());
        if (!r.ok()) return r.error();
        compound->add(std::make_unique<DetachLayerCommand>("평탄화", tree, it->layer, kInvalidLayerId, it->index, nullptr));
    }
    // 3. 새 레이어에 결과를 쓴다.
    const Result<LayerPtr> made = tree.addRaster("평탄화");
    if (!made.ok()) return made.error();
    ora::Image8 img;
    img.width = cs.width;
    img.height = cs.height;
    img.pixels = std::move(px);
    const Result<void> w = ora::writeRegion(*made.value()->tiles(), Rect{0, 0, cs.width, cs.height}, img);
    if (!w.ok()) return w.error();
    compound->add(std::make_unique<InverseCommand>(
        std::make_unique<DetachLayerCommand>("평탄화", tree, made.value(), kInvalidLayerId, 0, nullptr)));
    (void)tree.setActiveLayer(made.value()->id());
    doc.undoStack().push(std::move(compound));
    doc.markDirty();
    return Ok(made.value()->id());
}

Result<void> addLayerMask(Document& doc, LayerId id, bool fromSelection) {
    const LayerPtr l = doc.layers().find(id);
    if (!l || l->kind() != LayerKind::Raster) return Err("래스터 레이어에만 마스크를 붙인다", ErrorCode::InvalidArgument);
    Result<TileMapPtr> m = makeMask(doc, fromSelection && !doc.selectionMask().isAll());
    if (!m.ok()) return m.error();
    TileMapPtr before = l->mask() ? l->mask()->snapshot() : nullptr;
    l->setMask(m.value());
    doc.undoStack().push(std::make_unique<MaskSwapCommand>("마스크 추가", l, before, m.value()));
    doc.markDirty();
    return Ok();
}

Result<void> removeLayerMask(Document& doc, LayerId id) {
    const LayerPtr l = doc.layers().find(id);
    if (!l || l->mask() == nullptr) return Err("마스크가 없다", ErrorCode::NotFound);
    TileMapPtr before = l->mask()->snapshot();
    l->setMask(nullptr);
    doc.undoStack().push(std::make_unique<MaskSwapCommand>("마스크 삭제", l, before, nullptr));
    doc.markDirty();
    return Ok();
}

Result<void> applyLayerMask(Document& doc, LayerId id) {
    const LayerPtr l = doc.layers().find(id);
    if (!l || l->mask() == nullptr || l->tiles() == nullptr) return Err("마스크가 없다", ErrorCode::NotFound);
    const TileMap* mask = l->mask();
    TileMap* tiles = l->tiles();
    DirtyTiles coords;
    tiles->collectTiles(tiles->bounds(), coords);
    auto pixels = TileSnapshotCommand::begin("마스크 적용", tiles);
    pixels->captureBefore(coords);
    for (const TileCoord& c : coords) {
        Result<TilePtr> t = tiles->writable(c);
        if (!t.ok()) return t.error();
        const ConstTilePtr mt = mask->at(c);
        u8* p = t.value()->mutablePixels();
        for (i32 y = 0; y < kTileSize; ++y) {
            for (i32 x = 0; x < kTileSize; ++x) {
                const u32 mv = mt ? mt->pixels()[static_cast<usize>(y) * mt->stride() + static_cast<usize>(x)] : 0u;
                u8* q = p + static_cast<usize>(y) * t.value()->stride() + static_cast<usize>(x) * 4u;
                q[3] = static_cast<u8>((q[3] * mv + 127u) / 255u);
            }
        }
    }
    pixels->captureAfter(coords);
    TileMapPtr before = mask->snapshot();
    l->setMask(nullptr);
    auto compound = std::make_unique<CompoundCommand>("마스크 적용");
    compound->add(std::move(pixels));
    compound->add(std::make_unique<MaskSwapCommand>("마스크 적용", l, before, nullptr));
    doc.undoStack().push(std::move(compound));
    doc.markDirty();
    return Ok();
}

Result<void> paintMaskWithSelection(Document& doc, LayerId id, u8 value) {
    const LayerPtr l = doc.layers().find(id);
    if (!l || l->kind() != LayerKind::Raster) return Err("래스터 레이어가 아니다", ErrorCode::InvalidArgument);
    TileMapPtr before = l->mask() ? l->mask()->snapshot() : nullptr;
    TileMapPtr after;
    if (before) {
        after = before->snapshot();
    } else {
        Result<TileMapPtr> m = makeMask(doc, false);
        if (!m.ok()) return m.error();
        after = m.value();
    }
    const SelectionMask& sel = doc.selectionMask();
    const Size cs = doc.canvasSize();
    for (i32 ty = 0; ty <= tileIndexFor(cs.height - 1); ++ty) {
        for (i32 tx = 0; tx <= tileIndexFor(cs.width - 1); ++tx) {
            Result<TilePtr> t = after->writable(TileCoord{tx, ty});
            if (!t.ok()) return t.error();
            u8* p = t.value()->mutablePixels();
            for (i32 y = 0; y < kTileSize; ++y) {
                for (i32 x = 0; x < kTileSize; ++x) {
                    const u32 sv = sel.valueAt(tileOrigin(tx) + x, tileOrigin(ty) + y);
                    if (sv == 0) continue;
                    u8& q = p[static_cast<usize>(y) * t.value()->stride() + static_cast<usize>(x)];
                    q = static_cast<u8>((value * sv + q * (255u - sv) + 127u) / 255u);
                }
            }
        }
    }
    l->setMask(after);
    doc.undoStack().push(std::make_unique<MaskSwapCommand>(value ? "마스크: 선택 보이기" : "마스크: 선택 가리기",
                                                           l, before, after));
    doc.markDirty();
    return Ok();
}

} // namespace mari::app
