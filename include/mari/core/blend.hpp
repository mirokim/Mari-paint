// Mari Paint — 픽셀 단위 블렌드 모드
//
// docs/02 2절 core/layer. types.hpp 의 BlendMode 전부를 여기서 구현한다.
//
// 규약:
//   · 색은 **straight alpha**(비프리멀티플라이)다. 내부 계산만 프리멀티플라이를 쓴다.
//   · 합성식은 W3C Compositing and Blending Level 1 을 따른다.
//       ao = as + ab*(1-as)
//       Co*ao = (1-ab)*as*Cs + ab*as*B(Cb,Cs) + (1-as)*ab*Cb
//   · Erase 만 예외다 — 색을 섞지 않고 대상 알파를 깎는다(지우개 스탬프용).
//   · 전부 noexcept. 합성기 핫 패스에서 불린다.
#ifndef MARI_CORE_BLEND_HPP
#define MARI_CORE_BLEND_HPP

#include <mari/core/types.hpp>

namespace mari {

/// 채널별로 독립 계산되는 모드인지. Hue/Saturation/Color/Luminosity/Erase 는 false.
[[nodiscard]] bool isSeparableBlend(BlendMode m) noexcept;

/// 분리 가능 모드의 채널 블렌드 함수 B(cb, cs). 입출력 [0,1].
/// 분리 불가 모드에 부르면 cs 를 그대로 돌려준다(의미 있는 답이 없다).
[[nodiscard]] f32 blendChannel(BlendMode m, f32 cb, f32 cs) noexcept;

/// 픽셀 하나 합성. dst(배경) 위에 src(전경)를 extraAlpha 배(레이어 불투명도·마스크)로 올린다.
/// 입출력 모두 straight alpha.
[[nodiscard]] ColorF blendPixel(BlendMode m, const ColorF& dst, const ColorF& src,
                                f32 extraAlpha = 1.0f) noexcept;

/// blendPixel 의 8비트판. 반올림해서 되돌린다.
[[nodiscard]] Color8 blendPixel8(BlendMode m, Color8 dst, Color8 src,
                                 f32 extraAlpha = 1.0f) noexcept;

/// RGBA8 한 줄을 제자리 합성한다. 합성기의 안쪽 루프.
/// mask 는 픽셀당 1바이트(Gray8)이며 nullptr 이면 255(전부 통과)로 본다.
void blendRowRgba8(BlendMode m, u8* dst, const u8* src, i32 count, f32 extraAlpha,
                   const u8* mask) noexcept;

} // namespace mari

#endif // MARI_CORE_BLEND_HPP
