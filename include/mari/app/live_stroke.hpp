// Mari Paint — 실시간 획: 이벤트가 **시간을 두고** 도착하는 획 하나를 처음부터 끝까지
//
// 사람 펜(WM_POINTER)은 점을 한꺼번에 주지 않는다. down 이 오고, move 가 수십 번 오고,
// 언젠가 up 이 온다. 그 사이 화면은 계속 갱신되어야 하고, 실행취소는 **칠하기 전** 상태를
// 담아야 한다. 이 클래스는 그 순서를 지키면서 agent-api 의 `stroke` 연산(agent/ops_draw.cpp)과
// **같은 부품**을 같은 순서로 굴린다:
//
//     엔진 준비 → 실행취소 캡처(칠하기 전) → 파이프라인 [1]~[4] → StrokeEntry 로 기록
//     → finish(바뀐 타일 수) → 기록이 고장 났으면 되돌린다(docs/06 결정 ④)
//
// 🔴 발행 코드는 여기에 **없다.** 기록은 `app::StrokeEntry` 하나가 한다(docs/06 결정 ③).
//    이 클래스는 그 입구에 정규화된 샘플을 넘길 뿐이다. 출처(`StrokeSource`)도 만들지 않고
//    받기만 한다 — 사람 펜이면 호출자가 `StrokeSource::humanPen()` 을 넘긴다.
//
// 🔴 실행취소 규약: 타일은 COW 라 "칠하기 전" 스냅샷은 포인터 하나다. 그래서 **넉넉히**
//    잡는다 — 매 이벤트마다 직전 점과 현재 점의 경계 상자에 붓 여유(strokeMargin)를 더해
//    캡처한다. 모자라면 실행취소가 거짓말을 하므로 끝날 때 대조해서 `undoComplete` 로 알린다.
//
// 스레드: 한 스트로크는 한 스레드가 굴린다. 화면 갱신은 호출자가 `takeDisplayDirty()` 로
// 가져가서 한다 — 이 클래스는 화면을 모른다.
#ifndef MARI_APP_LIVE_STROKE_HPP
#define MARI_APP_LIVE_STROKE_HPP

#include <mari/app/document.hpp>
#include <mari/app/stroke_entry.hpp>
#include <mari/brush/engine.hpp>
#include <mari/brush/preset.hpp>
#include <mari/core/origin.hpp>
#include <mari/core/undo.hpp>
#include <mari/stroke/pipeline.hpp>

#include <memory>
#include <string>

namespace mari::app {

/// 스탬프가 점에서 벗어날 수 있는 최대 거리(px). 실행취소 범위를 잡는 데 쓴다.
/// **넉넉하게** 잡는다 — 모자라면 실행취소가 거짓말을 한다.
[[nodiscard]] f32 strokeMargin(const brush::MariBrushPreset& p) noexcept;

/// 사각형이 덮는 타일 좌표를 전부 `out` 에 덧붙인다(중복 제거 안 함).
void tilesForRect(const Rect& r, DirtyTiles& out);

/// 획 하나의 설정. begin() 뒤에는 바꾸지 않는다.
struct LiveStrokeConfig {
    brush::MariBrushPreset preset;
    BrushId brushId = kInvalidBrushId; ///< 기록에 실리는 붓 id
    Color color{};
    Color background{255, 255, 255, 255}; ///< 색 변화(전경↔배경)가 쓴다
    bool eraser = false;
    u64 seed = 0;
    stroke::SmoothingMode smoothing = stroke::SmoothingMode::Off;
    f32 deadZone = 0.0f;        ///< 캔버스 px. 보정이 켜졌을 때만
    bool endCorrection = true;  ///< 펜을 뗀 자리까지 이어 그린다
    std::string undoText = "획";
};

/// 획이 끝난 뒤의 결과. 숨기는 것이 없다 — 기록됐는지, 되돌렸는지 전부 적는다.
struct LiveStrokeOutcome {
    u32 changedTiles = 0;      ///< 실제로 내용이 바뀐 타일 수(축 C)
    usize dirtyTiles = 0;      ///< 엔진이 건드린 타일 수
    usize stamps = 0;
    Rect dirtyBounds{};        ///< 캔버스 좌표. 화면 갱신 범위
    bool recorded = false;     ///< 레코더가 붙어 있었나
    bool rolledBack = false;   ///< 🔴 저널 고장으로 이 획을 되돌렸다(docs/06 결정 ④)
    bool undoComplete = true;  ///< 실행취소가 칠한 범위를 전부 덮었나
    std::string engineNote;    ///< 핫 패스에서 삼킨 오류. 비면 없음
};

class LiveStroke {
public:
    /// 펜이 닿았다. 엔진·실행취소·기록 입구를 준비하고 첫 점을 찍는다.
    /// 🔴 출처는 인자로 받는다. 여기서 고르지 않는다.
    [[nodiscard]] static Result<std::unique_ptr<LiveStroke>>
    begin(Document& doc, const StrokeSource& src, LayerId layerId, const LiveStrokeConfig& cfg,
          const stroke::RawInputEvent& first);

    ~LiveStroke();
    LiveStroke(const LiveStroke&) = delete;
    LiveStroke& operator=(const LiveStroke&) = delete;

    /// 펜이 움직였다. **핫 패스** — 할당은 실행취소 캡처(새 타일을 처음 만날 때)뿐이다.
    void extend(const stroke::RawInputEvent& e) noexcept;

    /// 펜이 떨어졌다. `last` 가 nullptr 이면 마지막 이벤트 없이 끝낸다(포커스 상실 등).
    /// 실행취소 스택에 넣고, 기록을 마무리하고, 고장이면 되돌린다.
    /// 🔴 실패는 "저널이 고장 났고 롤백도 실패했다" 뿐이다. 그 외는 Outcome 에 적힌다.
    [[nodiscard]] Result<LiveStrokeOutcome> end(const stroke::RawInputEvent* last);

    /// 마지막으로 가져간 뒤 새로 더러워진 캔버스 영역. 화면 갱신용. 가져가면 비워진다.
    ///
    /// 정확한 이유: 한 번의 extend() 가 찍는 스탬프는 **직전 스무딩 샘플과 현재 스무딩 샘플
    /// 사이**에만 놓인다(보간 규약). 그 두 점의 경계 상자에 붓 여유를 더한 것이 그 호출이
    /// 더럽힌 영역의 상한이다. 스무딩 지연이 커도 이 계산은 흔들리지 않는다.
    [[nodiscard]] Rect takeDisplayDirty() noexcept;

    [[nodiscard]] bool active() const noexcept { return pipe_ != nullptr && pipe_->active(); }
    [[nodiscard]] LayerId layerId() const noexcept { return layerId_; }
    /// 이 획의 출처. 보고용.
    [[nodiscard]] const StrokeSource& source() const noexcept { return entry_->source(); }

private:
    LiveStroke() = default;

    /// 직전 점~현재 점의 경계 상자 + 여유만큼 "칠하기 전" 타일을 담는다.
    void captureAround(f64 x, f64 y) noexcept;
    /// 스무딩 샘플이 p0 → p1 로 움직였다. 그 사이에 찍힌 스탬프 범위를 화면 더티에 더한다.
    void noteDisplay(PointF p0, PointF p1) noexcept;
    /// 파이프라인의 마지막 샘플을 PenSample 로.
    [[nodiscard]] PenSample sampleNow() const noexcept;

    Document* doc_ = nullptr;
    LayerId layerId_ = kInvalidLayerId;
    brush::BrushEnginePtr engine_;
    std::unique_ptr<brush::StrokeContext> ctx_;
    std::unique_ptr<stroke::StrokePipeline> pipe_;
    std::unique_ptr<TileSnapshotCommand> undo_;
    std::unique_ptr<StrokeEntry> entry_;
    DirtyTiles captured_;     ///< 실행취소에 담은 타일(정렬 안 됨. 대조용)
    DirtyTiles scratch_;      ///< captureAround 작업 버퍼. 재할당하지 않는다
    Rect displayDirty_{};     ///< takeDisplayDirty() 가 가져갈 영역
    f64 lastX_ = 0.0;
    f64 lastY_ = 0.0;
    i32 margin_ = 0;
    bool ended_ = false;
};

} // namespace mari::app

#endif // MARI_APP_LIVE_STROKE_HPP
