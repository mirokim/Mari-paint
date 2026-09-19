// 파이프라인 [2] 스무딩 구현. 헤더: include/mari/stroke/smoothing.hpp
#include <mari/stroke/smoothing.hpp>

#include <cmath>

namespace mari::stroke {

InputSample Smoother::reset(const InputSample& first) noexcept {
    state_ = first;
    lastRaw_ = first;
    started_ = true;
    return first;
}

f32 Smoother::lagPx() const noexcept {
    if (mode_ == SmoothingMode::Off || !started_)
        return 0.0f;
    const f32 dx = lastRaw_.pos.x - state_.pos.x, dy = lastRaw_.pos.y - state_.pos.y;
    return std::sqrt(dx * dx + dy * dy);
}

InputSample Smoother::catchUp(f32 t) const noexcept {
    InputSample out = lastRaw_;
    out.pos.x = state_.pos.x + (lastRaw_.pos.x - state_.pos.x) * t;
    out.pos.y = state_.pos.y + (lastRaw_.pos.y - state_.pos.y) * t;
    return out;
}

InputSample Smoother::smooth(const InputSample& s) noexcept {
    // 끔 = 항등. 값도, 상태도 건드리지 않는다.
    if (mode_ == SmoothingMode::Off) {
        state_ = s;
        lastRaw_ = s;
        started_ = true;
        return s;
    }
    if (!started_)
        return reset(s);
    lastRaw_ = s;

    // 데드존: 원본이 반지름 안에 있으면 위치는 그대로(필압 등만 다듬는다). 밖이면 반지름만큼 남기고 끌려간다.
    if (deadZone_ > 0.0f) {
        const f32 dx = s.pos.x - state_.pos.x, dy = s.pos.y - state_.pos.y;
        const f32 d = std::sqrt(dx * dx + dy * dy);
        if (d > deadZone_) {
            const f32 k = (d - deadZone_) / d;
            state_.pos.x += dx * k;
            state_.pos.y += dy * k;
        }
        const f32 ap = smoothingAlpha(mode_) + (1.0f - smoothingAlpha(mode_)) * 0.5f;
        state_.pressure += ap * (s.pressure - state_.pressure);
        state_.tiltX += ap * (s.tiltX - state_.tiltX);
        state_.tiltY += ap * (s.tiltY - state_.tiltY);
        InputSample out = s;
        out.pos = state_.pos;
        out.pressure = state_.pressure;
        out.tiltX = state_.tiltX;
        out.tiltY = state_.tiltY;
        return out;
    }

    const f32 a = smoothingAlpha(mode_);
    // 필압은 위치보다 훨씬 덜 깎는다. 필압을 뭉개면 선 끝이 뭉툭해진다.
    const f32 ap = a + (1.0f - a) * 0.5f;

    InputSample out = s; // 시간·속도·방위각은 **원본 그대로** 간다(정본 기록 보존)
    state_.pos.x += a * (s.pos.x - state_.pos.x);
    state_.pos.y += a * (s.pos.y - state_.pos.y);
    state_.pressure += ap * (s.pressure - state_.pressure);
    state_.tiltX += ap * (s.tiltX - state_.tiltX);
    state_.tiltY += ap * (s.tiltY - state_.tiltY);

    out.pos = state_.pos;
    out.pressure = state_.pressure;
    out.tiltX = state_.tiltX;
    out.tiltY = state_.tiltY;
    return out;
}

} // namespace mari::stroke
