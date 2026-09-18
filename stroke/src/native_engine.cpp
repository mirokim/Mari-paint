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

        if (preset_.texture.has_value()) {
            BrushTexture& tex = *preset_.texture;
            if (tex.image.empty()) {
                preset_.texture.reset();
                if (report)
                    report->add(ImportSeverity::Degraded, "texture", "텍스처 이미지가 비어 있어 텍스처를 껐습니다");
            } else {
                tex.scale = std::clamp(tex.scale, 0.05f, 64.0f);
                tex.depth = clamp01(tex.depth);
                switch (tex.blendMode) {
                case BlendMode::Multiply:
                case BlendMode::Subtract:
                case BlendMode::Darken:
                case BlendMode::Screen:
                case BlendMode::Overlay:
                case BlendMode::HardLight:
                    break;
                default:
                    if (report)
                        report->add(ImportSeverity::Degraded, "texture/blendMode",
                                    std::string("텍스처 합성 '") + blendModeName(tex.blendMode) +
                                        "' 은 곱하기로 근사합니다");
                    tex.blendMode = BlendMode::Multiply;
                    break;
                }
            }
        }
        if (preset_.dual.has_value()) {
            DualBrush& d = *preset_.dual;
            if (d.tip.kind == TipKind::Bitmap && d.tip.bitmap.empty()) {
                d.tip.kind = TipKind::Procedural;
                if (report)
                    report->add(ImportSeverity::Degraded, "dual/tip", "듀얼 브러시 팁이 비어 있어 원형으로 대체했습니다");
            }
            d.tip.diameter = std::clamp(d.tip.diameter > 0.0f ? d.tip.diameter : preset_.tip.diameter,
                                        kMinDiameter, kMaxDiameter);
            d.tip.aspectRatio = std::clamp(d.tip.aspectRatio, 0.01f, 1.0f);
            d.tip.hardness = clamp01(d.tip.hardness);
            d.count = std::clamp(d.count, 1, 16);
            switch (d.blendMode) {
            case BlendMode::Multiply:
            case BlendMode::Darken:
            case BlendMode::Screen:
            case BlendMode::Add:
            case BlendMode::Overlay:
                break;
            default:
                if (report)
                    report->add(ImportSeverity::Degraded, "dual/blendMode",
                                std::string("듀얼 브러시 합성 '") + blendModeName(d.blendMode) +
                                    "' 은 곱하기로 근사합니다");
                d.blendMode = BlendMode::Multiply;
                break;
            }
        }
        preset_.scatterCount = std::clamp(preset_.scatterCount, 1, 16);
        preset_.noise = clamp01(preset_.noise);
        if (preset_.airbrush && report)
            report->add(ImportSeverity::Degraded, "airbrush",
                        "에어브러시(멈춰 있어도 쌓임)는 native 엔진이 시간 반복을 하지 않아 보통 붓처럼 찍습니다");

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
        // 🔴 핫 패스 비용 0 규약: 선택을 **여기서 한 번만** 본다.
        //    "제한 없음"(없거나 전체 선택)이면 포인터를 꺼서 stamp() 가 아예 묻지 않게 한다.
        sel_ = (ctx.selection != nullptr && !ctx.selection->isAll()) ? ctx.selection : nullptr;
        rng_.seed(ctx.seed);
        hasLast_ = false;
        active_ = true;
        // 색 변화: 획마다 한 번이면 여기서 뽑고, 스탬프마다면 stamp() 가 뽑는다.
        strokeColor_ = ctx.color;
        if (preset_.colorDynamics.active() && !preset_.colorDynamics.perTip)
            strokeColor_ = jitterColor();
        // 핫 패스 버퍼는 여기서 잡는다. 타일 하나분(64×64)이면 충분하다.
        cov_.assign(static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize), 0.0f);
        return Ok();
    }

    void stamp(const StampInput& in, DirtyTiles& dirty) noexcept override {
        if (!active_)
            return;

        const Dynamics dyn = evalDynamics(in);

        const f32 diameter = std::clamp(preset_.tip.diameter * dyn.size, 0.0f, kMaxDiameter);
        const f32 alpha = clamp01(preset_.opacity * dyn.opacity) * clamp01(preset_.flow * dyn.flow);
        if (diameter < kMinDiameter || alpha < kInkEpsilon) {
            updateLast(in);
            return; // 잉크가 없으면 타일도 더티도 없다
        }

        // 포토샵 Count: 한 위치에 여러 번. 흩뿌림이 없으면 같은 자리에 겹쳐 찍힌다(포토샵도 그렇다).
        const int count = preset_.scatterCount;
        for (int i = 0; i < count; ++i) {
            f32 cx = in.pos.x;
            f32 cy = in.pos.y;
            if (dyn.scatter > 0.0f) {
                const f32 ang = rng_.unit() * 6.2831853f;
                const f32 rad = std::sqrt(rng_.unit()) * dyn.scatter * diameter;
                cx += std::cos(ang) * rad;
                cy += std::sin(ang) * rad;
            }
            const Color color = (preset_.colorDynamics.active() && preset_.colorDynamics.perTip) ? jitterColor()
                                                                                                : strokeColor_;
            stampOnce(cx, cy, diameter, alpha, dyn, color, dirty);
        }
        updateLast(in);
    }

    void stampOnce(f32 cx, f32 cy, f32 diameter, f32 alpha, const Dynamics& dyn, const Color& color,
                   DirtyTiles& dirty) noexcept {
        const f32 rx = diameter * 0.5f;
        const f32 ry = std::max(rx * std::clamp(preset_.tip.aspectRatio * dyn.roundness, 0.01f,
                                                1.0f),
                                kMinDiameter * 0.5f);
        const f32 theta = (preset_.tip.angle + dyn.rotationDeg) * 0.017453292f;
        const f32 ct = std::cos(theta);
        const f32 st = std::sin(theta);

        // 듀얼 브러시 — 크기는 첫 팁과 같은 배율로 따라간다. 흩뿌림은 자기 것.
        const DualBrush* dual = preset_.dual.has_value() ? &*preset_.dual : nullptr;
        f32 dcx = cx, dcy = cy, dInvRx = 0.0f, dInvRy = 0.0f, dct = 1.0f, dst = 0.0f, dInner = 1.0f,
            dFeather = 0.01f;
        if (dual != nullptr) {
            const f32 drx = std::max(dual->tip.diameter * dyn.size * 0.5f, kMinDiameter * 0.5f);
            const f32 dry = std::max(drx * dual->tip.aspectRatio, kMinDiameter * 0.5f);
            if (dual->scatter > 0.0f) {
                const f32 ang = rng_.unit() * 6.2831853f;
                const f32 rad = std::sqrt(rng_.unit()) * dual->scatter * drx * 2.0f;
                dcx += std::cos(ang) * rad;
                dcy += std::sin(ang) * rad;
            }
            const f32 dth = (dual->tip.angle + dyn.rotationDeg) * 0.017453292f;
            dct = std::cos(dth);
            dst = std::sin(dth);
            dInvRx = 1.0f / drx;
            dInvRy = 1.0f / dry;
            const f32 daa = std::clamp(1.0f / std::max(drx, dry), 0.01f, 1.0f);
            dFeather = std::clamp(std::max(1.0f - dual->tip.hardness, daa), 0.01f, 1.0f);
            dInner = 1.0f - dFeather;
        }

        // 경계 상자 — 회전한 타원/사각형을 전부 덮는 보수적 반경 + AA 여백 1px.
        const f32 half = std::sqrt(rx * rx + ry * ry) + 1.0f;
        const i32 x0 = static_cast<i32>(std::floor(cx - half));
        const i32 y0 = static_cast<i32>(std::floor(cy - half));
        const i32 x1 = static_cast<i32>(std::ceil(cx + half)); // 배타적
        const i32 y1 = static_cast<i32>(std::ceil(cy + half));
        if (x1 <= x0 || y1 <= y0)
            return;

        const f32 invRx = 1.0f / rx;
        const f32 invRy = 1.0f / ry;
        // 안티에일리어싱: 하드니스가 1이어도 최소 한 픽셀만큼은 부드럽게 끝낸다.
        const f32 aa = std::clamp(1.0f / std::max(rx, ry), 0.01f, 1.0f);
        const f32 feather = std::clamp(std::max(1.0f - preset_.tip.hardness, aa), 0.01f, 1.0f);
        const f32 inner = 1.0f - feather;

        const BrushTexture* tex = preset_.texture.has_value() ? &*preset_.texture : nullptr;
        const bool wet = preset_.wetEdges;
        const f32 noise = preset_.noise;

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
                    const f32 pyc = static_cast<f32>(py0 + y) + 0.5f;
                    const f32 dy = pyc - cy;
                    f32* row = cov_.data() + static_cast<usize>(y) * static_cast<usize>(w);
                    for (i32 x = 0; x < w; ++x) {
                        const f32 pxc = static_cast<f32>(px0 + x) + 0.5f;
                        const f32 dx = pxc - cx;
                        const f32 lx = (dx * ct + dy * st) * invRx;
                        const f32 ly = (-dx * st + dy * ct) * invRy;
                        f32 c = coverage(preset_.tip, lx, ly, inner, feather);
                        if (c > 0.0f) {
                            if (dual != nullptr) {
                                const f32 ddx = pxc - dcx, ddy = pyc - dcy;
                                const f32 dlx = (ddx * dct + ddy * dst) * dInvRx;
                                const f32 dly = (-ddx * dst + ddy * dct) * dInvRy;
                                c = combineDual(c, coverage(dual->tip, dlx, dly, dInner, dFeather), dual->blendMode);
                            }
                            if (tex != nullptr)
                                c = applyTexture(c, *tex, tex->anchoredToCanvas ? pxc : dx, tex->anchoredToCanvas ? pyc : dy);
                            if (wet)
                                c = c < 0.5f ? c * 1.4f : 0.7f - (c - 0.5f) * 0.6f;
                            if (noise > 0.0f)
                                c *= 1.0f - noise * (1.0f - c) * hash01(px0 + x, py0 + y);
                        }
                        row[x] = c;
                        maxCov = std::max(maxCov, c);
                    }
                }
                // 선택 마스크를 커버리지에 곱한다. sel_ 이 null 이면 이 블록은 통째로 없다.
                if (sel_ != nullptr && !applySelection(tc, px0, py0, w, h, maxCov))
                    continue; // 선택 밖이라 잉크가 하나도 안 남았다
                if (maxCov * alpha < kInkEpsilon)
                    continue; // 이 타일에는 아무것도 안 닿는다 — 만들지도 않는다

                auto wt = ctx_->target->writable(tc);
                if (!wt.ok())
                    continue; // 핫 패스에서는 던지지 않는다. 이 타일만 포기한다
                Tile* tile = wt.value().get();
                if (tile == nullptr)
                    continue;
                blendTile(*tile, px0 - ox, py0 - oy, w, h, alpha, color);
                dirty.push_back(tc); // 실제로 그린 타일만 덧붙인다
            }
        }
    }

    void endStroke(DirtyTiles&) noexcept override {
        active_ = false;
        hasLast_ = false;
        sel_ = nullptr;
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
    [[nodiscard]] static f32 coverage(const BrushTip& tip, f32 lx, f32 ly, f32 inner, f32 feather) noexcept {
        if (tip.kind == TipKind::Bitmap)
            return sampleBitmap(tip.bitmap, lx, ly);

        f32 r;
        switch (tip.shape) {
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
    [[nodiscard]] static f32 sampleBitmap(const GrayImage& img, f32 lx, f32 ly) noexcept {
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

    // ── 듀얼 · 텍스처 · 노이즈 · 색 변화 ──────────────────────────────────
    [[nodiscard]] static f32 combineDual(f32 c, f32 d, BlendMode mode) noexcept {
        switch (mode) {
        case BlendMode::Darken:  return std::min(c, d);
        case BlendMode::Screen:  return c + d - c * d;
        case BlendMode::Add:     return std::min(1.0f, c + d);
        case BlendMode::Overlay: return c < 0.5f ? 2.0f * c * d : 1.0f - 2.0f * (1.0f - c) * (1.0f - d);
        default:                 return c * d; // Multiply
        }
    }

    /// 텍스처 값을 커버리지에 얹는다. 텍스처는 "밝을수록 잉크가 잘 묻는" 종이 결이다(0..1).
    [[nodiscard]] static f32 applyTexture(f32 c, const BrushTexture& tex, f32 x, f32 y) noexcept {
        const GrayImage& img = tex.image;
        // 캔버스 좌표를 배율로 나눠 타일링(음수도 감싼다).
        const f32 fx = x / tex.scale;
        const f32 fy = y / tex.scale;
        i32 ix = static_cast<i32>(std::floor(fx)) % img.width;
        i32 iy = static_cast<i32>(std::floor(fy)) % img.height;
        if (ix < 0) ix += img.width;
        if (iy < 0) iy += img.height;
        const f32 t0 = static_cast<f32>(img.pixels[static_cast<usize>(iy) * static_cast<usize>(img.width) +
                                                   static_cast<usize>(ix)]) * (1.0f / 255.0f);
        // depth 0 = 텍스처 없음(1.0), depth 1 = 텍스처 그대로.
        const f32 t = 1.0f - tex.depth * (1.0f - t0);
        switch (tex.blendMode) {
        case BlendMode::Subtract:  return std::max(0.0f, c - tex.depth * (1.0f - t0));
        case BlendMode::Darken:    return std::min(c, t);
        case BlendMode::Screen:    return c * (t + (1.0f - t) * c); // 밝은 결이 중심을 남기고 가장자리를 깎는다
        case BlendMode::Overlay:
        case BlendMode::HardLight: return c < 0.5f ? 2.0f * c * t : 1.0f - 2.0f * (1.0f - c) * (1.0f - t);
        default:                   return c * t; // Multiply
        }
    }

    /// 픽셀 좌표 해시 → 0..1. 스탬프 위치와 무관하게 캔버스에 고정된 노이즈라 겹쳐 찍어도 결이 유지된다.
    [[nodiscard]] static f32 hash01(i32 x, i32 y) noexcept {
        u32 h = static_cast<u32>(x) * 0x8da6b343u ^ static_cast<u32>(y) * 0xd8163841u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<f32>(h & 0xffffffu) * (1.0f / 16777216.0f);
    }

    /// 색 변화 — 전경↔배경 · 색조 · 채도 · 명도 지터. 난수는 rng_ 에서 뽑아 재현된다.
    [[nodiscard]] Color jitterColor() noexcept {
        const ColorDynamics& cd = preset_.colorDynamics;
        const Color& fg = ctx_->color;
        const Color& bg = ctx_->background;
        f32 r = static_cast<f32>(fg.r), g = static_cast<f32>(fg.g), b = static_cast<f32>(fg.b);
        if (cd.fgBgJitter > 0.0f) {
            const f32 t = rng_.unit() * cd.fgBgJitter;
            r += (static_cast<f32>(bg.r) - r) * t;
            g += (static_cast<f32>(bg.g) - g) * t;
            b += (static_cast<f32>(bg.b) - b) * t;
        }
        r *= 1.0f / 255.0f; g *= 1.0f / 255.0f; b *= 1.0f / 255.0f;
        // RGB → HSV
        const f32 mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        const f32 d = mx - mn;
        f32 h = 0.0f;
        if (d > 1e-6f) {
            if (mx == r)      h = std::fmod((g - b) / d, 6.0f);
            else if (mx == g) h = (b - r) / d + 2.0f;
            else              h = (r - g) / d + 4.0f;
            h *= 60.0f;
            if (h < 0.0f) h += 360.0f;
        }
        f32 sat = mx > 1e-6f ? d / mx : 0.0f;
        f32 v = mx;
        const auto pm = [this]() noexcept { return rng_.unit() * 2.0f - 1.0f; };
        if (cd.hueJitter > 0.0f)        h = std::fmod(h + pm() * cd.hueJitter * 180.0f + 360.0f, 360.0f);
        if (cd.saturationJitter > 0.0f) sat = clamp01(sat + pm() * cd.saturationJitter);
        if (cd.brightnessJitter > 0.0f) v = clamp01(v + pm() * cd.brightnessJitter);
        if (cd.purity > 0.0f)           sat = clamp01(sat + (1.0f - sat) * cd.purity);
        else if (cd.purity < 0.0f)      sat = clamp01(sat * (1.0f + cd.purity));
        // HSV → RGB
        const f32 c = v * sat;
        const f32 hh = h / 60.0f;
        const f32 xx = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
        f32 rr = 0, gg = 0, bb = 0;
        if (hh < 1)      { rr = c; gg = xx; }
        else if (hh < 2) { rr = xx; gg = c; }
        else if (hh < 3) { gg = c; bb = xx; }
        else if (hh < 4) { gg = xx; bb = c; }
        else if (hh < 5) { rr = xx; bb = c; }
        else             { rr = c; bb = xx; }
        const f32 m = v - c;
        return Color{toByte(rr + m), toByte(gg + m), toByte(bb + m), fg.a};
    }

    // ── 선택 마스크 ──────────────────────────────────────────────────────
    /// 커버리지 버퍼에 선택 값을 곱한다. 남은 잉크가 있으면 true.
    /// `maxCov` 를 곱한 뒤의 값으로 다시 채운다 — 안 그러면 "선택 밖인데 칠했다"가 된다.
    [[nodiscard]] bool applySelection(TileCoord tc, i32 px0, i32 py0, i32 w, i32 h,
                                      f32& maxCov) noexcept {
        const u8* mask = sel_->tilePixels(tc);
        const Size canvas = sel_->canvasSize();
        if (mask == nullptr && sel_->outsideValue() == 255) {
            // 이 타일은 통째로 선택 안쪽이다. 캔버스 안이기만 하면 손댈 게 없다.
            if (px0 >= 0 && py0 >= 0 && px0 + w <= canvas.width && py0 + h <= canvas.height)
                return true;
        }
        const i32 ox = tileOrigin(tc.tx);
        const i32 oy = tileOrigin(tc.ty);
        f32 newMax = 0.0f;
        for (i32 y = 0; y < h; ++y) {
            f32* row = cov_.data() + static_cast<usize>(y) * static_cast<usize>(w);
            const i32 cy = py0 + y;
            const u8* mrow =
                mask != nullptr
                    ? mask + static_cast<usize>(cy - oy) * static_cast<usize>(kTileSize)
                    : nullptr;
            const bool rowInCanvas = cy >= 0 && cy < canvas.height;
            for (i32 x = 0; x < w; ++x) {
                const i32 cx = px0 + x;
                // 🔴 캔버스 밖은 선택되지 않는다 — 마스크가 있든 없든 같다.
                u8 m = 0;
                if (rowInCanvas && cx >= 0 && cx < canvas.width)
                    m = mrow != nullptr ? mrow[static_cast<usize>(cx - ox)] : sel_->outsideValue();
                const f32 v = row[x] * (static_cast<f32>(m) * (1.0f / 255.0f));
                row[x] = v;
                newMax = std::max(newMax, v);
            }
        }
        maxCov = newMax;
        return newMax > 0.0f;
    }

    // ── 합성 ─────────────────────────────────────────────────────────────
    void blendTile(Tile& tile, i32 lx0, i32 ly0, i32 w, i32 h, f32 alpha, const Color& color) noexcept {
        u8* base = tile.mutablePixels();
        if (base == nullptr)
            return;
        const usize stride = tile.stride();
        const BlendMode mode = ctx_->eraser ? BlendMode::Erase : preset_.blendMode;
        const f32 sr = static_cast<f32>(color.r) * (1.0f / 255.0f);
        const f32 sg = static_cast<f32>(color.g) * (1.0f / 255.0f);
        const f32 sb = static_cast<f32>(color.b) * (1.0f / 255.0f);
        const f32 srcA = static_cast<f32>(color.a) * (1.0f / 255.0f);

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
    /// 🔴 "제한 없음"이면 nullptr 이다. 핫 패스는 이 포인터 하나만 본다.
    const SelectionMask* sel_ = nullptr;
    Rng rng_{};
    Color strokeColor_{};
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
