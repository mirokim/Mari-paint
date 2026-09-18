// Mari Paint — 실행취소가 붙은 레이어 구조 연산 (UI·에이전트가 같이 쓴다)
#ifndef MARI_APP_LAYER_COMMANDS_HPP
#define MARI_APP_LAYER_COMMANDS_HPP

#include <mari/app/document.hpp>
#include <mari/core/origin.hpp>
#include <mari/core/selection.hpp>

namespace mari::app {

/// 아래와 병합. 아래 레이어 픽셀 스냅샷 + 위 레이어 분리를 하나의 실행취소 명령으로 스택에 넣는다.
/// 성공하면 아래 레이어 id 를 돌려준다(활성 레이어로 쓰라).
[[nodiscard]] Result<LayerId> mergeLayerDown(Document& doc, LayerId upperId);

/// 레이어 삭제(실행취소 가능 — 지운 레이어를 id 그대로 되살린다).
[[nodiscard]] Result<void> removeLayerUndoable(Document& doc, LayerId id);

/// 마스크(0..255)가 덮는 자리에 색을 쓴다(페인트통). 마스크 값만큼 섞고, 문서의 선택 마스크와 교집합한다.
/// 실행취소 + 기록(RegionOpKind::Fill, 출처는 인자로 받는다 — 여기서 고르지 않는다).
/// 🔴 기록이 고장 나면 되돌리고 실패한다(docs/06 결정 ④).
[[nodiscard]] Result<u32> fillWithMask(Document& doc, const StrokeSource& src, LayerId layerId,
                                       const SelectionMask& mask, Color8 color, bool eraser);

/// 보이는 레이어 전부를 새 래스터 하나로 평탄화하고 나머지를 지운다(실행취소 하나).
[[nodiscard]] Result<LayerId> flattenAll(Document& doc);

/// 레이어 마스크. `fromSelection` 이면 현재 선택을 마스크로(선택 밖 = 가림), 아니면 전부 보임(255).
[[nodiscard]] Result<void> addLayerMask(Document& doc, LayerId id, bool fromSelection);
/// 마스크를 픽셀 알파에 구워 넣고 마스크를 없앤다.
[[nodiscard]] Result<void> applyLayerMask(Document& doc, LayerId id);
/// 마스크를 버린다.
[[nodiscard]] Result<void> removeLayerMask(Document& doc, LayerId id);
/// 현재 선택을 마스크에 쓴다(value: 255 보임 / 0 가림). 마스크가 없으면 만든다.
[[nodiscard]] Result<void> paintMaskWithSelection(Document& doc, LayerId id, u8 value);

} // namespace mari::app

#endif // MARI_APP_LAYER_COMMANDS_HPP
