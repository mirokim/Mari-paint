// 파이프라인 전체 [1]~[4] + 더티 추적 — 실제로 그린 범위와 보고가 일치하는가.
#include <mari/stroke/native_engine.hpp>
#include <mari/stroke/pipeline.hpp>
#include <mari/test/harness.hpp>

#include "fake_tilemap.hpp"

#include <cmath>
#include <vector>

using namespace mari;
using namespace mari::stroke;
using mari::test::FakeTileMap;

namespace {

constexpr u64 kMs = 1'000'000ull;

RawInputEvent pen(f64 x, f64 y, u64 ms, f32 pressure = 1.0f) {
    RawInputEvent e{};
    e.x = x;
    e.y = y;
    e.timestampNs = ms * kMs;
    e.pressure = pressure;
    return e;
}

brush::MariBrushPreset roundPreset(f32 d, f32 spacing = 0.1f) {
    brush::MariBrushPreset p;
    p.name = "pipeline-round";
    p.tip.diameter = d;
    p.tip.hardness = 0.9f;
    p.spacing = spacing;
    return p;
}

brush::StrokeContext ctxFor(TileMap* m) {
    // A0: 출처는 생성 시점에 박힌다. 나중에 바꿀 방법은 없다.
    brush::StrokeContext c(StrokeSource::humanPen());
    c.target = m;
    c.color = Color8::rgba(20, 40, 60, 255);
    c.seed = 7;
    return c;
}

/// 사선 하나를 긋고 파이프라인을 돌려준다.
struct Run {
    FakeTileMap map;
    brush::BrushEnginePtr engine;
    std::vector<TileCoord> dirty;
    Rect bounds{};
    usize stamps = 0;
};

void drawDiagonal(StrokePipeline& pipe, FakeTileMap& map, SmoothingMode mode, int steps = 24) {
    StrokeConfig cfg;
    cfg.smoothing = mode;
    pipe.setConfig(cfg);
    const auto r = pipe.begin(ctxFor(&map), pen(30, 30, 0));
    (void)r; // 시작 실패는 호출한 테스트가 결과로 알아챈다
    for (int i = 1; i <= steps; ++i) {
        const f64 t = static_cast<f64>(i);
        // 떨림을 얹은 사선.
        pipe.extend(pen(30.0 + t * 8.0, 30.0 + t * 6.0 + ((i % 2) ? 2.0 : -2.0),
                        static_cast<u64>(i) * 5));
    }
    pipe.end();
}

} // namespace

MARI_TEST(pipeline_draws_and_reports_exactly_what_it_painted) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(roundPreset(12.0f), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;
    drawDiagonal(pipe, map, SmoothingMode::Off);

    const DirtyTiles& dirty = pipe.dirtyTiles();
    CHECK(pipe.stampCount() > 20u);
    CHECK(!dirty.empty());
    // 과대·과소 둘 다 아님: 더티 목록 == 실제로 잉크가 닿은 타일.
    CHECK(dirty == map.paintedTiles());
    CHECK_EQ(map.tileCount(), dirty.size()); // 빈 타일을 만들지 않는다

    // 경계 상자는 그린 픽셀을 전부 담되, 타일 한 칸 밖으로는 못 나간다.
    const Rect painted = map.paintedBounds();
    const Rect box = pipe.dirtyBounds();
    CHECK(!painted.isEmpty());
    CHECK(box.intersected(painted) == painted);
    CHECK(painted.x - box.x < kTileSize);
    CHECK(box.right() - painted.right() < kTileSize);
    CHECK(painted.y - box.y < kTileSize);
    CHECK(box.bottom() - painted.bottom() < kTileSize);
}

MARI_TEST(pipeline_dirty_list_is_deduplicated_and_sorted) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(roundPreset(8.0f, 0.05f), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;
    drawDiagonal(pipe, map, SmoothingMode::Off);

    const DirtyTiles& dirty = pipe.dirtyTiles();
    for (usize i = 1; i < dirty.size(); ++i) {
        const bool ordered =
            (dirty[i - 1].ty < dirty[i].ty) ||
            (dirty[i - 1].ty == dirty[i].ty && dirty[i - 1].tx < dirty[i].tx);
        CHECK(ordered); // 정렬 + 중복 없음
    }
    // 스탬프는 수백 개인데 타일은 몇 개뿐이어야 한다(스탬프마다 갱신하면 안 된다).
    CHECK(dirty.size() < pipe.stampCount());
}

MARI_TEST(pipeline_clear_dirty_keeps_capacity) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(roundPreset(10.0f), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;
    drawDiagonal(pipe, map, SmoothingMode::Off);
    CHECK(!pipe.dirtyTiles().empty());
    pipe.clearDirty();
    CHECK(pipe.dirtyTiles().empty());
    CHECK(pipe.dirtyBounds().isEmpty());
}

MARI_TEST(pipeline_smoothing_changes_the_result) {
    // 끄고/켜고가 같은 그림을 내면 스무딩이 동작하지 않는 것이다.
    const auto draw = [](SmoothingMode m) -> Rect {
        auto e = makeNativeEngine();
        (void)e.value()->setPreset(roundPreset(6.0f, 0.1f), nullptr);
        StrokePipeline pipe(e.value().get());
        FakeTileMap map;
        drawDiagonal(pipe, map, m);
        return map.paintedBounds();
    };
    const Rect off = draw(SmoothingMode::Off);
    const Rect strong = draw(SmoothingMode::Strong);
    CHECK(!(off == strong));
    CHECK(strong.height < off.height); // 떨림이 깎여 세로 폭이 준다
}

MARI_TEST(pipeline_pressure_ramp_changes_stroke_width) {
    brush::MariBrushPreset p = roundPreset(30.0f, 0.05f);
    brush::DynamicLink d;
    d.input = brush::DynamicInput::Pressure;
    d.output = brush::DynamicOutput::Size;
    p.dynamics.push_back(d);

    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(p, nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;

    // 왼쪽(약)에서 오른쪽(강)으로 필압을 올리며 가로로 긋는다.
    CHECK(pipe.begin(ctxFor(&map), pen(100, 200, 0, 0.05f)).ok());
    for (int i = 1; i <= 40; ++i)
        pipe.extend(pen(100.0 + i * 5.0, 200.0, static_cast<u64>(i) * 4,
                        0.05f + static_cast<f32>(i) * 0.02375f));
    pipe.end();

    const auto thicknessAt = [&](i32 x) -> i32 {
        i32 n = 0;
        for (i32 y = 120; y < 280; ++y)
            if (map.pixelAt(x, y).a > 0)
                ++n;
        return n;
    };
    const i32 thin = thicknessAt(110);
    const i32 thick = thicknessAt(290);
    CHECK(thin > 0);
    CHECK(thick > thin * 3); // 필압이 20배면 굵기도 눈에 띄게 달라야 한다
}

MARI_TEST(pipeline_rejects_bad_setup_without_throwing) {
    StrokePipeline noEngine(nullptr);
    FakeTileMap map;
    const auto r = noEngine.begin(ctxFor(&map), pen(0, 0, 0));
    CHECK(!r.ok());
    CHECK_EQ(r.code(), ErrorCode::InvalidArgument);
    noEngine.extend(pen(1, 1, 1)); // 시작 안 했으면 아무 일도 없다
    noEngine.end();
    CHECK(!noEngine.active());

    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(roundPreset(10.0f), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    brush::StrokeContext bad(StrokeSource::humanPen());
    bad.target = nullptr;
    CHECK(!pipe.begin(bad, pen(0, 0, 0)).ok());
    CHECK_EQ(pipe.lastError().code, ErrorCode::InvalidArgument);
    CHECK(!pipe.active());
}

MARI_TEST(pipeline_single_tap_leaves_one_dot) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(roundPreset(10.0f), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;
    CHECK(pipe.begin(ctxFor(&map), pen(300, 300, 0)).ok());
    pipe.end();
    CHECK_EQ(pipe.stampCount(), 1u);
    CHECK_EQ(pipe.dirtyTiles().size(), 1u);
    CHECK(map.pixelAt(300, 300).a > 0);
}

MARI_TEST(pipeline_keeps_canvas_coordinates_intact) {
    // 좌표는 캔버스 좌표 그대로다. y 는 아래로 증가한다.
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(roundPreset(4.0f, 0.25f), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;
    CHECK(pipe.begin(ctxFor(&map), pen(-70, 500, 0)).ok()); // 캔버스 밖 음수 좌표도 허용
    pipe.extend(pen(-70, 540, 20));
    pipe.end();
    CHECK(map.pixelAt(-70, 500).a > 0);
    CHECK(map.pixelAt(-70, 540).a > 0);
    bool negativeTile = false;
    for (const TileCoord& c : pipe.dirtyTiles())
        if (c.tx < 0)
            negativeTile = true;
    CHECK(negativeTile);
}

MARI_TEST_MAIN()
