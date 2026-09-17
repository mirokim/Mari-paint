// Mari Paint — 세션 시계 (docs/03 5.6)
#include <mari/sigan/clock.hpp>

namespace mari::sigan {
namespace {

u64 realSteadyNs() noexcept {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

i64 realWallMs() noexcept {
    const auto t = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<i64>(std::chrono::duration_cast<std::chrono::milliseconds>(t).count());
}

} // namespace

ClockSource systemClockSource() noexcept { return ClockSource{&realSteadyNs, &realWallMs}; }

SessionClock::SessionClock(ClockSource src) noexcept : src_(src) {
    if (src_.steadyNs == nullptr) {
        src_.steadyNs = &realSteadyNs;
    }
    if (src_.wallMs == nullptr) {
        src_.wallMs = &realWallMs;
    }
    steadyOriginNs_ = src_.steadyNs();
    // 벽시계는 여기서 **딱 한 번** 읽는다. 이후로는 절대 다시 보지 않는다.
    wallAnchorMs_ = src_.wallMs();
}

f64 SessionClock::elapsedMs() const noexcept {
    const u64 now = src_.steadyNs();
    // 단조 시계라 되감기지 않지만, 방어적으로 음수를 막는다.
    const u64 delta = now > steadyOriginNs_ ? now - steadyOriginNs_ : 0ull;
    return static_cast<f64>(delta) / 1.0e6;
}

} // namespace mari::sigan
