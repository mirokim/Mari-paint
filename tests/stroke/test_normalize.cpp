// 파이프라인 [1] 정규화 — 장치 독립성과 **시계 방어**를 검사한다.
#include <mari/stroke/input.hpp>
#include <mari/test/harness.hpp>

using namespace mari;
using namespace mari::stroke;

namespace {

RawInputEvent ev(f64 x, f64 y, u64 ns, f32 p = 1.0f, f32 pmax = 1.0f) {
    RawInputEvent e{};
    e.x = x;
    e.y = y;
    e.timestampNs = ns;
    e.pressure = p;
    e.pressureMax = pmax;
    return e;
}

constexpr u64 kMs = 1'000'000ull;

} // namespace

MARI_TEST(normalize_device_pressure_range) {
    InputNormalizer n;
    // Wintab 1023 단계 장치.
    const InputSample s = n.begin(ev(0, 0, 0, 511.5f, 1023.0f));
    CHECK_NEAR(s.pressure, 0.5f, 1e-4);
    // 범위를 넘겨도 잘린다.
    const InputSample s2 = n.normalize(ev(0, 0, kMs, 2000.0f, 1023.0f));
    CHECK_NEAR(s2.pressure, 1.0f, 1e-6);
}

MARI_TEST(normalize_mouse_has_full_pressure) {
    InputNormalizer n;
    RawInputEvent e = ev(10, 10, 0);
    e.hasPressure = false;
    e.pressure = 0.0f;
    CHECK_NEAR(n.begin(e).pressure, 1.0f, 1e-6);
}

MARI_TEST(normalize_tilt_to_unit_range) {
    InputNormalizer n;
    NormalizeConfig c;
    c.maxTiltDeg = 60.0f;
    n.setConfig(c);
    RawInputEvent e = ev(0, 0, 0);
    e.tiltXDeg = 30.0f;
    e.tiltYDeg = -90.0f; // 범위를 넘는다
    const InputSample s = n.begin(e);
    CHECK_NEAR(s.tiltX, 0.5f, 1e-5);
    CHECK_NEAR(s.tiltY, -1.0f, 1e-6); // 잘린다
}

MARI_TEST(normalize_time_is_relative_and_monotonic) {
    InputNormalizer n;
    const InputSample a = n.begin(ev(0, 0, 5'000'000'000ull)); // 기준이 5초여도
    CHECK_NEAR(a.timeMs, 0.0, 1e-9);                           // 상대 0ms 에서 시작한다
    const InputSample b = n.normalize(ev(1, 0, 5'000'000'000ull + 8 * kMs));
    CHECK_NEAR(b.timeMs, 8.0, 1e-6);
}

MARI_TEST(normalize_survives_clock_jumping_backwards) {
    // docs/03 5.6 — 시계가 뒤로 뛰어도 획 시간은 절대 줄지 않는다.
    InputNormalizer n;
    n.begin(ev(0, 0, 10'000 * kMs));
    const InputSample a = n.normalize(ev(1, 0, 10'010 * kMs));
    const InputSample b = n.normalize(ev(2, 0, 9'000 * kMs)); // NTP 점프
    const InputSample c = n.normalize(ev(3, 0, 9'005 * kMs));
    CHECK(b.timeMs >= a.timeMs);
    CHECK(c.timeMs >= b.timeMs);
    CHECK_NEAR(b.timeMs, a.timeMs, 1e-9); // 뒤로 간 구간은 0ms 로 친다
    CHECK_NEAR(c.timeMs, b.timeMs + 5.0, 1e-6);
}

MARI_TEST(normalize_clamps_absurd_forward_jump) {
    InputNormalizer n;
    NormalizeConfig c;
    c.maxDeltaMs = 100.0;
    n.setConfig(c);
    n.begin(ev(0, 0, 0));
    const InputSample s = n.normalize(ev(1, 0, 60'000 * kMs)); // 1분 점프
    CHECK_NEAR(s.timeMs, 100.0, 1e-6);
}

MARI_TEST(normalize_velocity_grows_with_speed) {
    NormalizeConfig cfg;
    cfg.velocitySmoothing = 1.0f; // 평활 없이 바로 본다
    cfg.velocityRefPxPerMs = 2.0f;

    InputNormalizer slow;
    slow.setConfig(cfg);
    slow.begin(ev(0, 0, 0));
    const f32 vSlow = slow.normalize(ev(2, 0, 10 * kMs)).velocity; // 0.2px/ms

    InputNormalizer fast;
    fast.setConfig(cfg);
    fast.begin(ev(0, 0, 0));
    const f32 vFast = fast.normalize(ev(40, 0, 10 * kMs)).velocity; // 4px/ms → 상한

    CHECK_NEAR(vSlow, 0.1f, 1e-5);
    CHECK_NEAR(vFast, 1.0f, 1e-6);
    CHECK(vFast > vSlow);
}

MARI_TEST(monotonic_clock_moves_forward) {
    const u64 a = monotonicNowNs();
    const u64 b = monotonicNowNs();
    CHECK(b >= a);
    CHECK(a > 0);
}

MARI_TEST_MAIN()
