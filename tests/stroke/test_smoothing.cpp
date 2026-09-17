// 파이프라인 [2] 스무딩 — 끌 수 있는가, 켜면 정말 다른 경로가 나오는가.
#include <mari/stroke/smoothing.hpp>
#include <mari/test/harness.hpp>

#include <cmath>
#include <vector>

using namespace mari;
using namespace mari::stroke;

namespace {

/// 직선 위에 톱니 떨림을 얹은 입력.
std::vector<InputSample> jittery(int n) {
    std::vector<InputSample> v;
    v.reserve(static_cast<usize>(n));
    for (int i = 0; i < n; ++i) {
        InputSample s{};
        s.pos.x = static_cast<f32>(i) * 4.0f;
        s.pos.y = (i % 2 == 0) ? 3.0f : -3.0f; // 떨림
        s.pressure = 0.5f;
        s.timeMs = static_cast<f64>(i) * 5.0;
        v.push_back(s);
    }
    return v;
}

f32 pathLength(const std::vector<InputSample>& v) {
    f32 len = 0.0f;
    for (usize i = 1; i < v.size(); ++i) {
        const f32 dx = v[i].pos.x - v[i - 1].pos.x;
        const f32 dy = v[i].pos.y - v[i - 1].pos.y;
        len += std::sqrt(dx * dx + dy * dy);
    }
    return len;
}

std::vector<InputSample> run(SmoothingMode m, const std::vector<InputSample>& in) {
    Smoother sm;
    sm.setMode(m);
    std::vector<InputSample> out;
    out.reserve(in.size());
    out.push_back(sm.reset(in.front()));
    for (usize i = 1; i < in.size(); ++i)
        out.push_back(sm.smooth(in[i]));
    return out;
}

} // namespace

MARI_TEST(smoothing_off_is_exact_identity) {
    const auto in = jittery(20);
    const auto out = run(SmoothingMode::Off, in);
    CHECK_EQ(out.size(), in.size());
    for (usize i = 0; i < in.size(); ++i) {
        CHECK(out[i].pos == in[i].pos); // 근사가 아니라 **그대로**여야 한다
        CHECK_EQ(out[i].pressure, in[i].pressure);
    }
}

MARI_TEST(smoothing_on_takes_a_different_path) {
    const auto in = jittery(20);
    const auto off = run(SmoothingMode::Off, in);
    const auto on = run(SmoothingMode::Medium, in);

    bool differs = false;
    for (usize i = 0; i < in.size(); ++i)
        if (!(off[i].pos == on[i].pos))
            differs = true;
    CHECK(differs);
    // 떨림이 깎였으니 경로가 짧아져야 한다.
    CHECK(pathLength(on) < pathLength(off));
}

MARI_TEST(smoothing_strength_is_ordered) {
    const auto in = jittery(40);
    const f32 lenOff = pathLength(run(SmoothingMode::Off, in));
    const f32 lenLight = pathLength(run(SmoothingMode::Light, in));
    const f32 lenMedium = pathLength(run(SmoothingMode::Medium, in));
    const f32 lenStrong = pathLength(run(SmoothingMode::Strong, in));
    CHECK(lenLight < lenOff);
    CHECK(lenMedium < lenLight);
    CHECK(lenStrong < lenMedium);
}

MARI_TEST(smoothing_never_touches_time_or_velocity) {
    // 시간을 다듬으면 Sigan 정본 기록이 왜곡된다. 위치만 만진다.
    auto in = jittery(10);
    for (usize i = 0; i < in.size(); ++i)
        in[i].velocity = static_cast<f32>(i) * 0.05f;
    const auto on = run(SmoothingMode::Strong, in);
    for (usize i = 0; i < in.size(); ++i) {
        CHECK_EQ(on[i].timeMs, in[i].timeMs);
        CHECK_EQ(on[i].velocity, in[i].velocity);
    }
}

MARI_TEST(smoothing_lags_behind_a_step_input) {
    // 강한 스무딩은 계단 입력을 바로 따라가지 못한다(= 지연이 있다. 그래서 끌 수 있어야 한다).
    InputSample a{};
    InputSample b{};
    b.pos.x = 100.0f;

    Smoother sm;
    sm.setMode(SmoothingMode::Strong);
    sm.reset(a);
    const f32 x = sm.smooth(b).pos.x;
    CHECK(x > 0.0f);
    CHECK(x < 100.0f);
    CHECK_NEAR(x, 100.0f * smoothingAlpha(SmoothingMode::Strong), 1e-3);
}

MARI_TEST(smoothing_mode_names_are_stable) {
    CHECK_EQ(std::string(smoothingModeName(SmoothingMode::Off)), std::string("off"));
    CHECK_EQ(std::string(smoothingModeName(SmoothingMode::Strong)), std::string("strong"));
}

MARI_TEST_MAIN()
