// 타일 캔버스 — 희소성과 Copy-on-Write 검증.
// docs/02 3절의 핵심 주장("구석에 점 하나 = 타일 1개")을 여기서 증명한다.
#include <mari/core/tile.hpp>
#include <mari/test/harness.hpp>

#include <cstring>

using namespace mari;

namespace {

/// 캔버스 좌표 (x,y) 에 RGBA 한 픽셀을 찍는다.
void dot(TileMap& map, i32 x, i32 y, Color8 c) {
    const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
    auto w = map.writable(tc);
    if (!w.ok())
        return;
    TilePtr t = std::move(w).value();
    u8* p = t->mutablePixels() + static_cast<usize>(y - tileOrigin(tc.ty)) * t->stride() +
            static_cast<usize>(x - tileOrigin(tc.tx)) * 4u;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

/// 캔버스 좌표 (x,y) 의 픽셀을 읽는다. 타일이 없으면 완전 투명.
Color8 peek(const TileMap& map, i32 x, i32 y) {
    const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
    ConstTilePtr t = map.at(tc);
    if (!t)
        return Color8{0, 0, 0, 0};
    const u8* p = t->pixels() + static_cast<usize>(y - tileOrigin(tc.ty)) * t->stride() +
                  static_cast<usize>(x - tileOrigin(tc.tx)) * 4u;
    return Color8{p[0], p[1], p[2], p[3]};
}

} // namespace

MARI_TEST(make_tilemap_rejects_unknown_format) {
    auto bad = makeTileMap(PixelFormat::Unknown);
    CHECK(!bad.ok());
    CHECK(bad.code() == ErrorCode::InvalidArgument);

    auto good = makeTileMap(PixelFormat::RGBA8);
    CHECK(good.ok());
    CHECK(good.value()->format() == PixelFormat::RGBA8);
    CHECK_EQ(good.value()->tileCount(), 0u);
    CHECK(good.value()->at(TileCoord{0, 0}) == nullptr); // 없는 타일 = 완전 투명
}

// ★ docs/02 3절의 핵심 주장:
//   "4000×4000 캔버스에서 구석에 점 하나 찍으면 타일 1개만 메모리에 잡힌다"
MARI_TEST(single_dot_on_4000px_canvas_allocates_one_tile) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();

    // 4000×4000 을 전부 잡으면 63×63 = 3969 타일이다. 지금은 0개여야 한다.
    CHECK_EQ(map->tileCount(), 0u);

    dot(*map, 3999, 3999, Color8{255, 0, 0, 255}); // 오른쪽 아래 구석

    CHECK_EQ(map->tileCount(), 1u); // ← 이게 "가볍다"의 실체다
    CHECK_EQ(map->bounds(), (Rect{62 * 64, 62 * 64, 64, 64}));
    CHECK(peek(*map, 3999, 3999) == (Color8{255, 0, 0, 255}));
    CHECK(peek(*map, 0, 0) == (Color8{0, 0, 0, 0})); // 나머지는 메모리조차 없다

    // 반대편 구석을 하나 더 찍어도 2개다. 사이는 여전히 비어 있다.
    dot(*map, 0, 0, Color8{0, 255, 0, 255});
    CHECK_EQ(map->tileCount(), 2u);
    CHECK_EQ(map->bounds(), (Rect{0, 0, 63 * 64, 63 * 64}));
}

MARI_TEST(blank_tile_is_reported_blank) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    auto w = map->writable(TileCoord{0, 0});
    CHECK(w.ok());
    CHECK(w.value()->isBlank()); // 새 타일은 전부 0

    dot(*map, 1, 1, Color8{9, 9, 9, 255});
    CHECK(!map->at(TileCoord{0, 0})->isBlank());
}

MARI_TEST(snapshot_is_independent_after_write) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    dot(*map, 10, 10, Color8{10, 20, 30, 255});

    auto snap = map->snapshot();
    CHECK_EQ(snap->tileCount(), 1u);

    // 원본을 고친다 → 스냅샷은 그대로여야 한다 (COW 정확성)
    dot(*map, 10, 10, Color8{200, 200, 200, 255});
    CHECK(peek(*map, 10, 10) == (Color8{200, 200, 200, 255}));
    CHECK(peek(*snap, 10, 10) == (Color8{10, 20, 30, 255}));

    // 반대 방향도 성립한다.
    dot(*snap, 10, 10, Color8{1, 1, 1, 255});
    CHECK(peek(*map, 10, 10) == (Color8{200, 200, 200, 255}));

    // 새 타일 추가/삭제도 서로에게 새지 않는다.
    dot(*map, 500, 500, Color8{5, 5, 5, 255});
    CHECK_EQ(map->tileCount(), 2u);
    CHECK_EQ(snap->tileCount(), 1u);
    map->erase(TileCoord{0, 0});
    CHECK_EQ(snap->tileCount(), 1u);
    CHECK(peek(*snap, 10, 10) == (Color8{1, 1, 1, 255}));
}

MARI_TEST(held_writable_pointer_stays_valid_across_snapshots) {
    // writable() 이 돌려준 포인터를 붙잡은 채 스냅샷을 떠도 쓰기가 원본에 반영돼야 한다.
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    TilePtr t = map->writable(TileCoord{0, 0}).value();
    auto snap = map->snapshot();

    t->mutablePixels()[3] = 255; // 첫 픽셀 알파
    CHECK_EQ(peek(*map, 0, 0).a, 255);
    CHECK_EQ(peek(*snap, 0, 0).a, 0); // 스냅샷은 그대로
}

MARI_TEST(clear_does_not_touch_snapshot) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    dot(*map, 0, 0, Color8{1, 2, 3, 255});
    auto snap = map->snapshot();
    map->clear();
    CHECK_EQ(map->tileCount(), 0u);
    CHECK_EQ(snap->tileCount(), 1u);
}

MARI_TEST(collect_tiles_appends_only_allocated_ones) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    dot(*map, 0, 0, Color8{1, 1, 1, 255});     // 타일 (0,0)
    dot(*map, 640, 640, Color8{1, 1, 1, 255}); // 타일 (10,10)

    DirtyTiles out;
    map->collectTiles(Rect{0, 0, 64, 64}, out);
    CHECK_EQ(out.size(), 1u);
    CHECK(out[0] == (TileCoord{0, 0}));

    // 덧붙이는 게 계약이다 — 비우지 않는다.
    map->collectTiles(Rect{600, 600, 100, 100}, out);
    CHECK_EQ(out.size(), 2u);
    CHECK(out[1] == (TileCoord{10, 10}));

    // 넓은 영역을 훑어도 할당되지 않은 타일은 안 나온다.
    out.clear();
    map->collectTiles(Rect{0, 0, 4000, 4000}, out);
    CHECK_EQ(out.size(), 2u);

    out.clear();
    map->collectTiles(Rect{5000, 5000, 10, 10}, out);
    CHECK_EQ(out.size(), 0u);
}

MARI_TEST(negative_tile_coords_work) {
    // 캔버스 밖으로 나가는 레이어를 허용한다.
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    CHECK_EQ(tileIndexFor(-1), -1);
    dot(*map, -1, -1, Color8{7, 7, 7, 255});
    CHECK_EQ(map->tileCount(), 1u);
    CHECK(peek(*map, -1, -1) == (Color8{7, 7, 7, 255}));
    CHECK_EQ(map->bounds(), (Rect{-64, -64, 64, 64}));
}

MARI_TEST(gray8_mask_tilemap_works) {
    auto mask = makeTileMap(PixelFormat::Gray8).value();
    CHECK(mask->format() == PixelFormat::Gray8);
    auto w = mask->writable(TileCoord{0, 0});
    CHECK(w.ok());
    CHECK_EQ(w.value()->stride(), static_cast<usize>(kTileSize));
    CHECK(w.value()->isBlank());
    w.value()->mutablePixels()[0] = 255;
    CHECK(!mask->at(TileCoord{0, 0})->isBlank());
}

MARI_TEST_MAIN()
