// Mari Paint — 실행취소가 붙은 레이어 구조 연산 (include/mari/app/layer_commands.hpp)
#include <mari/app/layer_commands.hpp>

#include <mari/app/stroke_entry.hpp>
#include <mari/core/layer_ops.hpp>
#include <mari/ora/image.hpp>
#include <mari/core/undo.hpp>

#include <memory>
#include <string>
#include <utility>

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

} // namespace mari::app
