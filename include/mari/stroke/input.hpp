// Mari Paint — 파이프라인 [1] 정규화: 장치 입력 → 장치 독립 InputSample
//
// docs/02 4절:
//     **[1] 정규화** → [2] 스무딩 → [3] 보간 → [4] 엔진 → [5] 합성 → [6] 표시
//
// 여기서 지키는 것:
//   · 좌표는 들어올 때부터 **캔버스 좌표**다(뷰 변환은 호출자가 이미 풀었다).
//   · 시간은 **단조 시계 기준 상대 ms** 다. 벽시계를 쓰지 않는다 — NTP 점프가
//     획 시간을 뒤로 돌리면 Sigan 의 정본 기록에 구멍이 난다(docs/03 5.6).
//     타임스탬프가 뒤로 가거나 말도 안 되게 벌어지면 **여기서 막는다.**
//   · 장치 단위(필압 1023 단계, 기울기 각도)를 전부 0..1 / -1..1 로 접는다.
#ifndef MARI_STROKE_INPUT_HPP
#define MARI_STROKE_INPUT_HPP

#include <mari/core/types.hpp>

namespace mari::stroke {

/// 장치가 준 날것 입력 한 건. 정규화의 입력이다.
struct RawInputEvent {
    /// 캔버스 좌표. 화면 좌표를 넣으면 안 된다.
    f64 x = 0.0;
    f64 y = 0.0;
    /// 장치 원본 필압. 범위는 pressureMax 가 정한다(Wintab 은 보통 1023).
    f32 pressure = 1.0f;
    /// 장치 필압 최대값. 0 이하면 1.0 으로 본다.
    f32 pressureMax = 1.0f;
    /// 기울기(도). -90..90. 장치가 없으면 hasTilt=false.
    f32 tiltXDeg = 0.0f;
    f32 tiltYDeg = 0.0f;
    /// 펜 방위각(도) 0..360.
    f32 azimuthDeg = 0.0f;
    /// 펜 배럴 회전(도) 0..360.
    f32 rotationDeg = 0.0f;
    /// **단조 시계** 기준 ns. monotonicNowNs() 또는 장치가 준 단조 값을 넣는다.
    u64 timestampNs = 0;
    /// 필압을 지원하지 않는 장치(마우스)면 false — 정규화가 1.0 으로 채운다.
    bool hasPressure = true;
    /// 기울기를 지원하지 않으면 false — 0 으로 채운다.
    bool hasTilt = true;
};

/// 장치 독립 샘플. 파이프라인 [2] 이후는 이것만 본다.
struct InputSample {
    /// 캔버스 좌표.
    PointF pos{};
    /// 0..1.
    f32 pressure = 1.0f;
    /// -1..1.
    f32 tiltX = 0.0f;
    f32 tiltY = 0.0f;
    /// 0..360 도.
    f32 azimuthDeg = 0.0f;
    f32 rotationDeg = 0.0f;
    /// 0..1 로 정규화한 속도(NormalizeConfig::velocityRefPxPerMs 기준).
    f32 velocity = 0.0f;
    /// 스트로크 시작 기준 경과 ms. **단조 증가가 보장된다.**
    f64 timeMs = 0.0;
};

/// 정규화 설정. 장치 특성을 여기 한 곳에 모은다.
struct NormalizeConfig {
    /// 이 각도를 ±1 로 본다.
    f32 maxTiltDeg = 60.0f;
    /// 이 속도(px/ms)를 velocity 1.0 으로 본다. 약 2px/ms = 120px/프레임.
    f32 velocityRefPxPerMs = 2.0f;
    /// 속도 EMA 계수 0..1. 클수록 새 값을 빨리 따라간다.
    f32 velocitySmoothing = 0.35f;
    /// 두 이벤트 사이 간격의 상한(ms). 이보다 크면 잘라 낸다.
    /// 시계 점프·스레드 정지가 속도를 0 으로 만들거나 시간을 날리는 것을 막는다.
    f64 maxDeltaMs = 100.0;
};

/// 파이프라인 [1]. 스트로크 하나당 하나를 쓰고 begin() 으로 초기화한다.
class InputNormalizer {
public:
    void setConfig(const NormalizeConfig& c) noexcept { cfg_ = c; }
    [[nodiscard]] const NormalizeConfig& config() const noexcept { return cfg_; }

    /// 스트로크 시작. 이 이벤트의 시각이 timeMs = 0 의 기준이 된다.
    InputSample begin(const RawInputEvent& first) noexcept;
    /// 이어지는 이벤트를 정규화한다. begin() 전에 부르면 begin() 처럼 동작한다.
    InputSample normalize(const RawInputEvent& e) noexcept;

    [[nodiscard]] bool started() const noexcept { return started_; }
    /// 마지막으로 낸 샘플의 시각(ms).
    [[nodiscard]] f64 elapsedMs() const noexcept { return timeMs_; }

private:
    NormalizeConfig cfg_{};
    u64 lastNs_ = 0;
    f64 timeMs_ = 0.0;
    f32 lastX_ = 0.0f;
    f32 lastY_ = 0.0f;
    f32 velocity_ = 0.0f;
    bool started_ = false;
};

/// 단조 시계(steady_clock) 기준 ns. 장치가 타임스탬프를 안 주면 이걸 쓴다.
/// **벽시계를 쓰지 마라** — docs/03 5.6.
[[nodiscard]] u64 monotonicNowNs() noexcept;

} // namespace mari::stroke

#endif // MARI_STROKE_INPUT_HPP
