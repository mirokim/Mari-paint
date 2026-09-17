// Mari Paint — 파이프라인 [3] 보간: 이벤트 사이를 스플라인으로 채운다
//
//     [1] 정규화 → [2] 스무딩 → **[3] 보간** → [4] 엔진 → [5] 합성 → [6] 표시
//
// 펜 이벤트는 초당 200개쯤 오고 그 사이는 비어 있다. 그대로 찍으면 점선이 된다.
// 구간을 **중심 파라미터화 Catmull-Rom**(alpha=0.5)으로 채운다. 균등 파라미터화와 달리
// 급커브에서 고리(cusp)가 생기지 않는다.
//
// 지연 규칙: 새 샘플이 오면 **그 즉시** 직전 구간을 그린다. 다음 샘플을 기다리지 않는다
// (한 샘플 지연 = 5ms. 16ms 예산의 1/3이다). 뒤쪽 제어점은 외삽으로 만든다.
//
// 🔴 핫 패스다. 스탬프는 IStampSink 로 흘려보낸다 — 벡터를 만들지 않는다.
#ifndef MARI_STROKE_INTERPOLATE_HPP
#define MARI_STROKE_INTERPOLATE_HPP

#include <mari/brush/engine.hpp>
#include <mari/stroke/input.hpp>

#include <vector>

namespace mari::stroke {

/// 보간이 만들어 낸 스탬프를 받는 곳.
/// 콜백 대신 인터페이스를 쓰는 이유: std::function 은 힙을 잡을 수 있다.
class IStampSink {
public:
    virtual ~IStampSink() = default;
    /// 스탬프 하나. **던지지 않는다.**
    virtual void onStamp(const brush::StampInput& s) noexcept = 0;
};

/// 보간 설정.
struct InterpolateConfig {
    /// 스탬프 간격의 하한(px). 엔진이 0을 줘도 무한 루프에 빠지지 않게 한다.
    f32 minSpacingPx = 0.5f;
    /// 상한(px).
    f32 maxSpacingPx = 1024.0f;
    /// DynamicInput::Fade 가 1.0 에 닿는 이동 거리(px).
    f32 fadeLengthPx = 512.0f;
    /// 구간 하나가 만들 수 있는 스탬프 수 상한. 튀는 좌표에 대한 안전장치다.
    i32 maxStampsPerSegment = 8192;
};

/// 파이프라인 [3]. 스트로크 하나당 하나.
/// 위치뿐 아니라 **필압·틸트·방위각·회전·속도·시간도 같이 보간한다.**
class StrokeInterpolator {
public:
    void setConfig(const InterpolateConfig& c) noexcept { cfg_ = c; }
    [[nodiscard]] const InterpolateConfig& config() const noexcept { return cfg_; }

    /// 스트로크 시작. 시작점에 스탬프 하나를 찍는다(펜을 대고 떼면 점 하나가 남아야 한다).
    void begin(const InputSample& first, const brush::IBrushEngine& engine,
               IStampSink& sink) noexcept;
    /// 샘플 하나를 더한다. 직전 샘플 → 이 샘플 구간을 spacing 간격으로 채워 sink 에 흘린다.
    void push(const InputSample& s, const brush::IBrushEngine& engine,
              IStampSink& sink) noexcept;
    /// 스트로크 종료. 마지막 구간은 push() 에서 이미 그렸으므로 여기선 상태만 닫는다.
    void finish() noexcept;

    /// 지금까지 이동한 스플라인 길이(px).
    [[nodiscard]] f32 traveledPx() const noexcept { return traveled_; }
    /// 지금까지 흘려보낸 스탬프 수.
    [[nodiscard]] usize emitted() const noexcept { return emitted_; }
    /// 다음 스탬프까지 남은 거리(px). 구간 경계에서도 간격이 끊기지 않게 이월된다.
    [[nodiscard]] f32 pendingPx() const noexcept { return pending_; }

private:
    void emit(const InputSample& s, IStampSink& sink) noexcept;

    InterpolateConfig cfg_{};
    InputSample prevPrev_{};
    InputSample prev_{};
    f32 traveled_ = 0.0f;
    f32 pending_ = 0.0f; ///< 다음 스탬프까지 남은 거리
    usize emitted_ = 0;
    bool started_ = false;
    /// 구간 하나를 잘게 쪼갠 점과 누적 호길이. **begin() 에서 미리 잡는다** —
    /// push() 는 resize() 만 하고 할당하지 않는다.
    std::vector<PointF> pts_;
    std::vector<f32> cum_;
};

/// 중심 파라미터화 Catmull-Rom 한 점. t 는 p1→p2 구간의 0..1.
/// 테스트와 ui/ 의 경로 미리보기가 같은 곡선을 써야 해서 공개한다.
[[nodiscard]] PointF catmullRom(const PointF& p0, const PointF& p1, const PointF& p2,
                                const PointF& p3, f32 t) noexcept;

} // namespace mari::stroke

#endif // MARI_STROKE_INTERPOLATE_HPP
