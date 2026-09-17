// Mari Paint — 블렌드 모드 구현. 선언은 include/mari/core/blend.hpp.
#include <mari/core/blend.hpp>

#include <algorithm>
#include <cmath>

namespace mari {
namespace {

constexpr f32 kInv255 = 1.0f / 255.0f;

[[nodiscard]] inline f32 clamp01(f32 v) noexcept {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

[[nodiscard]] inline u8 toU8(f32 v) noexcept {
    return static_cast<u8>(clamp01(v) * 255.0f + 0.5f);
}

[[nodiscard]] inline f32 hardLight(f32 cb, f32 cs) noexcept {
    return cs <= 0.5f ? cb * (2.0f * cs) : (cb + (2.0f * cs - 1.0f) - cb * (2.0f * cs - 1.0f));
}

[[nodiscard]] inline f32 softLight(f32 cb, f32 cs) noexcept {
    // W3C 정의. cb 가 0.25 이하일 때의 다항식 분기가 핵심이다.
    if (cs <= 0.5f)
        return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
    const f32 d = (cb <= 0.25f) ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
    return cb + (2.0f * cs - 1.0f) * (d - cb);
}

// ── 분리 불가 모드용 헬퍼 (W3C non-separable blend functions) ─────────────
struct Rgb {
    f32 r = 0.0f, g = 0.0f, b = 0.0f;
};

[[nodiscard]] inline f32 lum(const Rgb& c) noexcept {
    return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
}

[[nodiscard]] inline Rgb clipColor(Rgb c) noexcept {
    const f32 l = lum(c);
    const f32 n = std::min({c.r, c.g, c.b});
    const f32 x = std::max({c.r, c.g, c.b});
    if (n < 0.0f && l - n > 1e-6f) {
        const f32 k = l / (l - n);
        c = Rgb{l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k};
    }
    if (x > 1.0f && x - l > 1e-6f) {
        const f32 k = (1.0f - l) / (x - l);
        c = Rgb{l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k};
    }
    return c;
}

[[nodiscard]] inline Rgb setLum(const Rgb& c, f32 l) noexcept {
    const f32 d = l - lum(c);
    return clipColor(Rgb{c.r + d, c.g + d, c.b + d});
}

[[nodiscard]] inline f32 sat(const Rgb& c) noexcept {
    return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
}

[[nodiscard]] inline Rgb setSat(Rgb c, f32 s) noexcept {
    // 최소/중간/최대 채널을 가려낸 뒤 중간값을 비율로 다시 놓는다.
    f32* ch[3] = {&c.r, &c.g, &c.b};
    if (*ch[0] > *ch[1])
        std::swap(ch[0], ch[1]);
    if (*ch[1] > *ch[2])
        std::swap(ch[1], ch[2]);
    if (*ch[0] > *ch[1])
        std::swap(ch[0], ch[1]);
    if (*ch[2] > *ch[0]) {
        *ch[1] = (*ch[1] - *ch[0]) * s / (*ch[2] - *ch[0]);
        *ch[2] = s;
    } else {
        *ch[1] = 0.0f;
        *ch[2] = 0.0f;
    }
    *ch[0] = 0.0f;
    return c;
}

[[nodiscard]] inline Rgb nonSeparable(BlendMode m, const Rgb& cb, const Rgb& cs) noexcept {
    switch (m) {
    case BlendMode::Hue:
        return setLum(setSat(cs, sat(cb)), lum(cb));
    case BlendMode::Saturation:
        return setLum(setSat(cb, sat(cs)), lum(cb));
    case BlendMode::Color:
        return setLum(cs, lum(cb));
    case BlendMode::Luminosity:
        return setLum(cb, lum(cs));
    default:
        return cs;
    }
}

} // namespace

bool isSeparableBlend(BlendMode m) noexcept {
    switch (m) {
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
    case BlendMode::Erase:
        return false;
    default:
        return true;
    }
}

f32 blendChannel(BlendMode m, f32 cb, f32 cs) noexcept {
    switch (m) {
    case BlendMode::Normal:
        return cs;
    case BlendMode::Multiply:
        return cb * cs;
    case BlendMode::Screen:
        return cb + cs - cb * cs;
    case BlendMode::Overlay:
        return hardLight(cs, cb); // HardLight 의 인자를 뒤집은 것이다
    case BlendMode::Darken:
        return std::min(cb, cs);
    case BlendMode::Lighten:
        return std::max(cb, cs);
    case BlendMode::ColorDodge:
        if (cb <= 0.0f)
            return 0.0f;
        if (cs >= 1.0f)
            return 1.0f;
        return std::min(1.0f, cb / (1.0f - cs));
    case BlendMode::ColorBurn:
        if (cb >= 1.0f)
            return 1.0f;
        if (cs <= 0.0f)
            return 0.0f;
        return 1.0f - std::min(1.0f, (1.0f - cb) / cs);
    case BlendMode::HardLight:
        return hardLight(cb, cs);
    case BlendMode::SoftLight:
        return softLight(cb, cs);
    case BlendMode::Difference:
        return std::fabs(cb - cs);
    case BlendMode::Exclusion:
        return cb + cs - 2.0f * cb * cs;
    case BlendMode::Add:
        return std::min(1.0f, cb + cs);
    case BlendMode::Subtract:
        return std::max(0.0f, cb - cs);
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
    case BlendMode::Erase:
        break;
    }
    return cs;
}

ColorF blendPixel(BlendMode m, const ColorF& dst, const ColorF& src, f32 extraAlpha) noexcept {
    const f32 as = clamp01(src.a) * clamp01(extraAlpha);
    const f32 ab = clamp01(dst.a);

    if (m == BlendMode::Erase) {
        // 색은 그대로 두고 알파만 깎는다.
        const f32 ao = ab * (1.0f - as);
        return ColorF{dst.r, dst.g, dst.b, ao};
    }
    if (as <= 0.0f)
        return ColorF{dst.r, dst.g, dst.b, ab};

    Rgb b{};
    if (isSeparableBlend(m)) {
        b = Rgb{blendChannel(m, dst.r, src.r), blendChannel(m, dst.g, src.g),
                blendChannel(m, dst.b, src.b)};
    } else {
        b = nonSeparable(m, Rgb{dst.r, dst.g, dst.b}, Rgb{src.r, src.g, src.b});
    }

    const f32 ao = as + ab * (1.0f - as);
    if (ao <= 0.0f)
        return ColorF{0.0f, 0.0f, 0.0f, 0.0f};

    // Co*ao = (1-ab)*as*Cs + ab*as*B + (1-as)*ab*Cb
    const f32 w0 = (1.0f - ab) * as;
    const f32 w1 = ab * as;
    const f32 w2 = (1.0f - as) * ab;
    return ColorF{clamp01((w0 * src.r + w1 * b.r + w2 * dst.r) / ao),
                  clamp01((w0 * src.g + w1 * b.g + w2 * dst.g) / ao),
                  clamp01((w0 * src.b + w1 * b.b + w2 * dst.b) / ao), clamp01(ao)};
}

Color8 blendPixel8(BlendMode m, Color8 dst, Color8 src, f32 extraAlpha) noexcept {
    const ColorF d{static_cast<f32>(dst.r) * kInv255, static_cast<f32>(dst.g) * kInv255,
                   static_cast<f32>(dst.b) * kInv255, static_cast<f32>(dst.a) * kInv255};
    const ColorF s{static_cast<f32>(src.r) * kInv255, static_cast<f32>(src.g) * kInv255,
                   static_cast<f32>(src.b) * kInv255, static_cast<f32>(src.a) * kInv255};
    const ColorF o = blendPixel(m, d, s, extraAlpha);
    return Color8{toU8(o.r), toU8(o.g), toU8(o.b), toU8(o.a)};
}

void blendRowRgba8(BlendMode m, u8* dst, const u8* src, i32 count, f32 extraAlpha,
                   const u8* mask) noexcept {
    if (dst == nullptr || src == nullptr || count <= 0)
        return;
    const f32 base = clamp01(extraAlpha);
    if (base <= 0.0f)
        return;
    for (i32 i = 0; i < count; ++i) {
        f32 a = base;
        if (mask != nullptr)
            a *= static_cast<f32>(mask[i]) * kInv255;
        if (a <= 0.0f)
            continue;
        const usize o = static_cast<usize>(i) * 4u;
        const Color8 d{dst[o + 0], dst[o + 1], dst[o + 2], dst[o + 3]};
        const Color8 s{src[o + 0], src[o + 1], src[o + 2], src[o + 3]};
        if (s.a == 0 && m != BlendMode::Erase)
            continue;
        const Color8 r = blendPixel8(m, d, s, a);
        dst[o + 0] = r.r;
        dst[o + 1] = r.g;
        dst[o + 2] = r.b;
        dst[o + 3] = r.a;
    }
}

} // namespace mari
