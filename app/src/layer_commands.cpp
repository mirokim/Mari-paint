// Mari Paint — 실행취소가 붙은 레이어 구조 연산 (include/mari/app/layer_commands.hpp)
#include <mari/app/layer_commands.hpp>

#include <mari/core/layer_ops.hpp>
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

} // namespace mari::app
