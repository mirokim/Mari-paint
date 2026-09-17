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

/// 래스터 레이어의 픽셀 저장소를 통째로 갈아 끼운다.
/// 파일 로더(.ora)와 실행취소가 쓴다. 그룹 레이어면 실패한다.
[[nodiscard]] Result<void> setLayerTiles(Layer& layer, TileMapPtr tiles);

} // namespace mari

#endif // MARI_CORE_LAYER_OPS_HPP
