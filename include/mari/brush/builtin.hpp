// Mari Paint — 기본 내장 브러시 네 자루
//
// agent-api 세션(`brush.list`)과 GUI 툴바가 **같은 목록**을 본다. 두 곳에 따로 적으면
// AI 가 고른 "연필" 과 사람이 고른 "연필" 이 다른 붓이 되고, 그건 기록 대조에서 설명이 안 된다.
#ifndef MARI_BRUSH_BUILTIN_HPP
#define MARI_BRUSH_BUILTIN_HPP

#include <mari/brush/preset.hpp>

#include <vector>

namespace mari::brush {

/// 내장 프리셋. 순서가 곧 기본 id 순서다(첫 번째가 기본 붓).
[[nodiscard]] std::vector<MariBrushPreset> builtinPresets();

} // namespace mari::brush

#endif // MARI_BRUSH_BUILTIN_HPP
