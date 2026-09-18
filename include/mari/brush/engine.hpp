// Mari Paint — IBrushEngine: 브러시 엔진 인터페이스
//
// docs/02 4절 스트로크 파이프라인의 [4]단계다.
//     정규화 → 스무딩 → 보간 → **엔진 stamp()** → 합성 → 표시
//
// 🔴 핫 패스다. 규칙:
//   · stamp() 는 **예외를 던지지 않는다**(noexcept). 오류는 DirtyTiles 를 비우고 돌려준다.
//   · stamp() 안에서 **힙 할당을 하지 않는다.** 필요한 버퍼는 beginStroke() 에서 잡는다.
//   · 더티 타일은 호출자가 넘긴 벡터에 **덧붙인다.** 매번 새 벡터를 만들지 않는다.
//   · UI 스레드가 아닌 스트로크 스레드에서 불린다. 엔진 인스턴스는 스레드 하나가 독점한다.
#ifndef MARI_BRUSH_ENGINE_HPP
#define MARI_BRUSH_ENGINE_HPP

#include <mari/brush/preset.hpp>
#include <mari/core/origin.hpp>
#include <mari/core/result.hpp>
#include <mari/core/selection.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

#include <memory>
#include <string>

namespace mari::brush {

/// 한 스탬프의 입력. 파이프라인 [1]~[3]이 만들어 엔진에 넘긴다.
/// 좌표는 **캔버스 좌표**(좌상단 원점, y 아래로 증가)다. 화면 좌표가 아니다.
struct StampInput {
    /// 캔버스 좌표. 서브픽셀 위치를 그대로 유지한다.
    PointF pos;
    /// 필압 0..1. 장치가 못 주면 1.0.
    f32 pressure = 1.0f;
    /// 기울기 -1..1.
    f32 tiltX = 0.0f;
    f32 tiltY = 0.0f;
    /// 펜 방위각(도) 0..360. 없으면 0.
    f32 azimuth = 0.0f;
    /// 펜 회전(도) 0..360. 없으면 0.
    f32 rotation = 0.0f;
    /// 정규화된 속도 0..1.
    f32 velocity = 0.0f;
    /// 스트로크 시작 기준 경과 ms. **단조 시계** 기준이다(docs/03 5.6).
    f64 timeMs = 0.0;
    /// 스트로크 진행도 0..1 (Fade 입력용). 모르면 0.
    f32 fade = 0.0f;
};

/// 스트로크 하나의 고정 설정. beginStroke() 에 한 번 넘긴다.
///
/// 🔴 `source`(획의 출처)는 **기본값이 없다**(docs/05 3.1 · A0).
///    기본 생성자를 지워서, 출처를 정하지 않은 스트로크는 **컴파일되지 않게** 했다.
///    기본값을 HumanPen 으로 두면 출처를 빠뜨린 코드가 AI 획을 사람 획으로 만든다.
///    사람 경로는 `StrokeSource::humanPen()`/`humanMouse()`, 에이전트 경로는
///    `mari::agent::AgentStrokeGate::source()` 를 쓴다 — 후자는 origin 을 인자로 받지 않는다.
struct StrokeContext {
    /// 유일한 생성자. 출처 없이는 스트로크 문맥 자체가 만들어지지 않는다.
    explicit StrokeContext(StrokeSource src) noexcept : source(src) {}

    /// 획의 출처. 이 값은 Sigan 프레임을 타고 서명 정본 안으로 들어간다.
    StrokeSource source;
    /// 그릴 대상 타일맵. 엔진은 writable() 로만 픽셀을 만진다.
    TileMap* target = nullptr;
    /// 지우개 모드. true 면 preset.blendMode 대신 Erase 로 합성한다.
    bool eraser = false;
    /// 알파 잠금 — 기존에 불투명한 픽셀만 고친다.
    bool alphaLocked = false;
    /// 전경색.
    Color color{};
    /// 배경색 — 색 변화(ColorDynamics.fgBgJitter)가 이쪽으로 오간다.
    Color background{255, 255, 255, 255};
    /// 난수 시드. 같은 시드 + 같은 입력 = 같은 획. 재현성이 Sigan 대조에 필요하다.
    u64 seed = 0;
    /// 기록용 레이어 id (더티 알림·저널에 붙는다).
    LayerId layerId = kInvalidLayerId;
    /// 선택 마스크. nullptr 이거나 `isAll()` 이면 제한이 없다.
    ///
    /// 🔴 **핫 패스 비용 0 규약**: 엔진은 `beginStroke()` 에서 이 값을 한 번만 보고
    ///    "제한 없음"이면 포인터를 꺼 둔다. 선택이 없는 그림에서 스탬프 하나당 늘어나는
    ///    비용은 0 이어야 한다(`tests/stroke/test_bench.cpp` 가 수치를 찍는다).
    ///    소유하지 않는다 — 스트로크가 끝날 때까지 살아 있어야 한다.
    const SelectionMask* selection = nullptr;
};

/// 브러시 엔진. 프리셋 하나를 물고 스탬프를 찍는다.
/// 인스턴스 하나는 **한 번에 한 스트로크**만 처리한다.
class IBrushEngine {
public:
    virtual ~IBrushEngine() = default;

    /// 엔진 이름 ("mypaint" / "native" 등). 프리셋의 engine 필드와 맞춘다.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// 프리셋을 적용한다. 엔진이 못 다루는 항목은 report 에 남긴다(정직하게 실패한다).
    /// 스트로크 중에는 부르지 않는다.
    [[nodiscard]] virtual Result<void> setPreset(const MariBrushPreset& preset,
                                                 ImportReport* report = nullptr) = 0;

    /// 스트로크 시작. 여기서 필요한 버퍼를 전부 잡아 둔다(핫 패스에서 할당하지 않기 위해).
    [[nodiscard]] virtual Result<void> beginStroke(const StrokeContext& ctx) = 0;

    /// 스탬프 하나를 찍는다. **핫 패스 — 던지지 않는다.**
    /// 건드린 타일 좌표를 dirty 에 **덧붙인다**(중복이 있을 수 있다. 호출자가 정리한다).
    virtual void stamp(const StampInput& in, DirtyTiles& dirty) noexcept = 0;

    /// 스트로크 종료. 남은 잉크를 밀어내고 그때 생긴 더티 타일을 덧붙인다.
    virtual void endStroke(DirtyTiles& dirty) noexcept = 0;

    /// 현재 프리셋 기준 스탬프 간격(px). 파이프라인 [3] 보간이 이 값을 쓴다.
    /// 필압에 따라 변하므로 대표 필압을 넘겨 묻는다.
    [[nodiscard]] virtual f32 spacingPx(f32 pressure) const noexcept = 0;
};

using BrushEnginePtr = std::unique_ptr<IBrushEngine>;

/// 엔진 팩토리. 엔진 모듈(engines/mypaint 등)이 자기를 등록한다.
/// 이름을 모르면 NotFound 오류를 돌려준다.
[[nodiscard]] Result<BrushEnginePtr> createEngine(const std::string& name);

} // namespace mari::brush

#endif // MARI_BRUSH_ENGINE_HPP
