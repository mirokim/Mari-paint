// 파이프라인 [3] 보간 구현. 헤더: include/mari/stroke/interpolate.hpp
#include <mari/stroke/interpolate.hpp>

#include <algorithm>
#include <cmath>

namespace mari::stroke {
namespace {

constexpr f32 kAlpha = 0.5f;    ///< 중심(centripetal) 파라미터화
constexpr f32 kEps = 1e-6f;
constexpr int kMaxSteps = 1024;  ///< 구간 하나를 쪼개는 최대 조각 수

[[nodiscard]] f32 dist(const PointF& a, const PointF& b) noexcept {
    const f32 dx = b.x - a.x;
    const f32 dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] PointF lerpPt(const PointF& a, const PointF& b, f32 t) noexcept {
    return PointF{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

[[nodiscard]] f32 lerpf(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

/// 각도 보간 — 359° → 1° 가 뒤로 358° 도는 것을 막는다.
[[nodiscard]] f32 lerpDeg(f32 a, f32 b, f32 t) noexcept {
    f32 d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    f32 v = std::fmod(a + d * t, 360.0f);
    if (v < 0.0f)
        v += 360.0f;
    return v;
}

/// 중심 파라미터화 Catmull-Rom 구간 하나. 제어점 4개의 매듭을 미리 계산해 둔다.
struct Spline {
    PointF p[4];
    f32 t[4];

    void build(const PointF& p0, const PointF& p1, const PointF& p2, const PointF& p3) noexcept {
        p[0] = p0;
        p[1] = p1;
        p[2] = p2;
        p[3] = p3;
        t[0] = 0.0f;
        for (int i = 1; i < 4; ++i) {
            const f32 d = std::pow(std::max(dist(p[i - 1], p[i]), kEps), kAlpha);
            t[i] = t[i - 1] + d;
        }
    }

    /// u 는 p1→p2 구간의 0..1.
    [[nodiscard]] PointF at(f32 u) const noexcept {
        const f32 tt = t[1] + (t[2] - t[1]) * u;
        const auto seg = [&](int i, int j) noexcept -> PointF {
            const f32 den = t[j] - t[i];
            const f32 w = den > kEps ? (tt - t[i]) / den : 0.0f;
            return lerpPt(p[i], p[j], w);
        };
        const PointF a1 = seg(0, 1);
        const PointF a2 = seg(1, 2);
        const PointF a3 = seg(2, 3);
        const auto mix = [&](const PointF& x, const PointF& y, int i, int j) noexcept -> PointF {
            const f32 den = t[j] - t[i];
            const f32 w = den > kEps ? (tt - t[i]) / den : 0.0f;
            return lerpPt(x, y, w);
        };
        const PointF b1 = mix(a1, a2, 0, 2);
        const PointF b2 = mix(a2, a3, 1, 3);
        const f32 den = t[2] - t[1];
        const f32 w = den > kEps ? (tt - t[1]) / den : 0.0f;
        return lerpPt(b1, b2, w);
    }
};

} // namespace

PointF catmullRom(const PointF& p0, const PointF& p1, const PointF& p2, const PointF& p3,
                  f32 t) noexcept {
    Spline s;
    s.build(p0, p1, p2, p3);
    return s.at(std::clamp(t, 0.0f, 1.0f));
}

void StrokeInterpolator::emit(const InputSample& s, IStampSink& sink) noexcept {
    brush::StampInput in{};
    in.pos = s.pos;
    in.pressure = s.pressure;
    in.tiltX = s.tiltX;
    in.tiltY = s.tiltY;
    in.azimuth = s.azimuthDeg;
    in.rotation = s.rotationDeg;
    in.velocity = s.velocity;
    in.timeMs = s.timeMs;
    in.fade = cfg_.fadeLengthPx > 0.0f
                  ? std::clamp(traveled_ / cfg_.fadeLengthPx, 0.0f, 1.0f)
                  : 0.0f;
    ++emitted_;
    sink.onStamp(in);
}

void StrokeInterpolator::begin(const InputSample& first, const brush::IBrushEngine& engine,
                               IStampSink& sink) noexcept {
    prevPrev_ = first;
    prev_ = first;
    traveled_ = 0.0f;
    emitted_ = 0;
    started_ = true;
    // 핫 패스에서 할당하지 않으려고 여기서 한 번만 잡는다.
    pts_.reserve(kMaxSteps + 1);
    cum_.reserve(kMaxSteps + 1);

    // 펜을 대고 바로 떼면 점 하나가 남아야 한다.
    emit(first, sink);
    pending_ = std::clamp(engine.spacingPx(first.pressure), cfg_.minSpacingPx, cfg_.maxSpacingPx);
}

void StrokeInterpolator::push(const InputSample& s, const brush::IBrushEngine& engine,
                              IStampSink& sink) noexcept {
    if (!started_) {
        begin(s, engine, sink);
        return;
    }

    // 뒤쪽 제어점은 **외삽**한다. 다음 샘플을 기다리지 않으려고(지연 = 품질).
    const PointF p3{s.pos.x + (s.pos.x - prev_.pos.x), s.pos.y + (s.pos.y - prev_.pos.y)};
    Spline spline;
    spline.build(prevPrev_.pos, prev_.pos, s.pos, p3);

    const f32 chord = dist(prev_.pos, s.pos);
    const f32 stepLen = std::max(cfg_.minSpacingPx * 0.5f, 0.25f);
    const int steps = std::clamp(static_cast<int>(std::ceil(chord / stepLen)), 4, kMaxSteps);

    // 구간을 잘게 쪼개 **호길이**를 잰다. 스플라인 파라미터 u 는 호길이에 비례하지 않아서
    // (특히 스트로크 첫 구간처럼 제어점이 겹칠 때) u 로 필압을 섞으면 값이 밀린다.
    const usize n = static_cast<usize>(steps) + 1u;
    pts_.resize(n);
    cum_.resize(n);
    pts_[0] = spline.at(0.0f);
    cum_[0] = 0.0f;
    for (usize i = 1; i < n; ++i) {
        pts_[i] = spline.at(static_cast<f32>(i) / static_cast<f32>(steps));
        cum_[i] = cum_[i - 1] + dist(pts_[i - 1], pts_[i]);
    }
    const f32 total = cum_[n - 1];
    const f32 segStart = traveled_;

    if (total > kEps) {
        const f32 invTotal = 1.0f / total;
        int budget = cfg_.maxStampsPerSegment;
        usize k = 1;
        f32 target = pending_; // 구간 시작점에서 다음 스탬프까지의 호길이
        while (target <= total && budget > 0) {
            while (k + 1 < n && cum_[k] < target)
                ++k;
            const f32 span = cum_[k] - cum_[k - 1];
            const f32 w = span > kEps ? (target - cum_[k - 1]) / span : 0.0f;
            const PointF pos = lerpPt(pts_[k - 1], pts_[k], std::clamp(w, 0.0f, 1.0f));

            // 속성은 **호길이 비율**로 섞는다 — 위치와 어긋나지 않게.
            const f32 u = std::clamp(target * invTotal, 0.0f, 1.0f);
            InputSample cur{};
            cur.pos = pos;
            cur.pressure = std::clamp(lerpf(prev_.pressure, s.pressure, u), 0.0f, 1.0f);
            cur.tiltX = lerpf(prev_.tiltX, s.tiltX, u);
            cur.tiltY = lerpf(prev_.tiltY, s.tiltY, u);
            cur.azimuthDeg = lerpDeg(prev_.azimuthDeg, s.azimuthDeg, u);
            cur.rotationDeg = lerpDeg(prev_.rotationDeg, s.rotationDeg, u);
            cur.velocity = lerpf(prev_.velocity, s.velocity, u);
            cur.timeMs = prev_.timeMs + (s.timeMs - prev_.timeMs) * static_cast<f64>(u);

            traveled_ = segStart + target;
            emit(cur, sink);
            --budget;

            pending_ =
                std::clamp(engine.spacingPx(cur.pressure), cfg_.minSpacingPx, cfg_.maxSpacingPx);
            target += pending_;
        }
        pending_ = std::max(target - total, 0.0f);
        traveled_ = segStart + total;
    }

    prevPrev_ = prev_;
    prev_ = s;
}

void StrokeInterpolator::finish() noexcept { started_ = false; }

} // namespace mari::stroke
