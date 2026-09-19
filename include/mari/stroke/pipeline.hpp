// Mari Paint — 스트로크 파이프라인 [1]~[4] + [6]용 더티 영역
//
//     [1] 정규화 → [2] 스무딩 → [3] 보간 → [4] 엔진 stamp()
//        → (합성기가 소비할) 더티 타일 목록 → [5] 합성 → [6] 표시
//
// 이 클래스는 **UI 스레드 밖**에서 돈다는 전제로 만들었다(docs/02 4절).
// 합성기는 dirtyTiles() 만 보면 된다. 전체 캔버스를 다시 그리는 경로는 여기에 없다.
//
// 🔴 핫 패스 규칙:
//   · extend()/end() 는 noexcept. 오류는 lastError() 에 남기고 획을 끊지 않는다.
//   · 버퍼는 begin() 에서 잡는다. extend() 는 할당하지 않는다.
#ifndef MARI_STROKE_PIPELINE_HPP
#define MARI_STROKE_PIPELINE_HPP

#include <mari/brush/engine.hpp>
#include <mari/stroke/input.hpp>
#include <mari/stroke/interpolate.hpp>
#include <mari/stroke/smoothing.hpp>

#include <optional>

namespace mari::stroke {

/// 파이프라인 설정. 스트로크 중간에 바꾸지 않는다.
struct StrokeConfig {
    NormalizeConfig normalize{};
    InterpolateConfig interpolate{};
    /// 떨림 보정 강도. 기본은 **끔** — 선 느낌을 말없이 바꾸지 않는다.
    SmoothingMode smoothing = SmoothingMode::Off;
    /// 데드존(캔버스 px). 보정이 켜졌을 때만 의미 있다. 0 = 없음.
    f32 deadZone = 0.0f;
    /// 끝점 보정: 펜을 뗄 때 다듬은 점을 뗀 자리까지 끌어다 마무리한다(보정이 켜졌을 때만).
    bool endCorrection = true;
    /// begin() 에서 미리 잡아 둘 더티 타일 칸 수. 핫 패스 할당을 없애기 위한 예산이다.
    usize dirtyReserve = 512;
};

/// 스트로크 하나를 처음부터 끝까지 굴린다.
/// 엔진은 소유하지 않는다 — 수명은 호출자가 책임진다.
class StrokePipeline {
public:
    /// engine 은 null 이 아니어야 한다(begin() 에서 검사한다).
    explicit StrokePipeline(brush::IBrushEngine* engine) noexcept : engine_(engine) {}

    void setConfig(const StrokeConfig& c) noexcept;
    [[nodiscard]] const StrokeConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] brush::IBrushEngine* engine() const noexcept { return engine_; }

    /// 펜이 닿았다. 여기서만 할당한다.
    [[nodiscard]] Result<void> begin(const brush::StrokeContext& ctx, const RawInputEvent& e);
    /// 펜이 움직였다. **핫 패스.**
    void extend(const RawInputEvent& e) noexcept;
    /// 에어브러시: 펜이 멈춰 있어도 마지막 자리에 스탬프 하나를 더 찍는다(보간 없이 엔진 직행).
    void holdStamp(f64 timeMs) noexcept;
    /// 펜이 떨어졌다. 마지막 이벤트가 있으면 같이 넘긴다.
    void end(const RawInputEvent& e) noexcept;
    /// 마지막 이벤트 없이 끝낸다(포커스 상실 등).
    void end() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    /// 이번 스트로크에서 건드린 타일. **중복이 제거되고 정렬되어** 있다.
    /// 합성기가 이 목록만 블렌딩한다([5]), 뷰는 이 범위만 갱신한다([6]).
    [[nodiscard]] const DirtyTiles& dirtyTiles() noexcept;
    /// 더티 타일 전체를 덮는 캔버스 좌표 경계 상자(타일 단위로 정렬되어 있다).
    /// 비어 있으면 빈 Rect.
    [[nodiscard]] Rect dirtyBounds() noexcept;
    /// 합성이 끝난 뒤 호출한다. 용량은 유지하므로 다음 스트로크도 할당하지 않는다.
    void clearDirty() noexcept;

    /// 지금까지 찍은 스탬프 수(벤치마크·회귀 감시용).
    /// 찍은 스탬프 수 — 보간기가 낸 것 + 머무르기(holdStamp)로 더 찍은 것.
    [[nodiscard]] usize stampCount() const noexcept { return interp_.emitted() + holds_; }
    /// 이번 스트로크의 출처. begin() 이 성공해야 채워진다.
    /// 🔴 읽기 전용이다 — 파이프라인에는 출처를 **바꾸는 API 가 없다**(docs/05 3.1).
    ///    Sigan 발행기가 이 값을 그대로 StrokeSample 에 실어 프레임으로 내보낸다.
    [[nodiscard]] const std::optional<StrokeSource>& source() const noexcept { return source_; }
    /// 마지막 샘플(스무딩까지 끝난 값). 커서 표시·Sigan 기록에 쓴다.
    [[nodiscard]] const InputSample& lastSample() const noexcept { return last_; }
    /// 핫 패스에서 삼킨 오류. 없으면 code()==ErrorCode::Unknown 이고 message 가 비어 있다.
    [[nodiscard]] const Error& lastError() const noexcept { return lastError_; }

private:
    class Sink;
    /// 더티 목록을 정렬·중복제거한다. 할당하지 않는다.
    void compactDirty() noexcept;

    brush::IBrushEngine* engine_ = nullptr;
    StrokeConfig cfg_{};
    InputNormalizer norm_{};
    Smoother smoother_{};
    bool endCorrection_ = true;
    StrokeInterpolator interp_{};
    DirtyTiles dirty_{};
    InputSample last_{};
    std::optional<StrokeSource> source_{}; ///< 이번 스트로크의 출처. 기본값은 "없음"이다
    Error lastError_{};
    bool active_ = false;
    usize holds_ = 0; ///< holdStamp 로 찍은 수
    bool dirtyClean_ = true; ///< dirty_ 가 정렬·중복제거된 상태인가
};

} // namespace mari::stroke

#endif // MARI_STROKE_PIPELINE_HPP
