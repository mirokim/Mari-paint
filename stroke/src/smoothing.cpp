// 파이프라인 [2] 스무딩 구현. 헤더: include/mari/stroke/smoothing.hpp
#include <mari/stroke/smoothing.hpp>

namespace mari::stroke {

InputSample Smoother::reset(const InputSample& first) noexcept {
    state_ = first;
    started_ = true;
    return first;
}

InputSample Smoother::smooth(const InputSample& s) noexcept {
    // 끔 = 항등. 값도, 상태도 건드리지 않는다.
    if (mode_ == SmoothingMode::Off) {
        state_ = s;
        started_ = true;
        return s;
    }
    if (!started_)
        return reset(s);

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
