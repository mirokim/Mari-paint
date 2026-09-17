// Mari Paint — BlendMode ↔ OpenRaster composite-op 문자열 매핑
//
// .ora 의 composite-op 는 SVG 합성/혼합 연산자 이름을 쓴다(`svg:` 접두사).
// 스펙이 필수로 요구하는 건 `svg:src-over` 하나뿐이고 나머지는 권장이다.
// 우리가 가진 BlendMode 중 SVG 에 대응이 없는 것은 `mari:` 접두사로 내보내고,
// 읽을 때는 Krita/GIMP 가 쓰는 별칭도 함께 받아준다(관대하게 읽고, 엄격하게 쓴다).
//
// | BlendMode   | 쓰기 이름          | 읽기 별칭 |
// |-------------|--------------------|-----------|
// | Normal      | svg:src-over       | normal, src-over |
// | Multiply    | svg:multiply       | multiply  |
// | Screen      | svg:screen         | screen    |
// | Overlay     | svg:overlay        | overlay   |
// | Darken      | svg:darken         | darken    |
// | Lighten     | svg:lighten        | lighten   |
// | ColorDodge  | svg:color-dodge    | color-dodge, dodge |
// | ColorBurn   | svg:color-burn     | color-burn, burn |
// | HardLight   | svg:hard-light     | hard-light |
// | SoftLight   | svg:soft-light     | soft-light |
// | Difference  | svg:difference     | difference |
// | Exclusion   | svg:exclusion      | exclusion |
// | Hue         | svg:hue            | hue       |
// | Saturation  | svg:saturation     | saturation |
// | Color       | svg:color          | color     |
// | Luminosity  | svg:luminosity     | luminosity |
// | Add         | svg:plus           | svg:add, plus, add, linear-dodge |
// | Subtract    | mari:subtract      | krita:subtract, subtract |
// | Erase       | svg:dst-out        | dst-out, erase |
#ifndef MARI_ORA_COMPOSITE_OP_HPP
#define MARI_ORA_COMPOSITE_OP_HPP

#include <mari/core/types.hpp>

#include <optional>
#include <string_view>

namespace mari::ora {

/// BlendMode 를 .ora 에 쓸 composite-op 문자열로. 모르는 값은 "svg:src-over".
[[nodiscard]] std::string_view compositeOpName(BlendMode mode);

/// composite-op 문자열을 BlendMode 로. 우리가 모르는 연산자면 std::nullopt.
/// 호출자는 nullopt 를 Normal 로 떨어뜨리되 **경고를 남겨야 한다**(조용히 버리지 않는다).
[[nodiscard]] std::optional<BlendMode> blendModeFromCompositeOp(std::string_view op);

} // namespace mari::ora

#endif // MARI_ORA_COMPOSITE_OP_HPP
