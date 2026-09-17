// 파이프라인 [1] 정규화 구현. 헤더: include/mari/stroke/input.hpp
#include <mari/stroke/input.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace mari::stroke {
namespace {

[[nodiscard]] f32 clampf(f32 v, f32 lo, f32 hi) noexcept { return std::clamp(v, lo, hi); }

/// 0..360 으로 접는다. NaN/무한은 0 으로 떨어뜨린다.
[[nodiscard]] f32 wrapDeg(f32 deg) noexcept {
    if (!std::isfinite(deg))
        return 0.0f;
    f32 v = std::fmod(deg, 360.0f);
    if (v < 0.0f)
        v += 360.0f;
    return v;
}

[[nodiscard]] f32 finiteOr(f32 v, f32 fallback) noexcept {
    return std::isfinite(v) ? v : fallback;
}

} // namespace

u64 monotonicNowNs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

InputSample InputNormalizer::begin(const RawInputEvent& first) noexcept {
    started_ = false;
    timeMs_ = 0.0;
    velocity_ = 0.0f;
    return normalize(first);
}

InputSample InputNormalizer::normalize(const RawInputEvent& e) noexcept {
    InputSample s{};

    s.pos.x = static_cast<f32>(finiteOr(static_cast<f32>(e.x), 0.0f));
    s.pos.y = static_cast<f32>(finiteOr(static_cast<f32>(e.y), 0.0f));

    // 필압 — 장치 단위(1023 등)를 0..1 로 접는다. 지원하지 않으면 1.0(항상 최대).
    if (e.hasPressure) {
        const f32 maxP = (e.pressureMax > 0.0f && std::isfinite(e.pressureMax)) ? e.pressureMax
                                                                                : 1.0f;
        s.pressure = clampf(finiteOr(e.pressure, 0.0f) / maxP, 0.0f, 1.0f);
    } else {
        s.pressure = 1.0f;
    }

    // 기울기 — 각도를 -1..1 로.
    if (e.hasTilt && cfg_.maxTiltDeg > 0.0f) {
        s.tiltX = clampf(finiteOr(e.tiltXDeg, 0.0f) / cfg_.maxTiltDeg, -1.0f, 1.0f);
        s.tiltY = clampf(finiteOr(e.tiltYDeg, 0.0f) / cfg_.maxTiltDeg, -1.0f, 1.0f);
    }

    s.azimuthDeg = wrapDeg(e.azimuthDeg);
    s.rotationDeg = wrapDeg(e.rotationDeg);

    // ── 시간. 여기가 NTP 방어 지점이다(docs/03 5.6) ──────────────────────
    // 절대 시각을 그대로 쓰지 않고 **잘라 낸 델타를 누적**한다. 그래서
    //   · 시계가 뒤로 점프해도 timeMs 는 절대 줄지 않는다
    //   · 앞으로 점프해도 한 이벤트가 최대 maxDeltaMs 만 먹는다
    f64 dtMs = 0.0;
    if (started_) {
        if (e.timestampNs > lastNs_)
            dtMs = static_cast<f64>(e.timestampNs - lastNs_) * 1e-6;
        dtMs = std::clamp(dtMs, 0.0, cfg_.maxDeltaMs);
        timeMs_ += dtMs;
    }
    lastNs_ = e.timestampNs;
    s.timeMs = timeMs_;

    // ── 속도 ────────────────────────────────────────────────────────────
    if (started_ && dtMs > 0.0 && cfg_.velocityRefPxPerMs > 0.0f) {
        const f32 dx = s.pos.x - lastX_;
        const f32 dy = s.pos.y - lastY_;
        const f32 dist = std::sqrt(dx * dx + dy * dy);
        const f32 raw = clampf(dist / static_cast<f32>(dtMs) / cfg_.velocityRefPxPerMs, 0.0f, 1.0f);
        const f32 a = clampf(cfg_.velocitySmoothing, 0.0f, 1.0f);
        velocity_ = velocity_ + a * (raw - velocity_);
    }
    s.velocity = velocity_;

    lastX_ = s.pos.x;
    lastY_ = s.pos.y;
    started_ = true;
    return s;
}

} // namespace mari::stroke
