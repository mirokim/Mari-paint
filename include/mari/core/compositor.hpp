// Mari Paint — 합성기(Compositor). 레이어 트리 → RGBA8 평면.
//
// docs/02 4절 [5]단계: **더티 타일만 블렌딩한다.**
// "전체 캔버스를 다시 그리는 코드는 절대 넣지 않는다" 를 지키려고
// 영역 합성(compositeArea)과 더티 합성(compositeDirty)을 나눠 뒀다.
//
// 규약:
//   · 좌표는 전부 **캔버스 좌표**(좌상단 원점, y 아래로 증가).
//   · 출력은 RGBA8 straight alpha. dst 는 area 크기의 버퍼이고 행 간격은 dstStride 바이트다.
//   · 합성은 인덱스 0(가장 아래)부터 위로 올라간다.
//   · 그룹 레이어는 자식을 임시 버퍼에 먼저 합성한 뒤, 그 결과를 그룹의
//     불투명도·블렌드 모드로 한 덩어리처럼 올린다(격리 그룹).
//   · **레이어 마스크 규약**: 마스크는 Gray8 타일맵이고 값이 클수록 보인다.
//     마스크가 붙은 레이어에서 **마스크 타일이 없는 영역은 0(완전히 가림)** 으로 본다
//     — "없는 타일 = 0" 이라는 TileMap 전체 규약과 같게 맞춘 것이다.
//     즉 마스크를 붙이면 칠한 곳만 드러난다(reveal 마스크).
#ifndef MARI_CORE_COMPOSITOR_HPP
#define MARI_CORE_COMPOSITOR_HPP

#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

namespace mari {

/// 더티 목록의 중복을 없앤다(제자리 정렬 후 unique).
/// 엔진은 중복을 허용하고 덧붙이기만 하므로 소비자가 여기서 정리한다.
void dedupDirtyTiles(DirtyTiles& tiles);

/// 더티 타일들이 덮는 캔버스 경계 상자. 비면 빈 Rect.
[[nodiscard]] Rect dirtyTilesBounds(const DirtyTiles& tiles);

/// 레이어 트리의 area 를 RGBA8 로 평탄화한다.
/// dst 는 호출자가 미리 지울 필요가 없다 — 투명으로 초기화한 뒤 덮어쓴다.
[[nodiscard]] Result<void> compositeArea(const LayerTree& tree, const Rect& area, u8* dst,
                                         usize dstStride);

/// **더티 타일만** 다시 합성한다.
/// dst 는 dstArea 전체를 담는 버퍼다. dirty 중 dstArea 와 겹치는 부분만 갱신하고
/// 나머지 픽셀은 손대지 않는다. 중복 좌표는 알아서 무시한다.
[[nodiscard]] Result<void> compositeDirty(const LayerTree& tree, const DirtyTiles& dirty,
                                          const Rect& dstArea, u8* dst, usize dstStride);

} // namespace mari

#endif // MARI_CORE_COMPOSITOR_HPP
