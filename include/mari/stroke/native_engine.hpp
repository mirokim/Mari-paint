// Mari Paint — 자체 최소 브러시 엔진 ("native")
//
// 파이프라인 [4]. IBrushEngine 구현이다.
// libmypaint 어댑터(engines/mypaint)가 들어오면 **같은 인터페이스로** 갈아끼운다 —
// 그래서 이 헤더에는 엔진 내부 타입이 하나도 나오지 않는다. 팩토리 하나뿐이다.
//
// 지금 하는 것:
//   · 원/사각/마름모 절차적 팁 + 비트맵 팁(쌍선형 샘플링)
//   · 하드니스 가장자리 감쇠 + 안티에일리어싱(반지름이 작아도 1px 이상 전이를 보장)
//   · 종횡비·회전, spacing, 필압 → 크기/불투명도
//   · MariBrushPreset 의 동적 반응 맵을 **실제로** 적용한다(입력 8종 → 출력 6종)
//   · 그린 타일만 더티로 보고한다. 전체 캔버스를 건드리는 경로는 없다
//
// 아직 못 하는 것은 조용히 버리지 않고 ImportReport 에 남긴다(정직하게 실패한다).
#ifndef MARI_STROKE_NATIVE_ENGINE_HPP
#define MARI_STROKE_NATIVE_ENGINE_HPP

#include <mari/brush/engine.hpp>

namespace mari::stroke {

/// 엔진 이름. MariBrushPreset::engine 에 이 문자열을 넣으면 이 엔진이 걸린다.
inline constexpr const char* kNativeEngineName = "native";

/// 자체 엔진 하나를 만든다. 인스턴스 하나는 스레드 하나가 독점한다.
[[nodiscard]] Result<brush::BrushEnginePtr> makeNativeEngine();

} // namespace mari::stroke

#endif // MARI_STROKE_NATIVE_ENGINE_HPP
