// 스트로크 파이프라인 구현. 헤더: include/mari/stroke/pipeline.hpp
#include <mari/stroke/pipeline.hpp>

#include <algorithm>

namespace mari::stroke {

/// 보간 → 엔진 어댑터. 스택에 올려 쓴다(힙을 잡지 않는다).
class StrokePipeline::Sink final : public IStampSink {
public:
    Sink(brush::IBrushEngine* e, DirtyTiles& d) noexcept : engine_(e), dirty_(d) {}
    void onStamp(const brush::StampInput& s) noexcept override { engine_->stamp(s, dirty_); }

private:
    brush::IBrushEngine* engine_;
    DirtyTiles& dirty_;
};

void StrokePipeline::setConfig(const StrokeConfig& c) noexcept {
    cfg_ = c;
    norm_.setConfig(c.normalize);
    interp_.setConfig(c.interpolate);
    smoother_.setMode(c.smoothing);
    smoother_.setDeadZone(c.deadZone);
    endCorrection_ = c.endCorrection;
}

Result<void> StrokePipeline::begin(const brush::StrokeContext& ctx, const RawInputEvent& e) {
    if (engine_ == nullptr)
        return Err("엔진이 없다", ErrorCode::InvalidArgument);
    if (active_)
        end();

    lastError_ = Error{};
    setConfig(cfg_); // 하위 단계에 설정을 다시 밀어 넣는다

    auto r = engine_->beginStroke(ctx);
    if (!r.ok()) {
        lastError_ = r.error();
        return r;
    }

    // 핫 패스 할당을 없애기 위한 유일한 예약 지점이다.
    if (dirty_.capacity() < cfg_.dirtyReserve)
        dirty_.reserve(cfg_.dirtyReserve);

    Sink sink(engine_, dirty_);
    const InputSample s = smoother_.reset(norm_.begin(e));
    interp_.begin(s, *engine_, sink);
    last_ = s;
    source_ = ctx.source; // 출처는 문맥에서 그대로 들고 온다. 여기서 고르지 않는다
    active_ = true;
    holds_ = 0;
    dirtyClean_ = dirty_.empty();
    return Ok();
}

void StrokePipeline::extend(const RawInputEvent& e) noexcept {
    if (!active_)
        return;
    Sink sink(engine_, dirty_);
    const InputSample s = smoother_.smooth(norm_.normalize(e)); // [1] → [2]
    interp_.push(s, *engine_, sink);                            // [3] → [4]
    last_ = s;
    dirtyClean_ = false;
    // 엔진은 스탬프마다 타일을 덧붙인다 — 긴 획이면 수만 개가 쌓여 재할당이 난다.
    // 칸이 차기 전에 접어 둔다. 실제 타일 수는 훨씬 적어서 용량은 그대로 유지된다.
    if (dirty_.size() >= dirty_.capacity())
        compactDirty();
}

void StrokePipeline::holdStamp(f64 timeMs) noexcept {
    if (!active_)
        return;
    Sink sink(engine_, dirty_);
    brush::StampInput in{};
    in.pos = last_.pos;
    in.pressure = last_.pressure;
    in.tiltX = last_.tiltX;
    in.tiltY = last_.tiltY;
    in.azimuth = last_.azimuthDeg;
    in.rotation = last_.rotationDeg;
    in.velocity = 0.0f;
    in.timeMs = timeMs;
    sink.onStamp(in);
    ++holds_;
    dirtyClean_ = false;
    if (dirty_.size() >= dirty_.capacity())
        compactDirty();
}

void StrokePipeline::end(const RawInputEvent& e) noexcept {
    if (!active_)
        return;
    extend(e);
    end();
}

void StrokePipeline::end() noexcept {
    if (!active_)
        return;
    // 끝점 보정: 다듬은 점이 펜을 뗀 자리에 못 미쳤으면 거기까지 이어 그린다(직선, 뗄 때 필압).
    if (endCorrection_ && smoother_.enabled()) {
        const f32 lag = smoother_.lagPx();
        if (lag > 0.5f) {
            Sink sink(engine_, dirty_);
            const int steps = static_cast<int>(std::min(32.0f, std::max(2.0f, lag / 2.0f)));
            for (int i = 1; i <= steps; ++i) {
                const InputSample s = smoother_.catchUp(static_cast<f32>(i) / static_cast<f32>(steps));
                interp_.push(s, *engine_, sink);
                last_ = s;
            }
        }
    }
    engine_->endStroke(dirty_);
    interp_.finish();
    active_ = false;
    dirtyClean_ = false;
}

void StrokePipeline::compactDirty() noexcept {
    // 엔진은 중복을 허용하며 덧붙인다. 정리는 여기서 한다. 할당은 일어나지 않는다.
    std::sort(dirty_.begin(), dirty_.end(), [](const TileCoord& a, const TileCoord& b) {
        return a.ty != b.ty ? a.ty < b.ty : a.tx < b.tx;
    });
    dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
    dirtyClean_ = true;
}

const DirtyTiles& StrokePipeline::dirtyTiles() noexcept {
    if (!dirtyClean_)
        compactDirty();
    return dirty_;
}

Rect StrokePipeline::dirtyBounds() noexcept {
    Rect r{};
    for (const TileCoord& c : dirtyTiles())
        r = r.united(c.canvasRect());
    return r;
}

void StrokePipeline::clearDirty() noexcept {
    dirty_.clear(); // 용량은 유지된다
    dirtyClean_ = true;
}

} // namespace mari::stroke
