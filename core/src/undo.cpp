// Mari Paint — 실행취소 구현. 선언은 include/mari/core/undo.hpp.
#include <mari/core/undo.hpp>

#include <algorithm>
#include <cstring>

namespace mari {
namespace {

const std::string& emptyText() {
    static const std::string s;
    return s;
}

/// 타일 하나를 타일맵에 되돌려 놓는다. tile 이 null 이면 그 좌표를 지운다.
Result<void> putTile(TileMap& map, TileCoord c, const ConstTilePtr& tile) {
    if (!tile) {
        map.erase(c);
        return Ok();
    }
    auto w = map.writable(c);
    if (!w.ok())
        return w.error();
    TilePtr dst = std::move(w).value();
    const usize rows = static_cast<usize>(kTileSize);
    const usize bytes = std::min(dst->stride(), tile->stride());
    u8* out = dst->mutablePixels();
    const u8* in = tile->pixels();
    for (usize y = 0; y < rows; ++y)
        std::memcpy(out + y * dst->stride(), in + y * tile->stride(), bytes);
    return Ok();
}

/// 두 타일의 내용이 같은지. 둘 다 null 이면 같다.
bool sameTile(const ConstTilePtr& a, const ConstTilePtr& b) {
    if (a.get() == b.get())
        return true;
    if (!a || !b)
        return false;
    if (a->stride() != b->stride())
        return false;
    return std::memcmp(a->pixels(), b->pixels(), a->stride() * static_cast<usize>(kTileSize)) == 0;
}

} // namespace

std::unique_ptr<TileSnapshotCommand> TileSnapshotCommand::begin(std::string text, TileMap* target) {
    if (target == nullptr)
        return nullptr;
    std::unique_ptr<TileSnapshotCommand> cmd(new TileSnapshotCommand());
    cmd->text_ = std::move(text);
    cmd->target_ = target;
    return cmd;
}

void TileSnapshotCommand::captureBefore(const DirtyTiles& tiles) {
    for (const TileCoord& c : tiles) {
        const bool known = std::any_of(entries_.begin(), entries_.end(),
                                       [&](const Entry& e) { return e.coord == c; });
        if (known)
            continue;
        // 지금 타일의 **사본**을 붙잡는다. clone() 은 픽셀 버퍼를 공유할 뿐이라 O(1) 이고,
        // 이후 누가 그 타일에 쓰면 COW 가 갈라서므로 여기 담긴 픽셀은 그대로 얼어붙는다.
        // (사본이 아니라 원본 객체를 잡으면 제자리 수정에 같이 오염된다.)
        const ConstTilePtr cur = target_->at(c);
        entries_.push_back(Entry{c, cur ? ConstTilePtr(cur->clone()) : nullptr, nullptr});
    }
}

void TileSnapshotCommand::captureAfter(const DirtyTiles& tiles) {
    (void)tiles; // 등록된 좌표 전부를 다시 읽는 게 안전하다(중복 호출 허용).
    for (Entry& e : entries_) {
        const ConstTilePtr cur = target_->at(e.coord);
        e.after = cur ? ConstTilePtr(cur->clone()) : nullptr;
    }
}

usize TileSnapshotCommand::changedTileCount() const {
    usize n = 0;
    for (const Entry& e : entries_)
        if (!sameTile(e.before, e.after))
            ++n;
    return n;
}

Result<void> TileSnapshotCommand::restore(bool toBefore) {
    if (target_ == nullptr)
        return Err("대상 타일맵이 사라졌다", ErrorCode::NotFound);
    for (const Entry& e : entries_) {
        auto r = putTile(*target_, e.coord, toBefore ? e.before : e.after);
        if (!r.ok())
            return r;
    }
    return Ok();
}

Result<void> TileSnapshotCommand::undo() {
    return restore(true);
}
Result<void> TileSnapshotCommand::redo() {
    return restore(false);
}

void TileSnapshotCommand::affectedTiles(DirtyTiles& out) const {
    for (const Entry& e : entries_)
        out.push_back(e.coord);
}

// ── UndoStack ────────────────────────────────────────────────────────────

void UndoStack::push(UndoCommandPtr cmd) {
    if (!cmd)
        return;
    redo_.clear(); // 새 가지가 났다 — 다시하기는 버린다
    undo_.push_back(std::move(cmd));
    while (undo_.size() > limit_)
        undo_.erase(undo_.begin());
}

const std::string& UndoStack::undoText() const {
    return undo_.empty() ? emptyText() : undo_.back()->text();
}

std::vector<std::string> UndoStack::undoTexts() const {
    std::vector<std::string> out;
    out.reserve(undo_.size());
    for (const auto& c : undo_)
        out.push_back(c->text());
    return out;
}

std::vector<std::string> UndoStack::redoTexts() const {
    std::vector<std::string> out;
    out.reserve(redo_.size());
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it)
        out.push_back((*it)->text());
    return out;
}

const std::string& UndoStack::redoText() const {
    return redo_.empty() ? emptyText() : redo_.back()->text();
}

Result<void> UndoStack::undo(DirtyTiles* dirty) {
    if (undo_.empty())
        return Err("되돌릴 게 없다", ErrorCode::NotFound);
    UndoCommandPtr cmd = std::move(undo_.back());
    undo_.pop_back();
    auto r = cmd->undo();
    if (!r.ok()) {
        undo_.push_back(std::move(cmd)); // 실패하면 스택을 원래대로 둔다
        return r;
    }
    if (dirty != nullptr)
        cmd->affectedTiles(*dirty);
    redo_.push_back(std::move(cmd));
    return Ok();
}

Result<void> UndoStack::redo(DirtyTiles* dirty) {
    if (redo_.empty())
        return Err("다시 할 게 없다", ErrorCode::NotFound);
    UndoCommandPtr cmd = std::move(redo_.back());
    redo_.pop_back();
    auto r = cmd->redo();
    if (!r.ok()) {
        redo_.push_back(std::move(cmd));
        return r;
    }
    if (dirty != nullptr)
        cmd->affectedTiles(*dirty);
    undo_.push_back(std::move(cmd));
    return Ok();
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
}

void UndoStack::setLimit(usize limit) {
    limit_ = limit == 0 ? 1 : limit;
    while (undo_.size() > limit_)
        undo_.erase(undo_.begin());
}

} // namespace mari

namespace mari {

Result<void> CompoundCommand::undo() {
    for (auto it = cmds_.rbegin(); it != cmds_.rend(); ++it) {
        const Result<void> r = (*it)->undo();
        if (!r.ok())
            return r;
    }
    return Ok();
}

Result<void> CompoundCommand::redo() {
    for (auto& c : cmds_) {
        const Result<void> r = c->redo();
        if (!r.ok())
            return r;
    }
    return Ok();
}

void CompoundCommand::affectedTiles(DirtyTiles& out) const {
    for (const auto& c : cmds_)
        c->affectedTiles(out);
}

} // namespace mari
