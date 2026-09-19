// Mari Paint — 파이프라인 [2] 스무딩: 떨림 보정
//
//     [1] 정규화 → **[2] 스무딩** → [3] 보간 → [4] 엔진 → [5] 합성 → [6] 표시
//
// docs/02 4절: "끌 수 있어야 한다. 선 느낌을 바꾼다."
//   · Off 는 **입력을 그대로 돌려준다.** 우회가 아니라 진짜 항등이다.
//   · 스무딩은 지연을 만든다(커서보다 선이 뒤처진다). 그래서 강도를 고를 수 있다.
//   · 위치만 다듬는다. 필압은 훨씬 약하게, 시간·속도는 건드리지 않는다 —
//     시간을 손대면 Sigan 정본 기록이 왜곡된다.
#ifndef MARI_STROKE_SMOOTHING_HPP
#define MARI_STROKE_SMOOTHING_HPP

#include <mari/stroke/input.hpp>

namespace mari::stroke {

/// 스무딩 강도. 값은 설정 저장에 쓰이므로 **뒤에만 더한다.**
enum class SmoothingMode : u8 {
    Off = 0, ///< 끔. 입력이 그대로 나간다(선 느낌을 바꾸지 않는다)
    Light,   ///< 약함 — 손떨림만 깎는다. 지연 거의 없음
    Medium,  ///< 보통 — 기본값 후보
    Strong,  ///< 강함 — 잉킹용. 커서보다 눈에 띄게 뒤처진다
};

/// 저장·로그용 안정 문자열.
[[nodiscard]] constexpr const char* smoothingModeName(SmoothingMode m) {
    switch (m) {
    case SmoothingMode::Off: return "off";
    case SmoothingMode::Light: return "light";
    case SmoothingMode::Medium: return "medium";
    case SmoothingMode::Strong: return "strong";
    }
    return "off";
}

/// 위치 EMA 계수(0..1). 작을수록 많이 깎고 많이 뒤처진다. Off 는 1.0(항등).
[[nodiscard]] constexpr f32 smoothingAlpha(SmoothingMode m) {
    switch (m) {
    case SmoothingMode::Off: return 1.0f;
    case SmoothingMode::Light: return 0.55f;
    case SmoothingMode::Medium: return 0.30f;
    case SmoothingMode::Strong: return 0.15f;
    }
    return 1.0f;
}

/// 파이프라인 [2]. 스트로크 하나당 하나. reset() 으로 시작한다.
class Smoother {
public:
    void setMode(SmoothingMode m) noexcept { mode_ = m; }
    [[nodiscard]] SmoothingMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool enabled() const noexcept { return mode_ != SmoothingMode::Off; }

    /// 첫 샘플로 상태를 채운다. 반환값은 first 그대로다(시작점은 못 다듬는다).
    InputSample reset(const InputSample& first) noexcept;
    /// 한 샘플을 다듬는다. Off 면 **비트 단위로 같은 값**을 그대로 돌려준다.
    [[nodiscard]] InputSample smooth(const InputSample& s) noexcept;

    /// 데드존(캔버스 px). 0 = 없음. 다듬은 점은 원본이 이 반지름 밖으로 나가야만 끌려간다
    /// ("끈에 매단 펜" — CSP 후보정·Krita stabilizer 의 방식). 느린 떨림을 완전히 죽인다.
    void setDeadZone(f32 px) noexcept { deadZone_ = px < 0.0f ? 0.0f : px; }
    [[nodiscard]] f32 deadZone() const noexcept { return deadZone_; }

    /// 끝점 보정: 펜을 뗀 자리(마지막 원본)까지 다듬은 점이 못 따라온 거리. 0 이면 보정할 게 없다.
    [[nodiscard]] f32 lagPx() const noexcept;
    /// 끝점 보정용: 다듬은 점을 마지막 원본 쪽으로 t(0..1) 만큼 옮긴 샘플을 준다(필압·시간은 마지막 원본 것).
    [[nodiscard]] InputSample catchUp(f32 t) const noexcept;
    [[nodiscard]] const InputSample& lastRaw() const noexcept { return lastRaw_; }

private:
    SmoothingMode mode_ = SmoothingMode::Off;
    InputSample state_{};
    InputSample lastRaw_{};
    f32 deadZone_ = 0.0f;
    bool started_ = false;
};

} // namespace mari::stroke

#endif // MARI_STROKE_SMOOTHING_HPP
