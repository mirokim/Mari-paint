// Mari Paint — PNG → GrayImage 디코더 (브러시 텍스처/팁용).
//
// .sut 의 텍스처는 tar 안의 PNG 다. 시스템 libpng 을 쓴다(외부 의존성 fetch 금지 규칙).
// 결과는 mari::brush::GrayImage — "값이 클수록 잉크가 많다"는 규약을 따른다.
#ifndef MARI_IO_BRUSH_PNG_GRAY_HPP
#define MARI_IO_BRUSH_PNG_GRAY_HPP

#include <mari/brush/preset.hpp>
#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

namespace mari::io::brush {

/// 디코드 결과에 붙는 참고 정보. 임포터가 리포트에 옮긴다.
struct PngGrayInfo {
    /// 원본에 알파 채널이 있었다. 투명한 곳은 흰색(=잉크 없음)으로 합성했다.
    bool hadAlpha = false;
    /// 원본이 컬러였다. 휘도로 접었다.
    bool hadColor = false;
    /// 원본 비트 깊이(8 로 낮췄으면 여기 원래 값이 남는다).
    int sourceBitDepth = 8;
};

/// 메모리 안의 PNG 를 그레이스케일로 디코드한다.
/// `invert` 가 true 면 밝은 픽셀이 잉크가 적은 것으로 뒤집는다(팁 마스크용).
[[nodiscard]] Result<mari::brush::GrayImage> decodePngGray(const u8* data, usize size,
                                                           bool invert = false,
                                                           PngGrayInfo* info = nullptr);

/// 팁/텍스처용 **잉크 마스크**로 디코드한다: 잉크 = 알파 × (255 − 밝기).
/// CSP 소재 썸네일(RGBA, 투명 바탕에 회색 그림)에 맞는 규약이다. 알파가 없으면 255 − 밝기.
[[nodiscard]] Result<mari::brush::GrayImage> decodePngInk(const u8* data, usize size);

/// 바이트가 PNG 시그니처로 시작하는가.
[[nodiscard]] bool looksLikePng(const u8* data, usize size);

} // namespace mari::io::brush

#endif // MARI_IO_BRUSH_PNG_GRAY_HPP
