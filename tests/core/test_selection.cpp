// 선택 마스크 — 정확성과 **메모리 0** 검증.
//
// 여기서 증명하는 것:
//   · 전체 선택·빈 선택은 타일 0개다(희소 저장의 요점)
//   · 각 선택 방식이 알려진 입출력을 낸다(사각형·타원·올가미·색상·내용·알파)
//   · 불린 결합 4종이 정확하다
//   · 반전이 캔버스를 할당하지 않는다
//   · 페더가 실제로 가장자리를 중간값으로 만든다
//   · 팽창·수축이 원형이다
#include <mari/core/selection.hpp>
#include <mari/core/tile.hpp>
#include <mari/test/harness.hpp>

using namespace mari;

namespace {

constexpr Size kCanvas{256, 256};

/// RGBA8 타일맵에 사각형을 칠한다(선택의 원본 레이어 역할).
void paint(TileMap& map, const Rect& r, Color8 c) {
    for (i32 y = r.y; y < r.bottom(); ++y) {
        for (i32 x = r.x; x < r.right(); ++x) {
            const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
            auto w = map.writable(tc);
            if (!w.ok()) {
                return;
            }
            const TilePtr t = w.value();
            u8* p = t->mutablePixels() + static_cast<usize>(y - tileOrigin(tc.ty)) * t->stride() +
                    static_cast<usize>(x - tileOrigin(tc.tx)) * 4u;
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = c.a;
        }
    }
}

TileMapPtr rgbaMap() {
    auto m = makeTileMap(PixelFormat::RGBA8);
    return m.ok() ? m.value() : nullptr;
}

} // namespace

// ── 🔴 전체 선택 = 타일 0개 ─────────────────────────────────────────────

MARI_TEST(full_and_empty_selection_allocate_no_tiles) {
    const SelectionMask all = SelectionMask::all(kCanvas);
    CHECK(all.isAll());
    CHECK(!all.isEmpty());
    CHECK_EQ(all.tileCount(), usize{0}); // 🔴 메모리 0
    CHECK_EQ(all.valueAt(0, 0), u8{255});
    CHECK_EQ(all.valueAt(255, 255), u8{255});
    // 캔버스 밖은 언제나 선택되지 않는다 — 그래서 반전이 잘 정의된다.
    CHECK_EQ(all.valueAt(256, 0), u8{0});
    CHECK_EQ(all.valueAt(-1, 0), u8{0});
    CHECK_EQ(all.bounds(), (Rect{0, 0, 256, 256}));
    CHECK_EQ(all.selectedPixels(), u64{256} * 256u);

    const SelectionMask none = SelectionMask::empty(kCanvas);
    CHECK(none.isEmpty());
    CHECK(!none.isAll());
    CHECK_EQ(none.tileCount(), usize{0}); // 🔴 메모리 0
    CHECK_EQ(none.valueAt(10, 10), u8{0});
    CHECK(none.bounds().isEmpty());
    CHECK_EQ(none.selectedPixels(), u64{0});
}

MARI_TEST(rect_covering_whole_canvas_collapses_to_zero_tiles) {
    auto full = SelectionMask::fromRect(kCanvas, Rect{0, 0, 256, 256});
    CHECK(full.ok());
    if (full.ok()) {
        CHECK(full.value().isAll());
        CHECK_EQ(full.value().tileCount(), usize{0});
    }
    // 캔버스를 넘어서는 사각형도 캔버스로 잘려 전체 선택이 된다.
    auto over = SelectionMask::fromRect(kCanvas, Rect{-100, -100, 1000, 1000});
    CHECK(over.ok());
    if (over.ok()) {
        CHECK(over.value().isAll());
        CHECK_EQ(over.value().tileCount(), usize{0});
    }
}

// ── 사각형 ──────────────────────────────────────────────────────────────

MARI_TEST(rect_selection_is_exact) {
    auto r = SelectionMask::fromRect(kCanvas, Rect{10, 20, 30, 40});
    CHECK(r.ok());
    if (!r.ok()) {
        return;
    }
    const SelectionMask& m = r.value();
    CHECK_EQ(m.valueAt(10, 20), u8{255});
    CHECK_EQ(m.valueAt(39, 59), u8{255});
    CHECK_EQ(m.valueAt(9, 20), u8{0});   // 왼쪽 한 칸 밖
    CHECK_EQ(m.valueAt(40, 20), u8{0});  // 반열림 — 오른쪽 경계는 포함되지 않는다
    CHECK_EQ(m.valueAt(10, 60), u8{0});
    CHECK_EQ(m.bounds(), (Rect{10, 20, 30, 40}));
    CHECK_EQ(m.selectedPixels(), u64{30} * 40u);
    // 30×40 은 타일 하나에 다 안 들어간다(경계를 걸친다) — 하지만 캔버스 전체(16개)는 아니다.
    CHECK(m.tileCount() >= 1);
    CHECK(m.tileCount() <= 4);
}

// ── 타원 ────────────────────────────────────────────────────────────────

MARI_TEST(ellipse_selection_is_round_and_antialiased) {
    auto e = SelectionMask::fromEllipse(kCanvas, Rect{0, 0, 101, 101}, true);
    CHECK(e.ok());
    if (!e.ok()) {
        return;
    }
    const SelectionMask& m = e.value();
    CHECK_EQ(m.valueAt(50, 50), u8{255}); // 한가운데
    CHECK_EQ(m.valueAt(0, 0), u8{0});     // 모서리는 원 밖
    CHECK_EQ(m.valueAt(100, 100), u8{0});
    // 면적 ≈ πr² (r=50.5). 2% 안쪽이면 "원을 그렸다"고 말할 수 있다.
    const auto want = static_cast<f64>(3.14159265 * 50.5 * 50.5);
    const auto got = static_cast<f64>(m.selectedPixels());
    CHECK(got > want * 0.98);
    CHECK(got < want * 1.02);
    // 가장자리에 중간값이 실제로 있다 = 안티에일리어싱이 돌았다.
    usize partial = 0;
    for (i32 y = 0; y < 101; ++y) {
        for (i32 x = 0; x < 101; ++x) {
            const u8 v = m.valueAt(x, y);
            partial += (v > 0 && v < 255) ? 1u : 0u;
        }
    }
    CHECK(partial > 100); // 둘레(≈317px)의 상당수가 부분 커버리지여야 한다

    // 안티에일리어싱을 끄면 중간값이 **하나도** 없다.
    auto hard = SelectionMask::fromEllipse(kCanvas, Rect{0, 0, 101, 101}, false);
    CHECK(hard.ok());
    if (hard.ok()) {
        usize mid = 0;
        for (i32 y = 0; y < 101; ++y) {
            for (i32 x = 0; x < 101; ++x) {
                const u8 v = hard.value().valueAt(x, y);
                mid += (v > 0 && v < 255) ? 1u : 0u;
            }
        }
        CHECK_EQ(mid, usize{0});
    }
}

// ── 올가미(폴리곤) ──────────────────────────────────────────────────────

MARI_TEST(lasso_polygon_uses_even_odd_rule) {
    // 삼각형 (0,0)-(100,0)-(0,100). 면적 5000 이어야 한다.
    const std::vector<PointF> tri{{0.0f, 0.0f}, {100.0f, 0.0f}, {0.0f, 100.0f}};
    auto t = SelectionMask::fromPolygon(kCanvas, tri, true);
    CHECK(t.ok());
    if (!t.ok()) {
        return;
    }
    const SelectionMask& m = t.value();
    CHECK_EQ(m.valueAt(10, 10), u8{255});  // 확실히 안
    CHECK_EQ(m.valueAt(90, 90), u8{0});    // 확실히 밖(빗변 너머)
    const auto got = static_cast<f64>(m.selectedPixels());
    CHECK(got > 5000.0 * 0.97);
    CHECK(got < 5000.0 * 1.03);

    // 점이 모자라면 조용히 빈 선택을 주지 않는다. 거절한다.
    const std::vector<PointF> two{{0.0f, 0.0f}, {1.0f, 1.0f}};
    CHECK(!SelectionMask::fromPolygon(kCanvas, two, true).ok());
}

MARI_TEST(lasso_with_hole_subtracts_by_even_odd) {
    // 바깥 사각형 안에 반대 방향 사각형을 이어 붙여 구멍을 만든다.
    const std::vector<PointF> poly{{0, 0},   {100, 0},  {100, 100}, {0, 100}, {0, 0},
                                   {25, 25}, {25, 75},  {75, 75},   {75, 25}, {25, 25}};
    auto r = SelectionMask::fromPolygon(kCanvas, poly, false);
    CHECK(r.ok());
    if (!r.ok()) {
        return;
    }
    CHECK_EQ(r.value().valueAt(10, 50), u8{255}); // 테두리
    CHECK_EQ(r.value().valueAt(50, 50), u8{0});   // 🔴 구멍 — 짝수-홀수 규칙이 뚫었다
}

// ── 색상 범위 · 내용 · 레이어 알파 ──────────────────────────────────────

MARI_TEST(color_range_content_and_alpha_selections) {
    const TileMapPtr src = rgbaMap();
    CHECK(src != nullptr);
    if (src == nullptr) {
        return;
    }
    paint(*src, Rect{0, 0, 20, 20}, Color8::rgba(255, 0, 0, 255));
    paint(*src, Rect{40, 40, 20, 20}, Color8::rgba(0, 0, 255, 255));
    paint(*src, Rect{80, 80, 10, 10}, Color8::rgba(255, 0, 0, 128)); // 반투명 빨강

    // 색상 범위: 완전 불투명 빨강만.
    auto red = SelectionMask::fromColorRange(kCanvas, *src, Color8::rgba(255, 0, 0, 255), 0);
    CHECK(red.ok());
    if (red.ok()) {
        CHECK_EQ(red.value().valueAt(5, 5), u8{255});
        CHECK_EQ(red.value().valueAt(45, 45), u8{0});
        CHECK_EQ(red.value().valueAt(85, 85), u8{0}); // 알파가 다르다
        CHECK_EQ(red.value().selectedPixels(), u64{400});
        CHECK(!red.value().isAll());
    }
    // 허용 오차를 128 로 열면 반투명 빨강까지 들어온다.
    auto loose = SelectionMask::fromColorRange(kCanvas, *src, Color8::rgba(255, 0, 0, 255), 130);
    CHECK(loose.ok());
    if (loose.ok()) {
        CHECK_EQ(loose.value().valueAt(85, 85), u8{255});
    }

    // 🔴 기준색이 "완전 투명"이면 할당되지 않은 타일까지 선택된다 — 타일은 여전히 0개가 아니다
    //    (칠해진 곳을 도려내야 하므로 그 타일만 생긴다).
    auto clear = SelectionMask::fromColorRange(kCanvas, *src, Color8::rgba(0, 0, 0, 0), 0);
    CHECK(clear.ok());
    if (clear.ok()) {
        CHECK_EQ(clear.value().outsideValue(), u8{255});
        CHECK_EQ(clear.value().valueAt(200, 200), u8{255}); // 빈 곳
        CHECK_EQ(clear.value().valueAt(5, 5), u8{0});       // 빨강 자리
    }

    // 내용 기반(nonEmpty): 알파가 있는 곳 전부.
    auto content = SelectionMask::fromContent(kCanvas, *src, 1);
    CHECK(content.ok());
    if (content.ok()) {
        CHECK_EQ(content.value().selectedPixels(), u64{400 + 400 + 100});
        CHECK_EQ(content.value().valueAt(85, 85), u8{255});
    }

    // 레이어 알파: 반투명은 **반투명한 선택**이 된다.
    auto alpha = SelectionMask::fromLayerAlpha(kCanvas, *src);
    CHECK(alpha.ok());
    if (alpha.ok()) {
        CHECK_EQ(alpha.value().valueAt(5, 5), u8{255});
        CHECK_EQ(alpha.value().valueAt(85, 85), u8{128});
    }
}

// ── 불린 결합 ───────────────────────────────────────────────────────────

MARI_TEST(boolean_combinations_are_exact) {
    const Rect a{0, 0, 100, 100};
    const Rect b{50, 50, 100, 100};

    const auto make = [&](const Rect& r) {
        auto m = SelectionMask::fromRect(kCanvas, r);
        return m.ok() ? m.value() : SelectionMask::empty(kCanvas);
    };

    { // 합집합 — 10000 + 10000 − 2500(겹침)
        SelectionMask m = make(a);
        CHECK(m.combine(make(b), SelectionOp::Union).ok());
        CHECK_EQ(m.selectedPixels(), u64{17500});
        CHECK_EQ(m.valueAt(10, 10), u8{255});
        CHECK_EQ(m.valueAt(140, 140), u8{255});
    }
    { // 교집합 — 겹치는 50×50
        SelectionMask m = make(a);
        CHECK(m.combine(make(b), SelectionOp::Intersect).ok());
        CHECK_EQ(m.selectedPixels(), u64{2500});
        CHECK_EQ(m.bounds(), (Rect{50, 50, 50, 50}));
    }
    { // 차집합 — a 에서 겹침을 뺀다
        SelectionMask m = make(a);
        CHECK(m.combine(make(b), SelectionOp::Subtract).ok());
        CHECK_EQ(m.selectedPixels(), u64{7500});
        CHECK_EQ(m.valueAt(60, 60), u8{0});
        CHECK_EQ(m.valueAt(10, 10), u8{255});
    }
    { // 대칭차 — 겹침만 빠진다
        SelectionMask m = make(a);
        CHECK(m.combine(make(b), SelectionOp::Xor).ok());
        CHECK_EQ(m.selectedPixels(), u64{15000});
        CHECK_EQ(m.valueAt(60, 60), u8{0});
        CHECK_EQ(m.valueAt(140, 140), u8{255});
    }
    { // 자기 자신과의 차집합 = 빈 선택 = **타일 0개로 되돌아간다**
        SelectionMask m = make(a);
        CHECK(m.combine(make(a), SelectionOp::Subtract).ok());
        CHECK(m.isEmpty());
        CHECK_EQ(m.tileCount(), usize{0});
    }
    { // 캔버스 크기가 다르면 거절한다 — 조용히 맞춰 주지 않는다
        SelectionMask m = make(a);
        CHECK(!m.combine(SelectionMask::all(Size{64, 64}), SelectionOp::Union).ok());
    }
}

// ── 반전 ────────────────────────────────────────────────────────────────

MARI_TEST(invert_does_not_allocate_the_canvas) {
    auto r = SelectionMask::fromRect(kCanvas, Rect{64, 64, 64, 64}); // 타일 하나에 딱 맞는다
    CHECK(r.ok());
    if (!r.ok()) {
        return;
    }
    SelectionMask m = r.value();
    const usize before = m.tileCount();
    CHECK_EQ(before, usize{1});

    m.invert();
    // 🔴 반전해도 타일 수가 그대로다. 캔버스(16타일)를 할당하지 않는다.
    CHECK_EQ(m.tileCount(), usize{1});
    CHECK_EQ(m.outsideValue(), u8{255});
    CHECK_EQ(m.valueAt(100, 100), u8{0});
    CHECK_EQ(m.valueAt(10, 10), u8{255});
    CHECK_EQ(m.selectedPixels(), u64{256} * 256u - 64u * 64u);

    m.invert(); // 두 번 뒤집으면 제자리
    CHECK_EQ(m.tileCount(), usize{1});
    CHECK_EQ(m.selectedPixels(), u64{64} * 64u);

    // 전체 선택의 반전 = 빈 선택. 양쪽 다 타일 0개.
    SelectionMask all = SelectionMask::all(kCanvas);
    all.invert();
    CHECK(all.isEmpty());
    CHECK_EQ(all.tileCount(), usize{0});
}

// ── 페더 ────────────────────────────────────────────────────────────────

MARI_TEST(feather_actually_softens_the_edge) {
    auto r = SelectionMask::fromRect(kCanvas, Rect{80, 80, 96, 96});
    CHECK(r.ok());
    if (!r.ok()) {
        return;
    }
    SelectionMask m = r.value();
    // 페더 전: 경계가 딱 떨어진다.
    CHECK_EQ(m.valueAt(79, 128), u8{0});
    CHECK_EQ(m.valueAt(80, 128), u8{255});

    CHECK(m.feather(8.0f).ok());
    // 경계에 중간값이 생겼다. 안쪽은 여전히 꽉 차 있고 먼 바깥은 여전히 0이다.
    const u8 edge = m.valueAt(80, 128);
    CHECK(edge > 0);
    CHECK(edge < 255);
    CHECK_EQ(m.valueAt(128, 128), u8{255});
    CHECK_EQ(m.valueAt(40, 128), u8{0});
    // 바깥으로도 번진다(경계 바로 밖이 0이 아니다).
    CHECK(m.valueAt(78, 128) > 0);
    // 단조 감소 — 안에서 밖으로 갈수록 약해진다.
    for (i32 x = 76; x < 90; ++x) {
        CHECK(m.valueAt(x, 128) <= m.valueAt(x + 1, 128));
    }

    // 🔴 전체 선택을 페더해도 테두리가 깎이지 않는다(캔버스 밖을 가장자리 값으로 연장한다).
    SelectionMask all = SelectionMask::all(kCanvas);
    CHECK(all.feather(8.0f).ok());
    CHECK(all.isAll());
    CHECK_EQ(all.tileCount(), usize{0});
}

// ── 팽창 · 수축 ─────────────────────────────────────────────────────────

MARI_TEST(expand_and_contract_are_circular) {
    auto r = SelectionMask::fromRect(kCanvas, Rect{100, 100, 40, 40});
    CHECK(r.ok());
    if (!r.ok()) {
        return;
    }
    SelectionMask m = r.value();
    CHECK(m.expand(10).ok());
    // 변의 한가운데는 정확히 10px 나간다.
    CHECK_EQ(m.valueAt(90, 120), u8{255});
    CHECK_EQ(m.valueAt(89, 120), u8{0});
    // 🔴 모서리는 **둥글다** — 사각형 팽창이었다면 (90,90) 이 선택됐을 것이다.
    CHECK_EQ(m.valueAt(90, 90), u8{0});
    CHECK_EQ(m.valueAt(97, 97), u8{255}); // 대각선으로는 10/√2 ≈ 7 만 나간다

    SelectionMask c = r.value();
    CHECK(c.expand(-10).ok());
    CHECK_EQ(c.bounds(), (Rect{110, 110, 20, 20}));
    CHECK_EQ(c.selectedPixels(), u64{20} * 20u);

    // 자기 크기보다 크게 수축하면 사라진다 — 타일 0개로 돌아간다.
    SelectionMask gone = r.value();
    CHECK(gone.expand(-100).ok());
    CHECK(gone.isEmpty());
    CHECK_EQ(gone.tileCount(), usize{0});
}

// ── COW 복사 ────────────────────────────────────────────────────────────

MARI_TEST(copy_is_independent_and_shares_pixels) {
    auto r = SelectionMask::fromRect(kCanvas, Rect{0, 0, 64, 64});
    CHECK(r.ok());
    if (!r.ok()) {
        return;
    }
    SelectionMask a = r.value();
    SelectionMask b = a; // COW 사본 — 픽셀 복사 0
    CHECK_EQ(b.selectedPixels(), a.selectedPixels());
    b.invert();
    // 사본을 고쳐도 원본은 그대로다(양방향 독립).
    CHECK_EQ(a.selectedPixels(), u64{64} * 64u);
    CHECK_EQ(b.selectedPixels(), u64{256} * 256u - 64u * 64u);
    CHECK_EQ(a.valueAt(10, 10), u8{255});
    CHECK_EQ(b.valueAt(10, 10), u8{0});
}

MARI_TEST(selection_op_names_round_trip) {
    for (const SelectionOp op : {SelectionOp::Replace, SelectionOp::Union, SelectionOp::Subtract,
                                 SelectionOp::Intersect, SelectionOp::Xor}) {
        const auto back = selectionOpFromName(selectionOpName(op));
        CHECK(back.ok());
        if (back.ok()) {
            CHECK(back.value() == op);
        }
    }
    CHECK(!selectionOpFromName("그런건없다").ok());
}

// ── 플러드(마술봉·페인트통) ───────────────────────────────────────────────

MARI_TEST(flood_selects_only_the_contiguous_region) {
    auto map = rgbaMap();
    // 빨간 테두리 사각형(구멍 뚫린 상자). 안쪽은 투명, 바깥도 투명 — 연결돼 있지 않다.
    paint(*map, Rect{40, 40, 100, 4}, Color8::rgba(255, 0, 0, 255));
    paint(*map, Rect{40, 136, 100, 4}, Color8::rgba(255, 0, 0, 255));
    paint(*map, Rect{40, 40, 4, 100}, Color8::rgba(255, 0, 0, 255));
    paint(*map, Rect{136, 40, 4, 100}, Color8::rgba(255, 0, 0, 255));

    auto inside = SelectionMask::fromFlood(kCanvas, *map, 90, 90, 0);
    CHECK(inside.ok());
    CHECK_EQ(inside.value().valueAt(90, 90), static_cast<u8>(255));
    CHECK_EQ(inside.value().valueAt(60, 100), static_cast<u8>(255));
    CHECK_EQ(inside.value().valueAt(41, 41), static_cast<u8>(0));  // 테두리
    CHECK_EQ(inside.value().valueAt(10, 10), static_cast<u8>(0));  // 바깥(연결 안 됨)
    // 안쪽 넓이 = 92×92
    CHECK_EQ(inside.value().selectedPixels(), 92ull * 92ull);

    // 테두리 색을 찍으면 테두리만.
    auto edge = SelectionMask::fromFlood(kCanvas, *map, 41, 41, 0);
    CHECK(edge.ok());
    CHECK_EQ(edge.value().valueAt(41, 41), static_cast<u8>(255));
    CHECK_EQ(edge.value().valueAt(90, 90), static_cast<u8>(0));
}

MARI_TEST(flood_gap_close_stops_leak_through_small_hole) {
    auto map = rgbaMap();
    paint(*map, Rect{40, 40, 100, 4}, Color8::rgba(0, 0, 0, 255));
    paint(*map, Rect{40, 136, 100, 4}, Color8::rgba(0, 0, 0, 255));
    paint(*map, Rect{40, 40, 4, 100}, Color8::rgba(0, 0, 0, 255));
    paint(*map, Rect{136, 40, 4, 100}, Color8::rgba(0, 0, 0, 255));
    // 오른쪽 벽에 3px 구멍
    paint(*map, Rect{136, 80, 4, 3}, Color8::rgba(0, 0, 0, 0));

    auto leak = SelectionMask::fromFlood(kCanvas, *map, 90, 90, 0, 0);
    CHECK(leak.ok());
    CHECK_EQ(leak.value().valueAt(200, 200), static_cast<u8>(255)); // 새어 나간다

    auto closed = SelectionMask::fromFlood(kCanvas, *map, 90, 90, 0, 2);
    CHECK(closed.ok());
    CHECK_EQ(closed.value().valueAt(90, 90), static_cast<u8>(255));
    CHECK_EQ(closed.value().valueAt(200, 200), static_cast<u8>(0));  // 틈이 닫혔다
}

MARI_TEST_MAIN()
