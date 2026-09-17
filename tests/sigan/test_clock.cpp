// docs/03 5.6 / 10절 `clock_jump` — 벽시계를 바꿔도 상대 시각은 불변이다.
#include <mari/sigan/clock.hpp>
#include <mari/test/harness.hpp>

using namespace mari;
using namespace mari::sigan;

namespace {
u64 g_steady = 0;
i64 g_wall = 1'700'000'000'000;
u64 fakeSteady() noexcept { return g_steady; }
i64 fakeWall() noexcept { return g_wall; }
int g_wallReads = 0;
i64 countingWall() noexcept {
    ++g_wallReads;
    return g_wall;
}
} // namespace

MARI_TEST(clock_jump_does_not_pollute_relative_time) {
    g_steady = 5'000'000'000ull; // 5초
    g_wall = 1'700'000'000'000;
    SessionClock c(ClockSource{&fakeSteady, &fakeWall});
    CHECK_EQ(c.wallAnchorUnixMs(), 1'700'000'000'000);

    g_steady += 16'000'000ull; // 16ms 진행
    CHECK_NEAR(c.elapsedMs(), 16.0, 1e-6);

    // NTP 가 그리는 중에 벽시계를 1시간 뒤로 점프시킨다.
    g_wall -= 3'600'000;
    g_steady += 16'000'000ull;
    CHECK_NEAR(c.elapsedMs(), 32.0, 1e-6); // 상대 시각은 오염되지 않는다
    CHECK_EQ(c.wallAnchorUnixMs(), 1'700'000'000'000); // 앵커도 그대로
}

MARI_TEST(wall_clock_is_read_exactly_once) {
    g_wallReads = 0;
    g_steady = 0;
    SessionClock c(ClockSource{&fakeSteady, &countingWall});
    for (int i = 0; i < 100; ++i) {
        g_steady += 1'000'000ull;
        (void)c.elapsedMs();
    }
    // 세션 시작 시 **한 번만** 읽는다(docs/03 5.6).
    CHECK_EQ(g_wallReads, 1);
}

MARI_TEST(session_clock_is_monotonic) {
    SessionClock c; // 진짜 시계
    const f64 a = c.elapsedMs();
    const f64 b = c.elapsedMs();
    CHECK(b >= a);
    CHECK(c.wallAnchorUnixMs() > 0);
}

MARI_TEST_MAIN()
