// Mari Paint — 획이 기록으로 흘러가는 유일한 입구 (docs/06 결정 ①·③·④)
//
// 🔴 이 파일은 sigan 을 모른다. 아는 것은 `agent::IStrokeRecorder` 뿐이고,
//    그 뒤에 `SiganPublisher` 가 있는지 아무것도 없는지는 위에서 정한다.
#include <mari/app/stroke_entry.hpp>

#include <algorithm>

namespace mari::app {

StrokeEntry::StrokeEntry(Document& doc, const StrokeSource& src, LayerId layerId, BrushId brushId,
                         bool eraser) noexcept
    : rec_(doc.recorder()), src_(src), layerId_(layerId), brushId_(brushId), eraser_(eraser) {}

bool StrokeEntry::broken() const noexcept {
    return rec_ != nullptr && rec_->recordingBroken();
}

void StrokeEntry::emit(const PenSample& s, u32 flags) noexcept {
    if (rec_ == nullptr) {
        return; // 널 레코더. 기록하지 않기로 한 빌드다 — 그리기는 그대로 된다
    }
    agent::StrokePointRecord p;
    p.pos = s.pos;
    p.pressure = s.pressure;
    p.tiltX = s.tiltX;
    p.tiltY = s.tiltY;
    p.rotation = s.rotation;
    p.velocity = s.velocity;
    p.layerId = layerId_;
    p.brushId = brushId_;
    p.flags = eraser_ ? (flags | agent::RecordFlag::Eraser) : flags;
    // 🔴 출처는 여기서 **고르는 게 아니라 넘기는 것**이다.
    rec_->onStrokePoint(src_, p);
}

void StrokeEntry::down(const PenSample& s) noexcept {
    emit(s, agent::recordFlags(agent::RecordFlag::Down));
}

void StrokeEntry::move(const PenSample& s) noexcept {
    emit(s, agent::recordFlags(agent::RecordFlag::Move));
}

void StrokeEntry::up(const PenSample& s) noexcept {
    emit(s, agent::recordFlags(agent::RecordFlag::Up));
}

void StrokeEntry::finish(u32 changedTiles) noexcept {
    if (rec_ == nullptr || ended_) {
        return;
    }
    ended_ = true;
    rec_->onStrokeEnd(src_, changedTiles);
}

Result<void> recordRegionOp(Document& doc, const StrokeSource& src, agent::RegionOpKind kind,
                            const Rect& area, LayerId layerId, bool eraser, u32 changedTiles) {
    agent::IStrokeRecorder* rec = doc.recorder();
    if (rec == nullptr) {
        return Ok(); // 널 레코더
    }
    if (area.isEmpty()) {
        // 빈 영역을 "칠했다"고 기록하지 않는다. 일어나지 않은 일은 적지 않는다(H2).
        return Ok();
    }
    agent::RegionOpRecord op;
    op.kind = kind;
    op.area = area;
    op.layerId = layerId;
    op.changedTiles = changedTiles;
    op.eraser = eraser;
    return rec->onRegionOp(src, op);
}

} // namespace mari::app
