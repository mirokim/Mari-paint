// Mari Paint — 레이어 부가 연산 (layer.hpp 의 인터페이스를 건드리지 않고 더한다)
#ifndef MARI_CORE_LAYER_OPS_HPP
#define MARI_CORE_LAYER_OPS_HPP

#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>

#include <string>

namespace mari {

/// 레이어 복제. **픽셀을 복사하지 않는다 — O(1)** (TileMap::snapshot 의 COW).
/// 복제본은 원본 바로 위에 같은 부모로 들어간다. 그룹이면 자식까지 재귀 복제한다.
/// newName 이 비면 "<원본 이름> 복사" 를 쓴다.
[[nodiscard]] Result<LayerPtr> duplicateLayer(LayerTree& tree, LayerId id,
                                              std::string newName = std::string{});

/// 레이어를 바로 아래 형제 래스터 레이어에 합치고 자신은 지운다(아래와 병합).
/// 위 레이어의 불투명도·블렌드 모드·가시성이 반영된다 — 화면 합성과 같은 커널을 쓴다.
/// 🔴 실행취소는 호출자 몫이다(호출 전에 두 레이어를 스냅샷해 두라).
[[nodiscard]] Result<void> mergeDown(LayerTree& tree, LayerId id);

/// 래스터 레이어의 픽셀 저장소를 통째로 갈아 끼운다.
/// 파일 로더(.ora)와 실행취소가 쓴다. 그룹 레이어면 실패한다.
[[nodiscard]] Result<void> setLayerTiles(Layer& layer, TileMapPtr tiles);

/// 트리에서 떼어낸 레이어를 **id 를 유지한 채** 다시 꽂는다.
///
/// 🔴 O(1) 스냅샷 복원(docs/05 2.2)을 위해 있다. `remove()` 된 레이어를
///    스냅샷이 shared_ptr 로 붙잡고 있다가 되돌릴 때 이 함수로 제자리에 넣는다.
///    새로 만들면 id 가 바뀌어 에이전트가 들고 있던 주소가 전부 무효가 된다.
/// 이미 트리에 있는 id 거나, 다른 트리에서 온 레이어면 실패한다.
[[nodiscard]] Result<void> reattachLayer(LayerTree& tree, const LayerPtr& layer,
                                         LayerId parent = kInvalidLayerId, int index = -1);


} // namespace mari

#endif // MARI_CORE_LAYER_OPS_HPP
