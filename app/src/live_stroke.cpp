// Mari Paint — 실시간 획 구현 (include/mari/app/live_stroke.hpp)
//
// agent/ops_draw.cpp 의 `stroke` 연산과 **같은 부품, 같은 순서**다. 다른 점은 점이
// 한꺼번에 오지 않는다는 것뿐이고, 그 차이는 실행취소 캡처를 이벤트마다 조금씩 하는 것으로
// 흡수한다.
#include <mari/app/live_stroke.hpp>

#include <mari/stroke/native_engine.hpp>

#include <algorithm>
#include <cmath>

namespace mari::app {

namespace {

// 타일 좌표 정렬용. TileCoord 에는 operator< 가 없다(해시 키로만 쓴다).
constexpr auto kTileLess = [](const TileCoord& l, const TileCoord& r) noexcept {
    return l.ty != r.ty ? l.ty < r.ty : l.tx < r.tx;
};

} // namespace

f32 strokeMargin(const brush::MariBrushPreset& p) noexcept {
    f32 sizeMul = 1.0f;
    f32 scatter = 0.0f;
    for (const brush::DynamicLink& l : p.dynamics) {
        f32 maxY = 1.0f;
        for (const brush::CurvePoint& c : l.curve.points) {
            maxY = std::max(maxY, std::abs(c.y));
        }
        if (l.output == brush::DynamicOutput::Size) {
            sizeMul = std::max(sizeMul, maxY * std::max(1.0f, std::abs(l.amount)));
        } else if (l.output == brush::DynamicOutput::Scatter) {
            scatter += maxY * std::abs(l.amount);
        }
    }
    const f32 d = p.tip.diameter * sizeMul;
    return d * (0.5f + scatter) + static_cast<f32>(kTileSize);
}

void tilesForRect(const Rect& r, DirtyTiles& out) {
    if (r.isEmpty()) {
        return;
    }
    const i32 tx0 = tileIndexFor(r.x);
    const i32 ty0 = tileIndexFor(r.y);
    const i32 tx1 = tileIndexFor(r.right() - 1);
    const i32 ty1 = tileIndexFor(r.bottom() - 1);
    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            out.push_back(TileCoord{tx, ty});
        }
    }
}

Result<std::unique_ptr<LiveStroke>> LiveStroke::begin(Document& doc, const StrokeSource& src,
                                                      LayerId layerId,
                                                      const LiveStrokeConfig& cfg,
                                                      const stroke::RawInputEvent& first) {
    if (doc.isClosed()) {
        return Err("닫힌 문서에는 그릴 수 없다", ErrorCode::InvalidArgument);
    }
    // 🔴 기록하기로 해 놓고 못 하는 상태면 시작도 안 한다(docs/06 결정 ④).
    if (doc.recordingBroken()) {
        return Err("기록이 고장 나 있다 — 기록 없이 그리지 않는다(docs/06 결정 ④)",
                   ErrorCode::InvalidArgument);
    }
    const LayerPtr layer = doc.layers().find(layerId);
    if (layer == nullptr) {
        return Err("레이어를 찾을 수 없다", ErrorCode::NotFound);
    }
    if (layer->kind() != LayerKind::Raster || layer->tiles() == nullptr) {
        return Err("래스터 레이어에만 그릴 수 있다", ErrorCode::InvalidArgument);
    }
    if (layer->locked()) {
        return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    }

    Result<brush::BrushEnginePtr> engine = stroke::makeNativeEngine();
    if (!engine.ok()) {
        return engine.error();
    }
    brush::ImportReport report;
    const Result<void> setp = engine.value()->setPreset(cfg.preset, &report);
    if (!setp.ok()) {
        return setp.error();
    }

    std::unique_ptr<LiveStroke> s(new LiveStroke());
    s->doc_ = &doc;
    s->layerId_ = layerId;
    s->engine_ = std::move(engine).value();
    s->margin_ = static_cast<i32>(std::ceil(strokeMargin(cfg.preset)));

    // 🔴 출처는 받은 그대로. 여기서 만들지도 바꾸지도 않는다.
    s->ctx_ = std::make_unique<brush::StrokeContext>(src);
    s->ctx_->target = layer->tiles();
    s->ctx_->color = cfg.color;
    s->ctx_->eraser = cfg.eraser;
    s->ctx_->alphaLocked = layer->alphaLocked();
    s->ctx_->layerId = layer->id();
    s->ctx_->seed = cfg.seed;
    // 선택 밖에는 한 픽셀도 찍히지 않는다. 전체 선택이면 엔진이 꺼 버려 비용 0 이다.
    s->ctx_->selection = &doc.selectionMask();

    s->pipe_ = std::make_unique<stroke::StrokePipeline>(s->engine_.get());
    stroke::StrokeConfig pc;
    pc.smoothing = cfg.smoothing;
    s->pipe_->setConfig(pc);

    // 실행취소는 칠하기 **전에** 담는다(core/undo.hpp 규약).
    s->undo_ = TileSnapshotCommand::begin(cfg.undoText, layer->tiles());
    s->scratch_.reserve(64);
    s->captured_.reserve(256);
    s->lastX_ = first.x;
    s->lastY_ = first.y;
    s->captureAround(first.x, first.y);

    // 🔴 기록 입구. agent-api 와 **같은 클래스**다 — 두 번째 발행 경로를 만들지 않는다.
    s->entry_ = std::make_unique<StrokeEntry>(doc, s->ctx_->source, layer->id(), cfg.brushId,
                                              cfg.eraser);

    const Result<void> begun = s->pipe_->begin(*s->ctx_, first);
    if (!begun.ok()) {
        return begun.error();
    }
    s->noteDisplay(s->pipe_->lastSample().pos, s->pipe_->lastSample().pos);
    s->entry_->down(s->sampleNow());
    return Ok(std::move(s));
}

LiveStroke::~LiveStroke() {
    // end() 없이 버려졌다(창이 닫혔다 등). 파이프라인을 조용히 닫고 실행취소는 버린다 —
    // 이미 칠해진 픽셀은 남지만 기록은 up 없이 끊긴다. 호출자가 end() 를 부르는 게 정상이다.
    if (pipe_ != nullptr && pipe_->active()) {
        pipe_->end();
    }
}

void LiveStroke::captureAround(f64 x, f64 y) noexcept {
    const auto x0 = static_cast<i32>(std::floor(std::min(x, lastX_))) - margin_;
    const auto y0 = static_cast<i32>(std::floor(std::min(y, lastY_))) - margin_;
    const auto x1 = static_cast<i32>(std::ceil(std::max(x, lastX_))) + margin_;
    const auto y1 = static_cast<i32>(std::ceil(std::max(y, lastY_))) + margin_;
    scratch_.clear();
    tilesForRect(Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1}, scratch_);
    // captureBefore 는 이미 담은 좌표를 건너뛴다(중복 호출 안전). COW 라 값싸다.
    undo_->captureBefore(scratch_);
    captured_.insert(captured_.end(), scratch_.begin(), scratch_.end());
    lastX_ = x;
    lastY_ = y;
}

PenSample LiveStroke::sampleNow() const noexcept {
    // 프레임에 싣는 점은 **실제로 그린 점**이다 — 정규화·보정을 지난 파이프라인 샘플.
    const stroke::InputSample& in = pipe_->lastSample();
    PenSample ps;
    ps.pos = in.pos;
    ps.pressure = in.pressure;
    ps.tiltX = in.tiltX;
    ps.tiltY = in.tiltY;
    ps.rotation = in.rotationDeg;
    ps.velocity = in.velocity;
    return ps;
}

void LiveStroke::extend(const stroke::RawInputEvent& e) noexcept {
    if (ended_ || !pipe_->active()) {
        return;
    }
    captureAround(e.x, e.y);
    const PointF before = pipe_->lastSample().pos;
    pipe_->extend(e);
    noteDisplay(before, pipe_->lastSample().pos);
    entry_->move(sampleNow());
}

void LiveStroke::noteDisplay(PointF p0, PointF p1) noexcept {
    const auto x0 = static_cast<i32>(std::floor(std::min(p0.x, p1.x))) - margin_;
    const auto y0 = static_cast<i32>(std::floor(std::min(p0.y, p1.y))) - margin_;
    const auto x1 = static_cast<i32>(std::ceil(std::max(p0.x, p1.x))) + margin_;
    const auto y1 = static_cast<i32>(std::ceil(std::max(p0.y, p1.y))) + margin_;
    const Rect r{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    displayDirty_ = displayDirty_.isEmpty() ? r : displayDirty_.united(r);
}

Rect LiveStroke::takeDisplayDirty() noexcept {
    const Rect r = displayDirty_;
    displayDirty_ = Rect{};
    return r;
}

Result<LiveStrokeOutcome> LiveStroke::end(const stroke::RawInputEvent* last) {
    if (ended_) {
        return Err("이미 끝난 획이다", ErrorCode::InvalidArgument);
    }
    ended_ = true;
    const PointF before = pipe_->lastSample().pos;
    if (last != nullptr) {
        captureAround(last->x, last->y);
        pipe_->end(*last);
    } else {
        pipe_->end();
    }
    noteDisplay(before, pipe_->lastSample().pos);
    entry_->up(sampleNow());

    LiveStrokeOutcome out;
    const DirtyTiles& dirty = pipe_->dirtyTiles();
    out.dirtyTiles = dirty.size();
    out.dirtyBounds = pipe_->dirtyBounds();
    out.stamps = pipe_->stampCount();
    out.recorded = entry_->recording();
    if (!pipe_->lastError().message.empty()) {
        out.engineNote = pipe_->lastError().message;
    }

    // 실행취소 범위가 실제로 칠해진 범위를 덮었는지 대조한다. 못 덮었으면 숨기지 않는다.
    std::sort(captured_.begin(), captured_.end(), kTileLess);
    captured_.erase(std::unique(captured_.begin(), captured_.end()), captured_.end());
    for (const TileCoord& c : dirty) {
        if (!std::binary_search(captured_.begin(), captured_.end(), c, kTileLess)) {
            out.undoComplete = false;
            break;
        }
    }
    undo_->captureAfter(captured_);
    out.changedTiles = static_cast<u32>(undo_->changedTileCount());
    if (!undo_->empty()) {
        doc_->undoStack().push(std::move(undo_));
    }

    // 획이 끝났다. 저널을 flush 하고 축 A·C 를 올린다(docs/06 결정 ②).
    entry_->finish(out.changedTiles);
    if (entry_->broken()) {
        // 🔴 픽셀은 바뀌었는데 정본에 흔적이 없다 = 인증서가 거짓이 된다.
        //    방금 그린 것을 되돌린다(docs/06 결정 ④).
        out.rolledBack = true;
        if (out.changedTiles > 0) {
            const Result<void> rolled = doc_->undo();
            if (!rolled.ok()) {
                return Err(std::string("저널에 기록하지 못했고 롤백도 실패했다: ") +
                               rolled.message(),
                           ErrorCode::IoError);
            }
        }
        return Ok(out);
    }
    if (out.changedTiles > 0) {
        doc_->markDirty();
    }
    return Ok(out);
}

} // namespace mari::app
