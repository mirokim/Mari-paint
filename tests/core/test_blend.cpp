// 블렌드 모드 — 알려진 입출력 값 검증.
// 불투명 위에 불투명을 올리면 합성식이 B(Cb, Cs) 로 줄어든다. 그 성질을 이용한다.
#include <mari/core/blend.hpp>
#include <mari/test/harness.hpp>

using namespace mari;

namespace {

/// 회색 cb 위에 회색 cs 를 불투명하게 올린 결과의 R 채널.
u8 mix(BlendMode m, u8 cb, u8 cs) {
    return blendPixel8(m, Color8{cb, cb, cb, 255}, Color8{cs, cs, cs, 255}).r;
}

f32 lum(Color8 c) {
    return (0.3f * static_cast<f32>(c.r) + 0.59f * static_cast<f32>(c.g) +
            0.11f * static_cast<f32>(c.b)) /
           255.0f;
}

} // namespace

MARI_TEST(separable_modes_have_known_values) {
    // 0.5 × 0.5 = 0.25
    CHECK_EQ(static_cast<int>(mix(BlendMode::Multiply, 128, 128)), 64);
    // 스크린은 곱하기의 여집합
    CHECK_EQ(static_cast<int>(mix(BlendMode::Screen, 128, 128)), 192);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Darken, 64, 192)), 64);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Lighten, 64, 192)), 192);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Difference, 200, 50)), 150);
    CHECK_NEAR(mix(BlendMode::Exclusion, 128, 128), 128, 1.0); // 0.5+0.5-2(0.25) = 0.5
    CHECK_EQ(static_cast<int>(mix(BlendMode::Subtract, 200, 50)), 150);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Add, 200, 100)), 255); // 포화
    CHECK_EQ(static_cast<int>(mix(BlendMode::Normal, 17, 200)), 200);

    // 항등원들
    CHECK_EQ(static_cast<int>(mix(BlendMode::Multiply, 200, 255)), 200);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Screen, 200, 0)), 200);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Add, 200, 0)), 200);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Subtract, 200, 0)), 200);
}

MARI_TEST(contrast_modes_have_known_values) {
    // Overlay 는 HardLight 의 인자를 뒤집은 것이다: Overlay(cb,cs) == HardLight(cs,cb)
    CHECK_EQ(static_cast<int>(mix(BlendMode::Overlay, 64, 255)), 128);
    CHECK_EQ(static_cast<int>(mix(BlendMode::HardLight, 255, 64)), 128);
    CHECK_EQ(static_cast<int>(mix(BlendMode::HardLight, 64, 255)), 255);
    CHECK_EQ(static_cast<int>(mix(BlendMode::Overlay, 255, 64)), 255);
    for (u8 v : {u8{0}, u8{64}, u8{128}, u8{255}})
        CHECK_EQ(static_cast<int>(mix(BlendMode::Overlay, 0, v)), 0); // 검정은 검정으로

    // 닷지/번의 경계값
    CHECK_EQ(static_cast<int>(mix(BlendMode::ColorDodge, 128, 0)), 128);
    CHECK_EQ(static_cast<int>(mix(BlendMode::ColorDodge, 128, 255)), 255);
    CHECK_EQ(static_cast<int>(mix(BlendMode::ColorDodge, 0, 200)), 0);
    CHECK_EQ(static_cast<int>(mix(BlendMode::ColorBurn, 128, 255)), 128);
    CHECK_EQ(static_cast<int>(mix(BlendMode::ColorBurn, 128, 0)), 0);
    CHECK_EQ(static_cast<int>(mix(BlendMode::ColorBurn, 255, 0)), 255);

    // SoftLight 는 cs=128 근처에서 거의 항등이다.
    CHECK_NEAR(mix(BlendMode::SoftLight, 100, 128), 100, 1.0);
    // cs=255 면 밝아지고 cs=0 이면 어두워진다.
    CHECK(mix(BlendMode::SoftLight, 100, 255) > 100);
    CHECK(mix(BlendMode::SoftLight, 100, 0) < 100);
}

MARI_TEST(alpha_compositing_is_correct) {
    // 검정(불투명) 위에 흰색 50% → 중간 회색
    const Color8 r =
        blendPixel8(BlendMode::Normal, Color8{0, 0, 0, 255}, Color8{255, 255, 255, 128});
    CHECK_NEAR(r.r, 128, 1.0);
    CHECK_EQ(static_cast<int>(r.a), 255);

    // 완전 투명 배경 위에 불투명 빨강 → 빨강 그대로 (색이 번지지 않는다)
    const Color8 s = blendPixel8(BlendMode::Multiply, Color8{0, 0, 0, 0}, Color8{255, 0, 0, 255});
    CHECK(s == (Color8{255, 0, 0, 255}));

    // extraAlpha(레이어 불투명도)도 같은 효과를 낸다
    const Color8 t =
        blendPixel8(BlendMode::Normal, Color8{0, 0, 0, 255}, Color8{255, 255, 255, 255}, 0.5f);
    CHECK_NEAR(t.r, 128, 1.0);

    // src 가 완전 투명이면 dst 그대로
    const Color8 u = blendPixel8(BlendMode::Normal, Color8{10, 20, 30, 200}, Color8{9, 9, 9, 0});
    CHECK(u == (Color8{10, 20, 30, 200}));
}

MARI_TEST(erase_cuts_destination_alpha) {
    CHECK(!isSeparableBlend(BlendMode::Erase));
    const Color8 half =
        blendPixel8(BlendMode::Erase, Color8{10, 20, 30, 255}, Color8{0, 0, 0, 128});
    CHECK_EQ(static_cast<int>(half.r), 10); // 색은 그대로
    CHECK_NEAR(half.a, 127, 1.0);           // 알파만 깎인다

    const Color8 all = blendPixel8(BlendMode::Erase, Color8{10, 20, 30, 255}, Color8{0, 0, 0, 255});
    CHECK_EQ(static_cast<int>(all.a), 0);
}

MARI_TEST(non_separable_modes_keep_their_invariants) {
    CHECK(!isSeparableBlend(BlendMode::Hue));
    CHECK(!isSeparableBlend(BlendMode::Saturation));
    CHECK(!isSeparableBlend(BlendMode::Color));
    CHECK(!isSeparableBlend(BlendMode::Luminosity));
    CHECK(isSeparableBlend(BlendMode::Multiply));

    const Color8 red{255, 0, 0, 255};
    const Color8 gray{128, 128, 128, 255};

    // Luminosity: 배경의 색상·채도 + 전경의 휘도
    const Color8 l = blendPixel8(BlendMode::Luminosity, red, gray);
    CHECK_NEAR(lum(l), lum(gray), 0.01);

    // Color: 전경의 색상·채도 + 배경의 휘도
    const Color8 c = blendPixel8(BlendMode::Color, red, gray);
    CHECK_NEAR(lum(c), lum(red), 0.01);
    CHECK_EQ(static_cast<int>(c.r), static_cast<int>(c.g)); // 회색을 입혔으니 무채색
    CHECK_EQ(static_cast<int>(c.g), static_cast<int>(c.b));

    // 같은 색끼리면 아무 모드나 항등이어야 한다.
    for (BlendMode m :
         {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity}) {
        const Color8 same = blendPixel8(m, red, red);
        CHECK_NEAR(same.r, 255, 2.0);
        CHECK_NEAR(same.g, 0, 2.0);
    }
}

MARI_TEST(blend_row_respects_mask_and_opacity) {
    u8 dst[4 * 4] = {};
    u8 src[4 * 4] = {};
    for (int i = 0; i < 4; ++i) {
        src[i * 4 + 0] = 255;
        src[i * 4 + 1] = 255;
        src[i * 4 + 2] = 255;
        src[i * 4 + 3] = 255;
    }
    const u8 mask[4] = {0, 128, 255, 255};
    blendRowRgba8(BlendMode::Normal, dst, src, 4, 1.0f, mask);

    CHECK_EQ(static_cast<int>(dst[0 * 4 + 3]), 0);   // 마스크 0 = 통과 없음
    CHECK_NEAR(dst[1 * 4 + 3], 128, 1.0);            // 마스크 128 = 절반
    CHECK_EQ(static_cast<int>(dst[2 * 4 + 3]), 255); // 마스크 255 = 전부

    // 불투명도 0이면 아무 일도 없어야 한다.
    u8 keep[4] = {1, 2, 3, 4};
    blendRowRgba8(BlendMode::Normal, keep, src, 1, 0.0f, nullptr);
    CHECK_EQ(static_cast<int>(keep[0]), 1);
    CHECK_EQ(static_cast<int>(keep[3]), 4);
}

MARI_TEST_MAIN()
