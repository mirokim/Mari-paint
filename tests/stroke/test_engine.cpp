// 파이프라인 [4] 자체 엔진 — 필압 반영, 더티 정확도, 정직한 임포트 리포트.
#include <mari/stroke/native_engine.hpp>
#include <mari/test/harness.hpp>

#include "fake_tilemap.hpp"

#include <algorithm>
#include <cmath>

using namespace mari;
using namespace mari::brush;
using namespace mari::stroke;
using mari::test::FakeTileMap;

namespace {

MariBrushPreset circlePreset(f32 diameter, f32 hardness = 1.0f) {
    MariBrushPreset p;
    p.name = "test-round";
    p.sourceFormat = "native";
    p.tip.kind = TipKind::Procedural;
    p.tip.shape = ProceduralShape::Circle;
    p.tip.diameter = diameter;
    p.tip.hardness = hardness;
    p.spacing = 0.1f;
    p.opacity = 1.0f;
    p.flow = 1.0f;
    return p;
}

/// 입력 → 출력 항등 반응(커브 점 0개 = 항등).
DynamicLink link(DynamicInput in, DynamicOutput out, f32 amount = 1.0f) {
    DynamicLink d;
    d.input = in;
    d.output = out;
    d.amount = amount;
    return d;
}

StampInput at(f32 x, f32 y, f32 pressure = 1.0f) {
    StampInput s;
    s.pos = PointF{x, y};
    s.pressure = pressure;
    return s;
}

StrokeContext ctxFor(TileMap* map) {
    // A0: 출처 없는 스트로크는 컴파일되지 않는다. 테스트도 사람 펜임을 명시한다.
    StrokeContext c(StrokeSource::humanPen());
    c.target = map;
    c.color = Color8::rgba(0, 0, 0, 255);
    c.seed = 12345;
    c.layerId = 7;
    return c;
}

/// 스탬프 하나를 찍고 더티 목록을 정렬해 돌려준다.
DirtyTiles stampOnce(IBrushEngine& e, TileMap& map, const StampInput& in) {
    DirtyTiles dirty;
    const auto r = e.beginStroke(ctxFor(&map));
    (void)r;
    e.stamp(in, dirty);
    e.endStroke(dirty);
    FakeTileMap::sortTiles(dirty);
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    return dirty;
}

} // namespace

MARI_TEST(engine_factory_registers_native) {
    auto a = createEngine("native");
    CHECK(a.ok());
    CHECK_EQ(std::string(a.value()->name()), std::string("native"));

    auto b = createEngine(""); // 비어 있으면 기본 엔진
    CHECK(b.ok());

    auto c = createEngine("mypaint"); // 아직 없다
    CHECK(!c.ok());
    CHECK_EQ(c.code(), ErrorCode::NotFound);
}

MARI_TEST(engine_needs_preset_and_rgba8) {
    auto e = makeNativeEngine();
    CHECK(e.ok());
    FakeTileMap map;
    // 프리셋 없이 시작하면 거부한다.
    CHECK(!e.value()->beginStroke(ctxFor(&map)).ok());
    CHECK(e.value()->setPreset(circlePreset(10.0f), nullptr).ok());
    CHECK(!e.value()->beginStroke(ctxFor(nullptr)).ok());
    CHECK(e.value()->beginStroke(ctxFor(&map)).ok());
}

MARI_TEST(engine_dot_paints_and_reports_only_touched_tiles) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(10.0f), nullptr).ok());
    FakeTileMap map;
    const DirtyTiles dirty = stampOnce(*e.value(), map, at(100.0f, 100.0f));

    CHECK_EQ(dirty.size(), 1u);
    CHECK(dirty[0] == (TileCoord{1, 1}));
    CHECK_EQ(map.pixelAt(100, 100).a, 255);
    CHECK_EQ(map.pixelAt(100, 100).r, 0);
    CHECK_EQ(map.pixelAt(120, 100).a, 0); // 반지름 밖
}

MARI_TEST(engine_dirty_never_over_reports) {
    // 반지름 70 원을 (128,128) 에 찍는다. 경계 상자는 타일 (0,0) 까지 덮지만
    // 원은 그 타일에 **닿지 않는다.** 경계 상자로 보고하면 여기서 걸린다.
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(140.0f), nullptr).ok());
    FakeTileMap map;
    const DirtyTiles dirty = stampOnce(*e.value(), map, at(128.0f, 128.0f));

    for (const TileCoord& c : dirty)
        CHECK(!(c == (TileCoord{0, 0})));
    // 그리고 실제로 칠해진 타일과 정확히 같아야 한다(과대·과소 둘 다 아님).
    CHECK(dirty == map.paintedTiles());
    CHECK(map.tileCount() == dirty.size()); // 빈 타일을 만들지도 않는다
}

MARI_TEST(engine_transparent_stamp_creates_nothing) {
    auto e = makeNativeEngine();
    MariBrushPreset p = circlePreset(20.0f);
    p.opacity = 0.0f;
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap map;
    const DirtyTiles dirty = stampOnce(*e.value(), map, at(10.0f, 10.0f));
    CHECK_EQ(dirty.size(), 0u);
    CHECK_EQ(map.tileCount(), 0u); // 타일을 잡지도 않는다
}

MARI_TEST(engine_pressure_drives_size) {
    MariBrushPreset p = circlePreset(40.0f);
    p.dynamics.push_back(link(DynamicInput::Pressure, DynamicOutput::Size));

    const auto widthAt = [&](f32 pressure) -> i32 {
        auto e = makeNativeEngine();
        (void)e.value()->setPreset(p, nullptr);
        FakeTileMap map;
        stampOnce(*e.value(), map, at(200.0f, 200.0f, pressure));
        return map.paintedBounds().width;
    };

    const i32 full = widthAt(1.0f);
    const i32 half = widthAt(0.5f);
    const i32 quarter = widthAt(0.25f);
    CHECK_NEAR(full, 40, 2);
    CHECK_NEAR(half, 20, 2);
    CHECK_NEAR(quarter, 10, 2);
    CHECK(quarter < half);
    CHECK(half < full);
}

MARI_TEST(engine_pressure_drives_opacity) {
    MariBrushPreset p = circlePreset(20.0f);
    p.dynamics.push_back(link(DynamicInput::Pressure, DynamicOutput::Opacity));

    const auto alphaAt = [&](f32 pressure) -> int {
        auto e = makeNativeEngine();
        (void)e.value()->setPreset(p, nullptr);
        FakeTileMap map;
        stampOnce(*e.value(), map, at(100.0f, 100.0f, pressure));
        return map.pixelAt(100, 100).a;
    };

    CHECK_EQ(alphaAt(1.0f), 255);
    CHECK_NEAR(alphaAt(0.5f), 128, 2);
    CHECK_NEAR(alphaAt(0.2f), 51, 2);
    CHECK(alphaAt(0.2f) < alphaAt(0.5f));
}

MARI_TEST(engine_dynamic_amount_zero_disables_link) {
    MariBrushPreset p = circlePreset(20.0f);
    p.dynamics.push_back(link(DynamicInput::Pressure, DynamicOutput::Opacity, 0.0f));
    auto e = makeNativeEngine();
    (void)e.value()->setPreset(p, nullptr);
    FakeTileMap map;
    stampOnce(*e.value(), map, at(100.0f, 100.0f, 0.1f));
    CHECK_EQ(map.pixelAt(100, 100).a, 255); // amount=0 이면 필압을 무시한다
}

MARI_TEST(engine_response_curve_is_applied) {
    // 필압 → 불투명도를 "항상 0.25" 상수 커브로 묶는다.
    MariBrushPreset p = circlePreset(20.0f);
    DynamicLink d = link(DynamicInput::Pressure, DynamicOutput::Opacity);
    d.curve.points.push_back(CurvePoint{0.0f, 0.25f}); // 점 1개 = 상수
    p.dynamics.push_back(d);

    auto e = makeNativeEngine();
    (void)e.value()->setPreset(p, nullptr);
    FakeTileMap map;
    stampOnce(*e.value(), map, at(100.0f, 100.0f, 1.0f));
    CHECK_NEAR(map.pixelAt(100, 100).a, 64, 2);
}

MARI_TEST(engine_antialiases_the_edge) {
    // 하드니스 1이어도 가장자리는 0/255 로 끊기지 않는다.
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(21.0f, 1.0f), nullptr).ok());
    FakeTileMap map;
    stampOnce(*e.value(), map, at(100.5f, 100.5f));

    bool partial = false;
    for (i32 x = 85; x < 116; ++x) {
        const u8 a = map.pixelAt(x, 100).a;
        if (a > 0 && a < 255)
            partial = true;
    }
    CHECK(partial);
    CHECK_EQ(map.pixelAt(100, 100).a, 255); // 가운데는 꽉 찬다
}

MARI_TEST(engine_soft_brush_fades_outward) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(40.0f, 0.0f), nullptr).ok());
    FakeTileMap map;
    stampOnce(*e.value(), map, at(100.0f, 100.0f));
    const u8 center = map.pixelAt(100, 100).a;
    const u8 mid = map.pixelAt(110, 100).a;
    const u8 outer = map.pixelAt(118, 100).a;
    CHECK(center > mid);
    CHECK(mid > outer);
}

MARI_TEST(engine_aspect_ratio_makes_an_ellipse) {
    MariBrushPreset p = circlePreset(40.0f);
    p.tip.aspectRatio = 0.25f;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap map;
    stampOnce(*e.value(), map, at(200.0f, 200.0f));
    const Rect b = map.paintedBounds();
    CHECK_NEAR(b.width, 40, 2);
    CHECK_NEAR(b.height, 10, 2);
}

MARI_TEST(engine_eraser_removes_alpha) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(40.0f), nullptr).ok());
    FakeTileMap map;
    stampOnce(*e.value(), map, at(100.0f, 100.0f));
    CHECK_EQ(map.pixelAt(100, 100).a, 255);

    StrokeContext c = ctxFor(&map);
    c.eraser = true;
    DirtyTiles dirty;
    CHECK(e.value()->beginStroke(c).ok());
    e.value()->stamp(at(100.0f, 100.0f), dirty);
    e.value()->endStroke(dirty);
    CHECK_EQ(map.pixelAt(100, 100).a, 0);
}

MARI_TEST(engine_alpha_lock_does_not_paint_transparent_pixels) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(20.0f), nullptr).ok());
    FakeTileMap map;
    StrokeContext c = ctxFor(&map);
    c.alphaLocked = true;
    DirtyTiles dirty;
    CHECK(e.value()->beginStroke(c).ok());
    e.value()->stamp(at(100.0f, 100.0f), dirty);
    e.value()->endStroke(dirty);
    CHECK_EQ(map.pixelAt(100, 100).a, 0); // 빈 레이어에는 아무것도 안 올라간다
}

MARI_TEST(engine_spacing_follows_diameter_and_pressure) {
    MariBrushPreset p = circlePreset(50.0f);
    p.spacing = 0.2f; // 지름의 20%
    p.dynamics.push_back(link(DynamicInput::Pressure, DynamicOutput::Size));
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    CHECK_NEAR(e.value()->spacingPx(1.0f), 10.0f, 1e-3);
    CHECK_NEAR(e.value()->spacingPx(0.5f), 5.0f, 1e-3);
}

MARI_TEST(engine_reports_what_it_could_not_do) {
    // 정직하게 실패한다 — 못 한 건 조용히 버리지 않는다.
    MariBrushPreset p = circlePreset(20.0f);
    p.texture = BrushTexture{};
    p.texture->image = GrayImage{2, 2, {0, 1, 2, 3}};
    p.texture->blendMode = BlendMode::Hue; // 텍스처 합성으로는 말이 안 된다 → 곱하기로 근사
    p.blendMode = BlendMode::Overlay; // 아직 못 한다
    p.extraParams.push_back({"csp/effector_size", 1.0f});
    p.tip.kind = TipKind::Bitmap; // 비트맵인데 그림이 없다
    p.airbrush = true;            // 시간 반복은 안 한다

    ImportReport rep;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, &rep).ok());

    CHECK(!rep.hasDropped()); // 텍스처는 이제 합성한다 — 버리는 게 없어야 한다
    CHECK(!rep.clean());
    bool texture = false, blend = false, extra = false, bitmap = false, air = false;
    for (const auto& n : rep.notes) {
        if (n.sourceKey == "texture/blendMode" && n.severity == ImportSeverity::Degraded)
            texture = true;
        if (n.sourceKey == "blendMode" && n.severity == ImportSeverity::Degraded)
            blend = true;
        if (n.sourceKey == "csp/effector_size" && n.severity == ImportSeverity::Info)
            extra = true;
        if (n.sourceKey == "tip/bitmap" && n.severity == ImportSeverity::Degraded)
            bitmap = true;
        if (n.sourceKey == "airbrush" && n.severity == ImportSeverity::Degraded)
            air = true;
    }
    CHECK(texture);
    CHECK(blend);
    CHECK(extra);
    CHECK(bitmap);
    CHECK(air);
}

MARI_TEST(engine_texture_multiplies_coverage) {
    // 2×1 텍스처 {0, 255}, 배율 1, 캔버스 고정: 짝수 x 는 잉크 0, 홀수 x 는 그대로.
    MariBrushPreset p = circlePreset(40.0f);
    p.texture = BrushTexture{};
    p.texture->image = GrayImage{2, 1, {0, 255}};
    p.texture->scale = 1.0f;
    p.texture->depth = 1.0f;
    p.texture->anchoredToCanvas = true;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap map;
    (void)stampOnce(*e.value(), map, at(100.0f, 100.0f));
    CHECK_EQ(map.pixelAt(100, 100).a, 0);   // 짝수 x → 텍스처 0
    CHECK_EQ(map.pixelAt(101, 100).a, 255); // 홀수 x → 텍스처 1
    // depth 0.5 면 짝수 x 도 반은 묻는다.
    p.texture->depth = 0.5f;
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap map2;
    (void)stampOnce(*e.value(), map2, at(100.0f, 100.0f));
    CHECK(map2.pixelAt(100, 100).a > 100 && map2.pixelAt(100, 100).a < 155);
}

MARI_TEST(engine_dual_brush_masks_the_tip) {
    // 듀얼 팁이 첫 팁보다 작으면 결과는 작은 쪽만큼만 찍힌다(곱하기).
    MariBrushPreset p = circlePreset(40.0f);
    p.dual = DualBrush{};
    p.dual->tip.kind = TipKind::Procedural;
    p.dual->tip.diameter = 10.0f;
    p.dual->tip.hardness = 1.0f;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap map;
    (void)stampOnce(*e.value(), map, at(100.0f, 100.0f));
    CHECK_EQ(map.pixelAt(100, 100).a, 255);
    CHECK_EQ(map.pixelAt(112, 100).a, 0); // 첫 팁 안(반지름 20)이지만 듀얼 밖(반지름 5)
}

MARI_TEST(engine_color_dynamics_change_color_per_stamp_reproducibly) {
    MariBrushPreset p = circlePreset(6.0f);
    p.colorDynamics.hueJitter = 1.0f;
    p.colorDynamics.perTip = true;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    const auto run = [&](FakeTileMap& map) {
        StrokeContext c = ctxFor(&map);
        c.color = Color8::rgba(255, 0, 0, 255);
        DirtyTiles dirty;
        CHECK(e.value()->beginStroke(c).ok());
        e.value()->stamp(at(20.0f, 20.0f), dirty);
        e.value()->stamp(at(60.0f, 20.0f), dirty);
        e.value()->stamp(at(100.0f, 20.0f), dirty);
        e.value()->endStroke(dirty);
    };
    FakeTileMap a, b;
    run(a);
    run(b);
    const auto p1 = a.pixelAt(20, 20), p2 = a.pixelAt(60, 20), p3 = a.pixelAt(100, 20);
    CHECK(p1.a == 255 && p2.a == 255 && p3.a == 255);
    // 셋이 전부 같은 색일 확률은 사실상 0 이다.
    CHECK(!(p1.r == p2.r && p1.g == p2.g && p1.b == p2.b && p2.r == p3.r && p2.g == p3.g && p2.b == p3.b));
    // 같은 시드 → 같은 색.
    CHECK(b.pixelAt(20, 20).r == p1.r && b.pixelAt(20, 20).g == p1.g && b.pixelAt(20, 20).b == p1.b);
    CHECK(b.pixelAt(60, 20).r == p2.r && b.pixelAt(60, 20).g == p2.g && b.pixelAt(60, 20).b == p2.b);
}

MARI_TEST(engine_scatter_count_stacks_ink) {
    // 반투명 스탬프를 같은 자리에 3번 찍으면 1번보다 진하다.
    MariBrushPreset p = circlePreset(10.0f);
    p.opacity = 0.3f;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap one;
    (void)stampOnce(*e.value(), one, at(50.0f, 50.0f));
    p.scatterCount = 3;
    CHECK(e.value()->setPreset(p, nullptr).ok());
    FakeTileMap three;
    (void)stampOnce(*e.value(), three, at(50.0f, 50.0f));
    CHECK(three.pixelAt(50, 50).a > one.pixelAt(50, 50).a + 60);
}

MARI_TEST(engine_clean_preset_reports_nothing) {
    ImportReport rep;
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(circlePreset(20.0f), &rep).ok());
    CHECK(rep.clean());
}

MARI_TEST(engine_bitmap_tip_is_sampled) {
    // 2×2 비트맵: 위쪽 절반만 잉크가 있다.
    MariBrushPreset p = circlePreset(20.0f);
    p.tip.kind = TipKind::Bitmap;
    p.tip.bitmap = GrayImage{2, 2, {255, 255, 0, 0}};
    auto e = makeNativeEngine();
    ImportReport rep;
    CHECK(e.value()->setPreset(p, &rep).ok());
    FakeTileMap map;
    stampOnce(*e.value(), map, at(100.0f, 100.0f));
    CHECK(map.pixelAt(100, 94).a > map.pixelAt(100, 106).a); // y 는 아래로 증가한다
}

MARI_TEST(engine_same_seed_reproduces_the_same_pixels) {
    // Sigan 대조가 여기 걸려 있다. 시드가 같으면 흩뿌림도 같아야 한다.
    MariBrushPreset p = circlePreset(12.0f);
    DynamicLink d = link(DynamicInput::Random, DynamicOutput::Scatter);
    d.curve.points.push_back(CurvePoint{0.0f, 0.0f});
    d.curve.points.push_back(CurvePoint{1.0f, 2.0f});
    p.dynamics.push_back(d);

    const auto runWith = [&](u64 seed) -> Rect {
        auto e = makeNativeEngine();
        (void)e.value()->setPreset(p, nullptr);
        FakeTileMap map;
        StrokeContext c = ctxFor(&map);
        c.seed = seed;
        DirtyTiles dirty;
        (void)e.value()->beginStroke(c);
        for (int i = 0; i < 20; ++i)
            e.value()->stamp(at(200.0f + static_cast<f32>(i), 200.0f), dirty);
        e.value()->endStroke(dirty);
        return map.paintedBounds();
    };

    const Rect a = runWith(42);
    const Rect b = runWith(42);
    const Rect c = runWith(99);
    CHECK(a == b);
    CHECK(!(a == c));
}

MARI_TEST_MAIN()
