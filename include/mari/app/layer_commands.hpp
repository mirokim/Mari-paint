// Mari Paint — 실행취소가 붙은 레이어 구조 연산 (UI·에이전트가 같이 쓴다)
#ifndef MARI_APP_LAYER_COMMANDS_HPP
#define MARI_APP_LAYER_COMMANDS_HPP

#include <mari/app/document.hpp>

namespace mari::app {

/// 아래와 병합. 아래 레이어 픽셀 스냅샷 + 위 레이어 분리를 하나의 실행취소 명령으로 스택에 넣는다.
/// 성공하면 아래 레이어 id 를 돌려준다(활성 레이어로 쓰라).
[[nodiscard]] Result<LayerId> mergeLayerDown(Document& doc, LayerId upperId);

/// 레이어 삭제(실행취소 가능 — 지운 레이어를 id 그대로 되살린다).
[[nodiscard]] Result<void> removeLayerUndoable(Document& doc, LayerId id);

} // namespace mari::app

#endif // MARI_APP_LAYER_COMMANDS_HPP
