// 파이프라인 [3] 보간 — spacing 대로 찍히는가, 필압·틸트도 같이 보간되는가.
#include <mari/stroke/interpolate.hpp>
#include <mari/test/harness.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace mari;
using namespace mari::stroke;

namespace {

/// 간격만 아는 엔진. 보간기만 떼어 보기 위한 스텁이다.
class SpacingOnlyEngine final : public brush::IBrushEngine {
public:
    explicit SpacingOnlyEngine(f32 fixed) : fixed_(fixed) {}
    void setPressureScaled(bool v) { pressureScaled_ = v; }

    const char* name() const noexcept override { return "spacing-stub"; }
    Result<void> setPreset(const brush::MariBrushPreset&, brush::ImportReport*) override {
        return Ok();
    }
    Result<void> beginStroke(const brush::StrokeContext&) override { return Ok(); }
    void stamp(const brush::StampInput&, DirtyTiles&) noexcept override {}
    void endStroke(DirtyTiles&) noexcept override {}
    f32 spacingPx(f32 pressure) const noexcept override {
        return pressureScaled_ ? fixed_ * std::max(pressure, 0.05f) : fixed_;
    }

private:
    f32 fixed_;
    bool pressureScaled_ = false;
};

/// 스탬프를 전부 모아 두는 싱크.
struct Recorder final : IStampSink {
    std::vector<brush::StampInput> stamps;
    void onStamp(const brush::StampInput& s) noexcept override { stamps.push_back(s); }
};

InputSample sampleAt(f32 x, f32 y, f32 pressure = 1.0f, f64 t = 0.0) {
    InputSample s{};
    s.pos.x = x;
    s.pos.y = y;
    s.pressure = pressure;
    s.timeMs = t;
    return s;
}

f32 dist(const PointF& a, const PointF& b) {
    const f32 dx = b.x - a.x;
    const f32 dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

MARI_TEST(interpolate_places_stamps_at_spacing) {
    SpacingOnlyEngine engine(10.0f);
    Recorder rec;
    StrokeInterpolator it;

    it.begin(sampleAt(0, 0), engine, rec);
    it.push(sampleAt(100, 0), engine, rec);

    // 시작점 1개 + 10px 마다 10개 = 11개.
    CHECK_EQ(rec.stamps.size(), 11u);
    CHECK_NEAR(rec.stamps.front().pos.x, 0.0f, 1e-3);
    CHECK_NEAR(rec.stamps.back().pos.x, 100.0f, 0.05);
    for (usize i = 1; i < rec.stamps.size(); ++i) {
        CHECK_NEAR(dist(rec.stamps[i - 1].pos, rec.stamps[i].pos), 10.0f, 0.05);
        CHECK_NEAR(rec.stamps[i].pos.y, 0.0f, 1e-3);
    }
}

MARI_TEST(interpolate_spacing_carries_across_segments) {
    // 구간 경계에서 간격이 끊기거나 겹치면 선에 마디가 생긴다.
    SpacingOnlyEngine engine(7.0f);
    Recorder rec;
    StrokeInterpolator it;
    it.begin(sampleAt(0, 0), engine, rec);
    it.push(sampleAt(10, 0), engine, rec);
    it.push(sampleAt(20, 0), engine, rec);
    it.push(sampleAt(30, 0), engine, rec);

    CHECK(rec.stamps.size() >= 4u);
    for (usize i = 1; i < rec.stamps.size(); ++i)
        CHECK_NEAR(dist(rec.stamps[i - 1].pos, rec.stamps[i].pos), 7.0f, 0.1);
}

MARI_TEST(interpolate_spacing_follows_pressure) {
    // 필압이 낮으면 브러시가 가늘어지고 간격도 좁아져야 한다(엔진이 그렇게 말하면).
    SpacingOnlyEngine engine(20.0f);
    engine.setPressureScaled(true);
    Recorder rec;
    StrokeInterpolator it;
    it.begin(sampleAt(0, 0, 1.0f), engine, rec);
    it.push(sampleAt(200, 0, 0.1f), engine, rec);

    CHECK(rec.stamps.size() > 11u); // 균일 20px 이면 11개, 가늘어지므로 더 많다
    const f32 first = dist(rec.stamps[0].pos, rec.stamps[1].pos);
    const f32 last = dist(rec.stamps[rec.stamps.size() - 2].pos, rec.stamps.back().pos);
    CHECK(last < first);
}

MARI_TEST(interpolate_carries_pressure_and_tilt) {
    SpacingOnlyEngine engine(10.0f);
    Recorder rec;
    StrokeInterpolator it;

    InputSample a = sampleAt(0, 0, 0.0f, 0.0);
    InputSample b = sampleAt(100, 0, 1.0f, 50.0);
    a.tiltX = -1.0f;
    b.tiltX = 1.0f;
    a.azimuthDeg = 350.0f;
    b.azimuthDeg = 10.0f;

    it.begin(a, engine, rec);
    it.push(b, engine, rec);
    CHECK_EQ(rec.stamps.size(), 11u);

    // 필압·틸트·시간이 위치와 같이 따라 올라가야 한다.
    for (usize i = 1; i < rec.stamps.size(); ++i) {
        CHECK(rec.stamps[i].pressure > rec.stamps[i - 1].pressure);
        CHECK(rec.stamps[i].tiltX > rec.stamps[i - 1].tiltX);
        CHECK(rec.stamps[i].timeMs >= rec.stamps[i - 1].timeMs);
    }
    CHECK_NEAR(rec.stamps[5].pressure, 0.5f, 0.02);
    CHECK_NEAR(rec.stamps[5].tiltX, 0.0f, 0.04);
    CHECK_NEAR(rec.stamps.back().timeMs, 50.0, 0.5);
    // 방위각은 짧은 쪽으로 돈다: 350° → 10° 는 0° 를 지나간다(180° 근처가 아니다).
    const f32 mid = rec.stamps[5].azimuth;
    CHECK(mid > 355.0f || mid < 5.0f);
}

MARI_TEST(interpolate_fade_reaches_one_after_fade_length) {
    SpacingOnlyEngine engine(10.0f);
    Recorder rec;
    StrokeInterpolator it;
    InterpolateConfig cfg;
    cfg.fadeLengthPx = 50.0f;
    it.setConfig(cfg);

    it.begin(sampleAt(0, 0), engine, rec);
    it.push(sampleAt(100, 0), engine, rec);
    CHECK_NEAR(rec.stamps.front().fade, 0.0f, 1e-6);
    CHECK_NEAR(rec.stamps[5].fade, 1.0f, 1e-6); // 50px 지점
    CHECK_NEAR(rec.stamps.back().fade, 1.0f, 1e-6);
}

MARI_TEST(interpolate_single_point_still_makes_a_dot) {
    // 펜을 대고 바로 떼면 점 하나가 남아야 한다.
    SpacingOnlyEngine engine(10.0f);
    Recorder rec;
    StrokeInterpolator it;
    it.begin(sampleAt(5, 7), engine, rec);
    it.finish();
    CHECK_EQ(rec.stamps.size(), 1u);
    CHECK_NEAR(rec.stamps[0].pos.x, 5.0f, 1e-6);
    CHECK_NEAR(rec.stamps[0].pos.y, 7.0f, 1e-6);
}

MARI_TEST(interpolate_curve_bulges_off_the_chord) {
    // 꺾인 경로에서는 스플라인이 직선 두 개(꺾은선)보다 바깥으로 부푼다.
    // 첫 구간은 제어점이 모자라 직선이므로, 두 번째 구간 (50,50)→(100,0) 을 본다.
    SpacingOnlyEngine engine(2.0f);
    Recorder rec;
    StrokeInterpolator it;
    it.begin(sampleAt(0, 0), engine, rec);
    it.push(sampleAt(50, 50), engine, rec);
    it.push(sampleAt(100, 0), engine, rec);

    f32 maxOff = 0.0f;
    for (const auto& st : rec.stamps)
        if (st.pos.x > 60.0f && st.pos.x < 90.0f)
            maxOff = std::max(maxOff, st.pos.y - (100.0f - st.pos.x)); // 현(chord) 위쪽
    CHECK(maxOff > 1.0f);
}

MARI_TEST(catmull_rom_passes_through_control_points) {
    const PointF p0{0, 0}, p1{10, 0}, p2{20, 10}, p3{30, 10};
    const PointF a = catmullRom(p0, p1, p2, p3, 0.0f);
    const PointF b = catmullRom(p0, p1, p2, p3, 1.0f);
    CHECK_NEAR(a.x, p1.x, 1e-3);
    CHECK_NEAR(a.y, p1.y, 1e-3);
    CHECK_NEAR(b.x, p2.x, 1e-3);
    CHECK_NEAR(b.y, p2.y, 1e-3);
}

MARI_TEST(catmull_rom_handles_duplicate_points) {
    // 같은 좌표가 겹쳐 들어와도 NaN 이 나오면 안 된다(펜이 멈춰 있을 때 실제로 생긴다).
    const PointF p{5, 5};
    const PointF r = catmullRom(p, p, p, p, 0.5f);
    CHECK(std::isfinite(r.x));
    CHECK(std::isfinite(r.y));
    CHECK_NEAR(r.x, 5.0f, 1e-4);
}

MARI_TEST_MAIN()
