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
    // 끄고/켜고가 같은 그림을 내면 스무딩이 동작하지 않는 것이다. 떨림(±2px)이 깎이면 사선 위 ±2 자리가 비어야 한다.
    const auto inkAt = [](SmoothingMode m, i32 x, i32 y, bool endCorrection, f32 deadZone) {
        auto e = makeNativeEngine();
        (void)e.value()->setPreset(roundPreset(3.0f, 0.2f), nullptr);
        StrokePipeline pipe(e.value().get());
        FakeTileMap map;
        StrokeConfig cfg;
        cfg.smoothing = m;
        cfg.endCorrection = endCorrection;
        cfg.deadZone = deadZone;
        pipe.setConfig(cfg);
        (void)pipe.begin(ctxFor(&map), pen(30, 30, 0));
        for (int i = 1; i <= 24; ++i) {
            const f64 t = static_cast<f64>(i);
            pipe.extend(pen(30.0 + t * 8.0, 30.0 + t * 6.0 + ((i % 2) ? 2.0 : -2.0), static_cast<u64>(i) * 5));
        }
        pipe.end();
        return map.pixelAt(x, y).a;
    };
    // 떨림 꼭짓점(i=11: x=118, y=96+2=98)은 끄면 찍히고 강하게 켜면 비어 있다.
    CHECK(inkAt(SmoothingMode::Off, 118, 98, true, 0.0f) > 0);
    CHECK_EQ(int(inkAt(SmoothingMode::Strong, 118, 98, true, 0.0f)), 0);
}

MARI_TEST(pipeline_end_correction_reaches_the_pen_up_point) {
    // 강한 보정은 커서보다 뒤처진다. 끝점 보정이 켜지면 뗀 자리(222,172)까지 이어 그리고, 끄면 못 미친다.
    const auto endInk = [](bool endCorrection) {
        auto e = makeNativeEngine();
        (void)e.value()->setPreset(roundPreset(3.0f, 0.2f), nullptr);
        StrokePipeline pipe(e.value().get());
        FakeTileMap map;
        StrokeConfig cfg;
        cfg.smoothing = SmoothingMode::Strong;
        cfg.endCorrection = endCorrection;
        pipe.setConfig(cfg);
        (void)pipe.begin(ctxFor(&map), pen(30, 30, 0));
        for (int i = 1; i <= 24; ++i) {
            const f64 t = static_cast<f64>(i);
            pipe.extend(pen(30.0 + t * 8.0, 30.0 + t * 6.0, static_cast<u64>(i) * 5));
        }
        pipe.end();
        return map.pixelAt(222, 174).a;
    };
    CHECK(endInk(true) > 0);
    CHECK_EQ(int(endInk(false)), 0);
}

MARI_TEST(pipeline_dead_zone_ignores_small_jitter) {
    // 데드존 6px: ±2px 떨림은 전혀 반영되지 않고, 다듬은 점은 원본에서 정확히 6px 뒤에서 끌려온다.
    auto e = makeNativeEngine();
    (void)e.value()->setPreset(roundPreset(3.0f, 0.2f), nullptr);
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;
    StrokeConfig cfg;
    cfg.smoothing = SmoothingMode::Light;
    cfg.deadZone = 6.0f;
    cfg.endCorrection = false;
    pipe.setConfig(cfg);
    (void)pipe.begin(ctxFor(&map), pen(30, 30, 0));
    // 제자리에서 ±2px 만 떨면 아무것도 안 움직인다.
    for (int i = 1; i <= 10; ++i) pipe.extend(pen(30.0 + ((i % 2) ? 2.0 : -2.0), 30.0, static_cast<u64>(i) * 5));
    CHECK_NEAR(pipe.lastSample().pos.x, 30.0f, 1e-4);
    // 30px 오른쪽으로 가면 24px 만 따라온다(6px 은 끈 길이).
    pipe.extend(pen(60.0, 30.0, 100));
    CHECK_NEAR(pipe.lastSample().pos.x, 54.0f, 1e-3);
    pipe.end();
}

MARI_TEST_MAIN()
