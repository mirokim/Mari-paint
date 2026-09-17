// MariBrushPreset / ImportReport 테스트 — docs/02 5절의 "정직하게 실패한다" 가 표현되는지.
#include <mari/brush/preset.hpp>
#include <mari/test/harness.hpp>

#include <string>

using namespace mari;
using namespace mari::brush;

MARI_TEST(preset_defaults_are_sane) {
    MariBrushPreset p;
    CHECK(p.tip.kind == TipKind::Procedural);
    CHECK(p.tip.shape == ProceduralShape::Circle);
    CHECK_NEAR(p.tip.aspectRatio, 1.0f, 1e-6);
    CHECK_NEAR(p.spacing, 0.1f, 1e-6); // 지름 대비 비율. 퍼센트가 아니다
    CHECK_NEAR(p.opacity, 1.0f, 1e-6);
    CHECK(p.blendMode == BlendMode::Normal);
    CHECK(p.dynamics.empty());
    CHECK(!p.texture.has_value());
    CHECK(p.tip.bitmap.empty());
}

MARI_TEST(dynamics_map_input_to_output) {
    MariBrushPreset p;
    p.dynamics.push_back(DynamicLink{DynamicInput::Pressure,
                                     DynamicOutput::Size,
                                     ResponseCurve{{{0.0f, 0.0f}, {1.0f, 1.0f}}},
                                     1.0f});
    p.dynamics.push_back(
        DynamicLink{DynamicInput::Random, DynamicOutput::Scatter, ResponseCurve{}, 0.5f});
    CHECK_EQ(p.dynamics.size(), 2u);
    CHECK(p.dynamics[0].input == DynamicInput::Pressure);
    CHECK(p.dynamics[0].output == DynamicOutput::Size);
    CHECK_EQ(p.dynamics[0].curve.points.size(), 2u);
    CHECK(p.dynamics[1].curve.empty());
}

MARI_TEST(dynamic_names_are_stable) {
    CHECK_EQ(std::string(dynamicInputName(DynamicInput::Pressure)), std::string("pressure"));
    CHECK_EQ(std::string(dynamicInputName(DynamicInput::TiltX)), std::string("tilt-x"));
    CHECK_EQ(std::string(dynamicInputName(DynamicInput::Azimuth)), std::string("azimuth"));
    CHECK_EQ(std::string(dynamicOutputName(DynamicOutput::Roundness)), std::string("roundness"));
    CHECK_EQ(std::string(dynamicOutputName(DynamicOutput::Scatter)), std::string("scatter"));
    CHECK_EQ(kDynamicInputCount, 8);
    CHECK_EQ(kDynamicOutputCount, 6);
}

MARI_TEST(import_report_records_drops) {
    ImportReport r;
    r.sourcePath = "/tmp/brushes.abr";
    r.presetCount = 3;
    CHECK(r.clean());
    CHECK(!r.hasDropped());

    r.add(ImportSeverity::Degraded, "spacing", "간격을 근사했습니다");
    CHECK(!r.clean());
    CHECK(!r.hasDropped()); // 근사는 버린 게 아니다

    r.add(ImportSeverity::Dropped, "BrushTipShape/flipX",
          "팁 좌우 반전은 아직 지원하지 않아 반영되지 않았습니다");
    CHECK(r.hasDropped());
    CHECK_EQ(r.notes.size(), 2u);
    CHECK(r.notes[1].severity == ImportSeverity::Dropped);
}

MARI_TEST(import_result_bundles_presets_and_report) {
    ImportResult res;
    res.presets.push_back(MariBrushPreset{});
    res.presets.back().name = "둥근 붓";
    res.presets.back().sourceFormat = "sut";
    res.report.presetCount = res.presets.size();
    CHECK_EQ(res.report.presetCount, 1u);
    CHECK_EQ(res.presets[0].sourceFormat, std::string("sut"));
}

MARI_TEST(texture_is_optional) {
    MariBrushPreset p;
    BrushTexture t;
    t.image.width = 2;
    t.image.height = 2;
    t.image.pixels = {0, 64, 128, 255};
    p.texture = t;
    CHECK(p.texture.has_value());
    CHECK(!p.texture->image.empty());
    CHECK(p.texture->blendMode == BlendMode::Multiply);
    CHECK(p.texture->anchoredToCanvas);
}

MARI_TEST_MAIN()
