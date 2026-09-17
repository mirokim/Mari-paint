// 파이프라인 [4] — 자체 최소 브러시 엔진. 헤더: include/mari/stroke/native_engine.hpp
//
// 설계 메모:
//   · 한 스탬프는 자기 경계 상자와 겹치는 타일만 건드린다. 캔버스 전체를 도는 경로는 없다.
//   · **커버리지가 0인 타일은 만들지도, 더티로 보고하지도 않는다.** 그래서 더티 목록이
//     "실제로 그린 범위"와 정확히 일치한다(과대 보고 금지 — docs/02 4절 [5][6]).
//   · stamp() 는 noexcept 이고 힙을 잡지 않는다. 커버리지 버퍼는 beginStroke() 에서 잡는다.
//     (TileMap::writable() 이 새 타일을 잡는 것은 불가피하며, 그쪽은 Result 로 실패를 준다.)
#include <mari/stroke/native_engine.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace mari::stroke {
namespace {

using namespace mari::brush;

constexpr f32 kMinDiameter = 0.2f;
constexpr f32 kMaxDiameter = 8192.0f;
/// 이보다 옅은 잉크는 8비트에서 아무 흔적도 남기지 못한다. 타일을 만들 이유가 없다.
constexpr f32 kInkEpsilon = 1.0f / 512.0f;

[[nodiscard]] f32 clamp01(f32 v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

/// 반응 곡선 평가. 점 0개 = 항등, 1개 = 상수, 그 외 구간 선형. 바깥은 끝값으로 잘린다.
[[nodiscard]] f32 evalCurve(const ResponseCurve& c, f32 x) noexcept {
    const auto& p = c.points;
    if (p.empty())
        return x;
    if (p.size() == 1)
        return p[0].y;
    if (x <= p.front().x)
        return p.front().y;
    if (x >= p.back().x)
        return p.back().y;
    for (usize i = 1; i < p.size(); ++i) {
        if (x <= p[i].x) {
            const f32 span = p[i].x - p[i - 1].x;
            if (span <= 1e-8f)
                return p[i].y;
            const f32 t = (x - p[i - 1].x) / span;
            return p[i - 1].y + (p[i].y - p[i - 1].y) * t;
        }
    }
    return p.back().y;
}

/// 재현 가능한 난수. 같은 시드 + 같은 입력 = 같은 획 (Sigan 대조에 필요하다).
struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    void seed(u64 v) noexcept { s = v ? v : 0x9E3779B97F4A7C15ull; }
    [[nodiscard]] u64 next() noexcept {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    /// 0..1
    [[nodiscard]] f32 unit() noexcept {
        return static_cast<f32>((next() >> 40)) * (1.0f / 16777216.0f);
    }
};

/// 한 스탬프의 동적 반응 결과.
struct Dynamics {
    f32 size = 1.0f;
    f32 opacity = 1.0f;
    f32 flow = 1.0f;
    f32 roundness = 1.0f;
    f32 rotationDeg = 0.0f;
    f32 scatter = 0.0f;
};

/// 자체 엔진.
class NativeEngine final : public IBrushEngine {
public:
    [[nodiscard]] const char* name() const noexcept override { return kNativeEngineName; }

    [[nodiscard]] Result<void> setPreset(const MariBrushPreset& preset,
                                         ImportReport* report) override {
        preset_ = preset;

        // 팁 검증 — 조용히 고치지 않고 리포트에 남긴다.
        if (!(preset_.tip.diameter > 0.0f)) {
            preset_.tip.diameter = 1.0f;
            if (report)
                report->add(ImportSeverity::Degraded, "tip/diameter",
                            "팁 지름이 0 이하라 1px 로 대체했습니다");
        }
        preset_.tip.diameter = std::clamp(preset_.tip.diameter, kMinDiameter, kMaxDiameter);
        preset_.tip.aspectRatio = std::clamp(preset_.tip.aspectRatio, 0.01f, 1.0f);
        preset_.tip.hardness = clamp01(preset_.tip.hardness);
        preset_.opacity = clamp01(preset_.opacity);
        preset_.flow = clamp01(preset_.flow);
        preset_.spacing = std::clamp(preset_.spacing, 0.005f, 10.0f);

        if (preset_.tip.kind == TipKind::Bitmap && preset_.tip.bitmap.empty()) {
            preset_.tip.kind = TipKind::Procedural;
            if (report)
                report->add(ImportSeverity::Degraded, "tip/bitmap",
                            "비트맵 팁이 비어 있어 절차적 원형으로 대체했습니다");
        }

        // 지원 블렌드 모드 밖이면 normal 로 떨어뜨리고 반드시 알린다.
        switch (preset_.blendMode) {
        case BlendMode::Normal:
        case BlendMode::Multiply:
        case BlendMode::Screen:
        case BlendMode::Add:
        case BlendMode::Erase:
            break;
        default:
            if (report)
                report->add(ImportSeverity::Degraded, "blendMode",
                            std::string("native 엔진은 '") + blendModeName(preset_.blendMode) +
                                "' 합성을 아직 못 해서 normal 로 그립니다");
            preset_.blendMode = BlendMode::Normal;
            break;
        }

        if (preset_.texture.has_value() && report)
            report->add(ImportSeverity::Dropped, "texture",
                        "종이/브러시 텍스처는 native 엔진이 아직 합성하지 않습니다");

        if (report)
            for (const auto& kv : preset_.extraParams)
                report->add(ImportSeverity::Info, kv.first,
                            "native 엔진이 모르는 파라미터라 무시했습니다");

        // 동적 반응을 출력별로 미리 갈라 둔다(핫 패스에서 훑지 않기 위해).
        for (auto& v : links_)
            v.clear();
        for (const auto& d : preset_.dynamics) {
            const auto oi = static_cast<usize>(d.output);
            if (oi < static_cast<usize>(kDynamicOutputCount))
                links_[oi].push_back(d);
        }
        hasPreset_ = true;
        return Ok();
    }

    [[nodiscard]] Result<void> beginStroke(const StrokeContext& ctx) override {
        if (!hasPreset_)
            return Err("프리셋이 없다. setPreset() 을 먼저 불러라", ErrorCode::InvalidArgument);
        if (ctx.target == nullptr)
            return Err("대상 타일맵이 없다", ErrorCode::InvalidArgument);
        if (ctx.target->format() != PixelFormat::RGBA8)
            return Err("native 엔진은 아직 RGBA8 만 그린다", ErrorCode::Unsupported);

        ctx_ = ctx;
        rng_.seed(ctx.seed);
        hasLast_ = false;
        active_ = true;
        // 핫 패스 버퍼는 여기서 잡는다. 타일 하나분(64×64)이면 충분하다.
        cov_.assign(static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize), 0.0f);
        return Ok();
    }

    void stamp(const StampInput& in, DirtyTiles& dirty) noexcept override {
        if (!active_)
            return;

        const Dynamics dyn = evalDynamics(in);

        f32 diameter = std::clamp(preset_.tip.diameter * dyn.size, 0.0f, kMaxDiameter);
        f32 alpha = clamp01(preset_.opacity * dyn.opacity) * clamp01(preset_.flow * dyn.flow);
        if (diameter < kMinDiameter || alpha < kInkEpsilon) {
            updateLast(in);
            return; // 잉크가 없으면 타일도 더티도 없다
        }

        // 흩뿌림 — 원판 안에서 균등하게.
        f32 cx = in.pos.x;
        f32 cy = in.pos.y;
        if (dyn.scatter > 0.0f) {
            const f32 ang = rng_.unit() * 6.2831853f;
            const f32 rad = std::sqrt(rng_.unit()) * dyn.scatter * diameter;
            cx += std::cos(ang) * rad;
            cy += std::sin(ang) * rad;
        }

        const f32 rx = diameter * 0.5f;
        const f32 ry = std::max(rx * std::clamp(preset_.tip.aspectRatio * dyn.roundness, 0.01f,
                                                1.0f),
                                kMinDiameter * 0.5f);
        const f32 theta = (preset_.tip.angle + dyn.rotationDeg) * 0.017453292f;
        const f32 ct = std::cos(theta);
        const f32 st = std::sin(theta);

        // 경계 상자 — 회전한 타원/사각형을 전부 덮는 보수적 반경 + AA 여백 1px.
        const f32 half = std::sqrt(rx * rx + ry * ry) + 1.0f;
        const i32 x0 = static_cast<i32>(std::floor(cx - half));
        const i32 y0 = static_cast<i32>(std::floor(cy - half));
        const i32 x1 = static_cast<i32>(std::ceil(cx + half)); // 배타적
        const i32 y1 = static_cast<i32>(std::ceil(cy + half));
        if (x1 <= x0 || y1 <= y0) {
            updateLast(in);
            return;
        }

        const f32 invRx = 1.0f / rx;
        const f32 invRy = 1.0f / ry;
        // 안티에일리어싱: 하드니스가 1이어도 최소 한 픽셀만큼은 부드럽게 끝낸다.
        const f32 aa = std::clamp(1.0f / std::max(rx, ry), 0.01f, 1.0f);
        const f32 feather = std::clamp(std::max(1.0f - preset_.tip.hardness, aa), 0.01f, 1.0f);
        const f32 inner = 1.0f - feather;

        for (i32 ty = tileIndexFor(y0); ty <= tileIndexFor(y1 - 1); ++ty) {
            for (i32 tx = tileIndexFor(x0); tx <= tileIndexFor(x1 - 1); ++tx) {
                const TileCoord tc{tx, ty};
                const i32 ox = tileOrigin(tx);
                const i32 oy = tileOrigin(ty);
                const i32 px0 = std::max(x0, ox);
                const i32 py0 = std::max(y0, oy);
                const i32 px1 = std::min(x1, ox + kTileSize);
                const i32 py1 = std::min(y1, oy + kTileSize);
                if (px1 <= px0 || py1 <= py0)
                    continue;

                const i32 w = px1 - px0;
                const i32 h = py1 - py0;
                f32 maxCov = 0.0f;
                for (i32 y = 0; y < h; ++y) {
                    const f32 dy = static_cast<f32>(py0 + y) + 0.5f - cy;
                    f32* row = cov_.data() + static_cast<usize>(y) * static_cast<usize>(w);
                    for (i32 x = 0; x < w; ++x) {
                        const f32 dx = static_cast<f32>(px0 + x) + 0.5f - cx;
                        const f32 lx = (dx * ct + dy * st) * invRx;
                        const f32 ly = (-dx * st + dy * ct) * invRy;
                        const f32 c = coverage(lx, ly, inner, feather);
                        row[x] = c;
                        maxCov = std::max(maxCov, c);
                    }
                }
                if (maxCov * alpha < kInkEpsilon)
                    continue; // 이 타일에는 아무것도 안 닿는다 — 만들지도 않는다

                auto wt = ctx_->target->writable(tc);
                if (!wt.ok())
                    continue; // 핫 패스에서는 던지지 않는다. 이 타일만 포기한다
                Tile* tile = wt.value().get();
                if (tile == nullptr)
                    continue;
                blendTile(*tile, px0 - ox, py0 - oy, w, h, alpha);
                dirty.push_back(tc); // 실제로 그린 타일만 덧붙인다
            }
        }
        updateLast(in);
    }

    void endStroke(DirtyTiles&) noexcept override {
        active_ = false;
        hasLast_ = false;
        ctx_.reset();
    }

    [[nodiscard]] f32 spacingPx(f32 pressure) const noexcept override {
        // 필압만 아는 상태에서의 대표 크기. 나머지 입력은 중립값으로 둔다.
        f32 inputs[kDynamicInputCount] = {};
        inputs[static_cast<usize>(DynamicInput::Pressure)] = clamp01(pressure);
        const f32 sizeMul = product(DynamicOutput::Size, inputs);
        const f32 d = std::clamp(preset_.tip.diameter * sizeMul, kMinDiameter, kMaxDiameter);
        return std::max(d * preset_.spacing, 0.05f);
    }

private:
    // ── 동적 반응 ────────────────────────────────────────────────────────
    [[nodiscard]] f32 product(DynamicOutput out, const f32 (&inputs)[kDynamicInputCount]) const
        noexcept {
        f32 m = 1.0f;
        for (const auto& d : links_[static_cast<usize>(out)]) {
            const f32 v = evalCurve(d.curve, inputs[static_cast<usize>(d.input)]);
            m *= 1.0f + d.amount * (v - 1.0f); // amount=0 이면 이 연결은 꺼진 것
        }
        return std::max(m, 0.0f);
    }

    [[nodiscard]] f32 sum(DynamicOutput out, const f32 (&inputs)[kDynamicInputCount]) const
        noexcept {
        f32 a = 0.0f;
        for (const auto& d : links_[static_cast<usize>(out)])
            a += d.amount * evalCurve(d.curve, inputs[static_cast<usize>(d.input)]);
        return a;
    }

    [[nodiscard]] Dynamics evalDynamics(const StampInput& in) noexcept {
        f32 inputs[kDynamicInputCount] = {};
        inputs[static_cast<usize>(DynamicInput::Pressure)] = clamp01(in.pressure);
        inputs[static_cast<usize>(DynamicInput::TiltX)] = std::clamp(in.tiltX, -1.0f, 1.0f);
        inputs[static_cast<usize>(DynamicInput::TiltY)] = std::clamp(in.tiltY, -1.0f, 1.0f);
        inputs[static_cast<usize>(DynamicInput::Azimuth)] = clamp01(in.azimuth / 360.0f);
        inputs[static_cast<usize>(DynamicInput::Velocity)] = clamp01(in.velocity);
        inputs[static_cast<usize>(DynamicInput::Random)] = rng_.unit();
        inputs[static_cast<usize>(DynamicInput::Fade)] = clamp01(in.fade);
        // 진행 방향은 엔진이 안다 — 직전 스탬프에서 여기까지의 각도다.
        f32 dir = lastDir_;
        if (hasLast_) {
            const f32 dx = in.pos.x - lastPos_.x;
            const f32 dy = in.pos.y - lastPos_.y;
            if (dx * dx + dy * dy > 1e-8f) {
                f32 deg = std::atan2(dy, dx) * 57.29578f;
                if (deg < 0.0f)
                    deg += 360.0f;
                dir = deg / 360.0f;
            }
        }
        lastDir_ = dir;
        inputs[static_cast<usize>(DynamicInput::Direction)] = dir;

        Dynamics d;
        d.size = product(DynamicOutput::Size, inputs);
        d.opacity = product(DynamicOutput::Opacity, inputs);
        d.flow = product(DynamicOutput::Flow, inputs);
        d.roundness = product(DynamicOutput::Roundness, inputs);
        d.rotationDeg = sum(DynamicOutput::Rotation, inputs);
        d.scatter = std::max(sum(DynamicOutput::Scatter, inputs), 0.0f);
        return d;
    }

    void updateLast(const StampInput& in) noexcept {
        lastPos_ = in.pos;
        hasLast_ = true;
    }

    // ── 팁 커버리지 ──────────────────────────────────────────────────────
    /// (lx, ly) 는 팁 로컬 좌표(경계가 1.0). 0..1 잉크량을 돌려준다.
    [[nodiscard]] f32 coverage(f32 lx, f32 ly, f32 inner, f32 feather) const noexcept {
        if (preset_.tip.kind == TipKind::Bitmap)
            return sampleBitmap(lx, ly);

        f32 r;
        switch (preset_.tip.shape) {
        case ProceduralShape::Square:
            r = std::max(std::fabs(lx), std::fabs(ly));
            break;
        case ProceduralShape::Diamond:
            r = std::fabs(lx) + std::fabs(ly);
            break;
        case ProceduralShape::Circle:
        default:
            r = std::sqrt(lx * lx + ly * ly);
            break;
        }
        if (r <= inner)
            return 1.0f;
        if (r >= 1.0f)
            return 0.0f;
        const f32 t = (r - inner) / feather;
        return 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep — 가장자리 AA
    }

    /// 비트맵 팁 쌍선형 샘플. 바깥은 0.
    [[nodiscard]] f32 sampleBitmap(f32 lx, f32 ly) const noexcept {
        const GrayImage& img = preset_.tip.bitmap;
        if (img.empty() || lx < -1.0f || lx > 1.0f || ly < -1.0f || ly > 1.0f)
            return 0.0f;
        const f32 fx = (lx * 0.5f + 0.5f) * static_cast<f32>(img.width) - 0.5f;
        const f32 fy = (ly * 0.5f + 0.5f) * static_cast<f32>(img.height) - 0.5f;
        const i32 ix = static_cast<i32>(std::floor(fx));
        const i32 iy = static_cast<i32>(std::floor(fy));
        const f32 tx = fx - static_cast<f32>(ix);
        const f32 tyf = fy - static_cast<f32>(iy);
        const auto at = [&](i32 x, i32 y) noexcept -> f32 {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height)
                return 0.0f;
            const usize idx = static_cast<usize>(y) * static_cast<usize>(img.width) +
                              static_cast<usize>(x);
            return static_cast<f32>(img.pixels[idx]) * (1.0f / 255.0f);
        };
        const f32 a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
        const f32 b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
        return a + (b - a) * tyf;
    }

    // ── 합성 ─────────────────────────────────────────────────────────────
    void blendTile(Tile& tile, i32 lx0, i32 ly0, i32 w, i32 h, f32 alpha) noexcept {
        u8* base = tile.mutablePixels();
        if (base == nullptr)
            return;
        const usize stride = tile.stride();
        const BlendMode mode = ctx_->eraser ? BlendMode::Erase : preset_.blendMode;
        const f32 sr = static_cast<f32>(ctx_->color.r) * (1.0f / 255.0f);
        const f32 sg = static_cast<f32>(ctx_->color.g) * (1.0f / 255.0f);
        const f32 sb = static_cast<f32>(ctx_->color.b) * (1.0f / 255.0f);
        const f32 srcA = static_cast<f32>(ctx_->color.a) * (1.0f / 255.0f);

        for (i32 y = 0; y < h; ++y) {
            const f32* row = cov_.data() + static_cast<usize>(y) * static_cast<usize>(w);
            u8* dst = base + static_cast<usize>(ly0 + y) * stride +
                      static_cast<usize>(lx0) * 4u;
            for (i32 x = 0; x < w; ++x, dst += 4) {
                const f32 sa = row[x] * alpha * srcA;
                if (sa < kInkEpsilon)
                    continue;

                const f32 da = static_cast<f32>(dst[3]) * (1.0f / 255.0f);
                if (mode == BlendMode::Erase) {
                    const f32 na = da * (1.0f - sa);
                    dst[3] = toByte(na);
                    continue;
                }
                if (ctx_->alphaLocked && da <= 0.0f)
                    continue; // 알파 잠금 — 투명한 곳은 건드리지 않는다

                const f32 dr = static_cast<f32>(dst[0]) * (1.0f / 255.0f);
                const f32 dg = static_cast<f32>(dst[1]) * (1.0f / 255.0f);
                const f32 db = static_cast<f32>(dst[2]) * (1.0f / 255.0f);

                f32 cr = sr, cg = sg, cb = sb;
                switch (mode) {
                case BlendMode::Multiply:
                    cr = sr * dr;
                    cg = sg * dg;
                    cb = sb * db;
                    break;
                case BlendMode::Screen:
                    cr = sr + dr - sr * dr;
                    cg = sg + dg - sg * dg;
                    cb = sb + db - sb * db;
                    break;
                case BlendMode::Add:
                    cr = std::min(1.0f, sr + dr);
                    cg = std::min(1.0f, sg + dg);
                    cb = std::min(1.0f, sb + db);
                    break;
                default:
                    break; // Normal
                }

                if (ctx_->alphaLocked) {
                    // 알파는 그대로, 색만 섞는다.
                    dst[0] = toByte(dr + (cr - dr) * sa);
                    dst[1] = toByte(dg + (cg - dg) * sa);
                    dst[2] = toByte(db + (cb - db) * sa);
                    continue;
                }

                const f32 na = sa + da * (1.0f - sa);
                if (na <= 0.0f)
                    continue;
                const f32 wd = da * (1.0f - sa);
                dst[0] = toByte((cr * sa + dr * wd) / na);
                dst[1] = toByte((cg * sa + dg * wd) / na);
                dst[2] = toByte((cb * sa + db * wd) / na);
                dst[3] = toByte(na);
            }
        }
    }

    [[nodiscard]] static u8 toByte(f32 v) noexcept {
        return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }

    MariBrushPreset preset_{};
    std::vector<DynamicLink> links_[kDynamicOutputCount];
    /// 🔴 optional 인 이유: StrokeContext 는 출처(StrokeSource)가 없으면 만들 수 없다.
    ///    "일단 사람 획으로 만들어 두고 나중에 덮어쓴다"는 자리표시자를 두지 않으려는
    ///    것이다 — 그 자리표시자가 새면 AI 획이 사람 획이 된다(docs/05 3.1).
    std::optional<StrokeContext> ctx_;
    Rng rng_{};
    std::vector<f32> cov_;
    PointF lastPos_{};
    f32 lastDir_ = 0.0f;
    bool hasLast_ = false;
    bool hasPreset_ = false;
    bool active_ = false;
};

} // namespace

Result<BrushEnginePtr> makeNativeEngine() {
    return Ok(BrushEnginePtr(new NativeEngine()));
}

} // namespace mari::stroke
