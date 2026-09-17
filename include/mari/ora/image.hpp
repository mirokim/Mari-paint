// Mari Paint — .ora 입출력용 8비트 RGBA 이미지와 PNG 코덱
//
// .ora 안의 픽셀은 전부 PNG 다. 여기서는 libpng 로 RGBA8 ↔ PNG 만 다룬다.
// 색은 코어와 같은 **비프리멀티플라이(straight alpha)** 다.
#ifndef MARI_ORA_IMAGE_HPP
#define MARI_ORA_IMAGE_HPP

#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

#include <vector>

namespace mari::ora {

/// 8비트 RGBA 이미지. 행 stride 는 항상 width*4 바이트다.
struct Image8 {
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> pixels; ///< width*height*4 바이트, R,G,B,A 순

    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] usize stride() const { return static_cast<usize>(width) * 4u; }
    /// width*height*4 크기로 0(완전 투명) 채워 만든다.
    static Image8 make(i32 w, i32 h);
};

/// RGBA8 이미지를 PNG 바이트로. level 은 zlib 압축 강도(0~9).
[[nodiscard]] Result<std::vector<u8>> encodePng(const Image8& img, int level = 6);

/// PNG 바이트를 RGBA8 로. 팔레트·그레이·16비트·tRNS 는 전부 RGBA8 로 펼친다.
/// 깨진 입력에는 **던지지 않고** ParseError 를 돌려준다.
[[nodiscard]] Result<Image8> decodePng(const u8* data, usize size);

// ── 타일맵 ↔ 이미지 ──────────────────────────────────────────────────────

/// 타일맵의 캔버스 영역 area 를 RGBA8 이미지로 읽어낸다.
/// 할당되지 않은 타일은 완전 투명으로 채운다. RGBA8 타일맵만 받는다.
[[nodiscard]] Result<Image8> readRegion(const TileMap& map, const Rect& area);

/// RGBA8 이미지를 타일맵의 캔버스 영역 area 에 써 넣는다(덮어쓰기, 합성 아님).
/// 전부 투명한 타일은 만들지 않는다 — 희소성을 지킨다.
[[nodiscard]] Result<void> writeRegion(TileMap& map, const Rect& area, const Image8& img);

/// 알파 가중 박스 필터로 축소한다. maxSide 이하가 될 때까지. 확대는 하지 않는다.
[[nodiscard]] Image8 downscaleToFit(const Image8& src, i32 maxSide);

} // namespace mari::ora

#endif // MARI_ORA_IMAGE_HPP
