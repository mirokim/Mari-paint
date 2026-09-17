// 스트로크 핫 패스 벤치마크 + **핫 패스 무할당** 검사.
//
// docs/02 8절: 펜 입력 → 화면 표시 16ms. 여기서는 [1]~[4] 의 비용만 잰다.
// 시간은 기계마다 다르므로 실패 기준으로 쓰지 않고 **회귀 감시용 기준선**으로 찍는다.
// 대신 "스탬프 하나가 16ms 를 통째로 먹는다" 같은 명백한 붕괴는 잡는다.
#include <mari/stroke/native_engine.hpp>
#include <mari/stroke/pipeline.hpp>
#include <mari/test/harness.hpp>

#include "fake_tilemap.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace mari;
using namespace mari::stroke;
using mari::test::FakeTileMap;

namespace {

/// 전역 할당 계수기. 이 실행파일 전체에 걸린다.
std::size_t g_allocs = 0;
bool g_counting = false;

struct AllocGuard {
    AllocGuard() {
        g_allocs = 0;
        g_counting = true;
    }
    ~AllocGuard() { g_counting = false; }
    [[nodiscard]] std::size_t count() const { return g_allocs; }
};

f64 msSince(const std::chrono::steady_clock::time_point& t0) {
    const auto dt = std::chrono::steady_clock::now() - t0;
    return std::chrono::duration<double, std::milli>(dt).count();
}

brush::MariBrushPreset benchPreset() {
    brush::MariBrushPreset p;
    p.name = "bench-round";
    p.tip.diameter = 24.0f;
    p.tip.hardness = 0.7f;
    p.spacing = 0.1f;
    brush::DynamicLink d;
    d.input = brush::DynamicInput::Pressure;
    d.output = brush::DynamicOutput::Size;
    p.dynamics.push_back(d);
    return p;
}

brush::StrokeContext ctxFor(TileMap* m) {
    brush::StrokeContext c(StrokeSource::humanPen());
    c.target = m;
    c.color = Color8::rgba(10, 20, 30, 255);
    c.seed = 2026;
    return c;
}

RawInputEvent pen(f64 x, f64 y, u64 ms, f32 pressure) {
    RawInputEvent e{};
    e.x = x;
    e.y = y;
    e.timestampNs = ms * 1'000'000ull;
    e.pressure = pressure;
    return e;
}

/// 지그재그로 캔버스를 훑는 획 하나.
void sweep(StrokePipeline& pipe, FakeTileMap& map, int events) {
    (void)pipe.begin(ctxFor(&map), pen(50, 50, 0, 0.2f));
    for (int i = 1; i <= events; ++i) {
        const f64 t = static_cast<f64>(i);
        const f64 x = 50.0 + t * 3.0;
        const f64 y = 300.0 + 200.0 * ((i % 40 < 20) ? (i % 40) / 20.0 : 1.0 - (i % 40) / 20.0);
        pipe.extend(pen(x, y, static_cast<u64>(i) * 5, 0.2f + 0.6f * static_cast<f32>(i % 10) / 9.0f));
    }
    pipe.end();
}

} // namespace

// 전역 new/delete 를 갈아끼워 할당 횟수를 센다.
// malloc/free 를 함수 포인터로 숨긴다 — 그러지 않으면 컴파일러가 new/free 쌍을
// 짝이 안 맞는 것으로 보고 경고한다(-Wmismatched-new-delete). 경고 0이 프로젝트 기준이다.
namespace {
void* (*volatile g_malloc)(std::size_t) = &std::malloc;
void (*volatile g_free)(void*) = &std::free;
} // namespace

void* operator new(std::size_t n) {
    if (g_counting)
        ++g_allocs;
    void* p = g_malloc(n ? n : 1);
    if (p == nullptr)
        throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { g_free(p); }
void operator delete[](void* p) noexcept { g_free(p); }
void operator delete(void* p, std::size_t) noexcept { g_free(p); }
void operator delete[](void* p, std::size_t) noexcept { g_free(p); }

MARI_TEST(bench_10k_stamps) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(benchPreset(), nullptr).ok());
    FakeTileMap map;
    DirtyTiles dirty;
    dirty.reserve(4096);
    CHECK(e.value()->beginStroke(ctxFor(&map)).ok());

    constexpr int kStamps = 10000;
    // 워밍업(타일 할당 비용을 측정에서 빼기 위해 같은 영역을 한 번 훑는다).
    for (int i = 0; i < kStamps; ++i) {
        brush::StampInput in;
        in.pos = PointF{20.0f + static_cast<f32>(i % 1000), 20.0f + static_cast<f32>(i / 1000) * 30.0f};
        in.pressure = 0.5f;
        e.value()->stamp(in, dirty);
    }
    dirty.clear();

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kStamps; ++i) {
        brush::StampInput in;
        in.pos = PointF{20.0f + static_cast<f32>(i % 1000), 20.0f + static_cast<f32>(i / 1000) * 30.0f};
        in.pressure = 0.25f + 0.75f * static_cast<f32>(i % 100) / 99.0f;
        e.value()->stamp(in, dirty);
    }
    const f64 ms = msSince(t0);
    e.value()->endStroke(dirty);

    std::printf("  [BENCH] 스탬프 %d개 (지름 24px): %.2f ms, 스탬프당 %.0f ns, %.0f 스탬프/초\n",
                kStamps, ms, ms * 1e6 / kStamps, kStamps / (ms / 1000.0));
    CHECK(ms > 0.0);
    CHECK(ms < 5000.0); // 명백한 붕괴만 잡는다
}

MARI_TEST(bench_full_pipeline_per_event) {
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(benchPreset(), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;

    constexpr int kEvents = 2000; // 초당 200점이면 10초짜리 획
    sweep(pipe, map, kEvents); // 워밍업 = 타일 확보
    pipe.clearDirty();

    const auto t0 = std::chrono::steady_clock::now();
    sweep(pipe, map, kEvents);
    const f64 ms = msSince(t0);

    const usize stamps = pipe.stampCount();
    std::printf("  [BENCH] 파이프라인 [1]~[4]: 이벤트 %d개 → 스탬프 %zu개, %.2f ms "
                "(이벤트당 %.1f us, 스탬프당 %.0f ns)\n",
                kEvents, stamps, ms, ms * 1000.0 / kEvents,
                stamps ? ms * 1e6 / static_cast<f64>(stamps) : 0.0);
    CHECK(stamps > 0u);
    // 이벤트 하나가 한 프레임(16ms)을 통째로 먹으면 제품이 죽는다.
    CHECK(ms / kEvents < 16.0);
}

MARI_TEST(hot_path_does_not_allocate) {
    // 🔴 핫 패스 규칙 검증: 이미 타일이 잡힌 영역을 다시 그으면 **할당이 0** 이어야 한다.
    // (첫 획은 TileMap 이 타일을 만드느라 할당한다 — 그건 타일맵의 몫이다.)
    auto e = makeNativeEngine();
    CHECK(e.value()->setPreset(benchPreset(), nullptr).ok());
    StrokePipeline pipe(e.value().get());
    FakeTileMap map;

    sweep(pipe, map, 800); // 1차: 타일을 만든다
    pipe.clearDirty();
    CHECK(map.tileCount() > 0u);

    std::size_t allocs = 0;
    {
        AllocGuard guard;
        sweep(pipe, map, 800); // 2차: 같은 경로 — 새 타일도, 새 버퍼도 필요 없다
        allocs = guard.count();
    }
    std::printf("  [BENCH] 2차 획(800 이벤트, 스탬프 %zu개) 힙 할당: %zu회\n", pipe.stampCount(),
                allocs);
    CHECK_EQ(allocs, 0u);
}

MARI_TEST_MAIN()
