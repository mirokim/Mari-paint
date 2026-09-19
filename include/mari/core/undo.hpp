// Mari Paint — 실행취소
//
// docs/02 3절: "실행취소 = 변경된 타일만 보관".
// 스트로크 하나가 8K 캔버스를 통째로 복사하면 "가볍다"가 무너진다.
// TileSnapshotCommand 는 손댄 타일의 **이전/이후 포인터만** 들고 있는다(픽셀 복사 없음).
//
// 쓰는 순서:
//   auto cmd = TileSnapshotCommand::begin("브러시", layer->tiles());
//   cmd->captureBefore(dirty);   // ← 실제로 칠하기 **전에** 부른다
//   ... 엔진이 칠한다 ...
//   cmd->captureAfter(dirty);    // ← 칠한 **뒤에**
//   stack.push(std::move(cmd));
#ifndef MARI_CORE_UNDO_HPP
#define MARI_CORE_UNDO_HPP

#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mari {

/// 되돌릴 수 있는 변경 하나.
class UndoCommand {
public:
    virtual ~UndoCommand() = default;

    /// UI 에 보일 이름("브러시", "레이어 삭제" 등).
    [[nodiscard]] virtual const std::string& text() const = 0;
    [[nodiscard]] virtual Result<void> undo() = 0;
    [[nodiscard]] virtual Result<void> redo() = 0;
    /// 이 명령이 건드리는 타일 좌표를 out 에 **덧붙인다**(화면 갱신용).
    virtual void affectedTiles(DirtyTiles& out) const = 0;
};

using UndoCommandPtr = std::unique_ptr<UndoCommand>;

/// 타일 스냅샷 기반 명령. 변경된 타일만 이전/이후로 들고 있는다.
/// 이전 타일 포인터를 붙잡아 두면 TileMap 의 COW 가 알아서 원본을 지켜 준다.
class TileSnapshotCommand final : public UndoCommand {
public:
    /// target 은 명령이 살아 있는 동안 유효해야 한다(레이어의 타일맵).
    [[nodiscard]] static std::unique_ptr<TileSnapshotCommand> begin(std::string text,
                                                                    TileMap* target);

    /// 칠하기 **전** 상태를 담는다. 이미 담은 좌표는 건너뛴다(중복 호출 안전).
    void captureBefore(const DirtyTiles& tiles);
    /// 칠한 **뒤** 상태를 담는다. captureBefore 로 등록된 좌표만 대상이다.
    void captureAfter(const DirtyTiles& tiles);

    /// 실제로 내용이 바뀐 타일 수. 0이면 스택에 넣을 가치가 없다.
    [[nodiscard]] usize changedTileCount() const;
    [[nodiscard]] bool empty() const { return changedTileCount() == 0; }

    [[nodiscard]] const std::string& text() const override { return text_; }
    [[nodiscard]] Result<void> undo() override;
    [[nodiscard]] Result<void> redo() override;
    void affectedTiles(DirtyTiles& out) const override;

private:
    struct Entry {
        TileCoord coord{};
        ConstTilePtr before; ///< nullptr = 그때 타일이 없었다(완전 투명)
        ConstTilePtr after;
    };

    Result<void> restore(bool toBefore);

    std::string text_;
    TileMap* target_ = nullptr;
    std::vector<Entry> entries_;
};

/// 여러 명령을 하나로 묶는다. undo 는 역순, redo 는 순서대로.
class CompoundCommand final : public UndoCommand {
public:
    explicit CompoundCommand(std::string text) : text_(std::move(text)) {}
    void add(UndoCommandPtr c) { cmds_.push_back(std::move(c)); }
    [[nodiscard]] bool empty() const { return cmds_.empty(); }
    [[nodiscard]] usize size() const { return cmds_.size(); }
    [[nodiscard]] const std::string& text() const override { return text_; }
    [[nodiscard]] Result<void> undo() override;
    [[nodiscard]] Result<void> redo() override;
    void affectedTiles(DirtyTiles& out) const override;

private:
    std::string text_;
    std::vector<UndoCommandPtr> cmds_;
};

/// undo/redo 를 뒤집는다("추가" = "분리"의 반대 같은 경우).
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

/// 실행취소 스택. 한도를 넘으면 가장 오래된 것부터 버린다.
class UndoStack {
public:
    explicit UndoStack(usize limit = 64) : limit_(limit == 0 ? 1 : limit) {}

    /// 새 명령을 쌓는다. **다시하기 스택은 버려진다.**
    void push(UndoCommandPtr cmd);

    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const { return !redo_.empty(); }
    [[nodiscard]] usize undoCount() const { return undo_.size(); }
    [[nodiscard]] usize redoCount() const { return redo_.size(); }

    /// 되돌릴/다시할 명령의 이름. 없으면 빈 문자열.
    [[nodiscard]] const std::string& undoText() const;
    [[nodiscard]] const std::string& redoText() const;

    /// 한 단계 되돌린다/다시 한다. 할 게 없으면 NotFound.
    /// dirty 가 있으면 갱신해야 할 타일 좌표를 덧붙인다.
    [[nodiscard]] Result<void> undo(DirtyTiles* dirty = nullptr);
    [[nodiscard]] Result<void> redo(DirtyTiles* dirty = nullptr);

    void clear();
    void setLimit(usize limit);
    [[nodiscard]] usize limit() const { return limit_; }

private:
    std::vector<UndoCommandPtr> undo_;
    std::vector<UndoCommandPtr> redo_;
    usize limit_;
};

} // namespace mari

#endif // MARI_CORE_UNDO_HPP
