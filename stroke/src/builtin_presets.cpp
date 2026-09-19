// Mari Paint — 기본 내장 브러시 (include/mari/brush/builtin.hpp)
#include <mari/brush/builtin.hpp>

namespace mari::brush {

std::vector<MariBrushPreset> builtinPresets() {
    std::vector<MariBrushPreset> out;
    auto add = [&](const char* name, f32 diameter, f32 hardness, f32 spacing, f32 opacity,
                   bool pressureSize) {
        MariBrushPreset p;
        p.name = name;
        p.sourceFormat = "native";
        p.engine = "native";
        p.tip.diameter = diameter;
        p.tip.hardness = hardness;
        p.spacing = spacing;
        p.opacity = opacity;
        if (pressureSize) {
            DynamicLink link;
            link.input = DynamicInput::Pressure;
            link.output = DynamicOutput::Size;
            link.amount = 1.0f;
            link.curve.points = {{0.0f, 0.15f}, {1.0f, 1.0f}};
            p.dynamics.push_back(link);
        }
        out.push_back(std::move(p));
    };
    // 기본 네 자루. 런타임 발견이 원칙이므로 목록은 brush.list 로 읽게 한다.
    add("연필", 6.0f, 0.85f, 0.08f, 0.9f, true);
    add("잉크펜", 10.0f, 1.0f, 0.06f, 1.0f, true);
    add("에어브러시", 48.0f, 0.15f, 0.05f, 0.35f, true);
    out.back().airbrush = true; // 머물면 쌓인다(GUI 30ms 타이머 · 에이전트 t 간격)
    add("납작붓", 28.0f, 0.6f, 0.12f, 1.0f, false);
    return out;
}

} // namespace mari::brush
