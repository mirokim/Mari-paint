// Mari Paint — 세션 시계 (docs/03 5.6)
//
// 규칙: 프레임의 `t` 는 **단조 시계** 기준 상대 ms 다.
//       벽시계는 **세션 시작 시 한 번만** 앵커로 잡는다.
// 이유: 그리는 중 NTP 동기화가 벽시계를 점프시켜도 상대 시각이 오염되면 안 된다.
#ifndef MARI_SIGAN_CLOCK_HPP
#define MARI_SIGAN_CLOCK_HPP

#include <mari/core/types.hpp>

#include <chrono>

namespace mari::sigan {

/// 시계 소스. 테스트가 갈아끼울 수 있게 함수 포인터로 둔다.
/// `steadyNs` 는 단조여야 하고, `wallMs` 는 점프해도 된다(그래서 앵커로만 쓴다).
struct ClockSource {
    u64 (*steadyNs)() noexcept = nullptr;
    i64 (*wallMs)() noexcept = nullptr;
};

/// 실제 시스템 시계.
ClockSource systemClockSource() noexcept;

/// 세션 시계. 생성 시점이 t=0 이고, 벽시계 앵커는 그때 한 번만 읽는다.
class SessionClock {
public:
    /// 기본 생성 = 시스템 시계로 지금 세션을 연다.
    SessionClock() : SessionClock(systemClockSource()) {}
    explicit SessionClock(ClockSource src) noexcept;

    /// 세션 시작 기준 상대 ms. 단조 — 벽시계가 어떻게 바뀌든 영향받지 않는다.
    [[nodiscard]] f64 elapsedMs() const noexcept;

    /// 세션 시작 순간의 벽시계(Unix epoch ms). **다시 읽지 않는다.**
    [[nodiscard]] i64 wallAnchorUnixMs() const noexcept { return wallAnchorMs_; }

    /// 앵커 기준 단조 시작점(ns). 디버깅·재현용.
    [[nodiscard]] u64 steadyOriginNs() const noexcept { return steadyOriginNs_; }

private:
    ClockSource src_{};
    u64 steadyOriginNs_ = 0;
    i64 wallAnchorMs_ = 0;
};

} // namespace mari::sigan

#endif // MARI_SIGAN_CLOCK_HPP
