// 합성기 — 평탄화와 **더티 타일만** 재합성하는 경로 검증.
#include <mari/core/compositor.hpp>
#include <mari/core/layer.hpp>
#include <mari/test/harness.hpp>

#include <vector>

using namespace mari;

namespace {

/// 레이어의 한 타일을 단색으로 채운다.
void fillTile(Layer& layer, TileCoord c, Color8 col) {
    auto w = layer.tiles()->writable(c);
    if (!w.ok())
        return;
    TilePtr t = std::move(w).value();
    u8* p = t->mutablePixels();
    for (i32 y = 0; y < kTileSize; ++y)
        for (i32 x = 0; x < kTileSize; ++x) {
            u8* q = p + static_cast<usize>(y) * t->stride() + static_cast<usize>(x) * 4u;
            q[0] = col.r;
            q[1] = col.g;
            q[2] = col.b;
            q[3] = col.a;
        }
}

/// Gray8 마스크 타일을 단색으로 채운다.
void fillMaskTile(TileMap& mask, TileCoord c, u8 v) {
    auto w = mask.writable(c);
    if (!w.ok())
        return;
    TilePtr t = std::move(w).value();
    u8* p = t->mutablePixels();
    for (i32 y = 0; y < kTileSize; ++y)
        for (i32 x = 0; x < kTileSize; ++x)
            p[static_cast<usize>(y) * t->stride() + static_cast<usize>(x)] = v;
}

struct Buffer {
    Rect area;
    std::vector<u8> px;

    explicit Buffer(Rect a)
        : area(a), px(static_cast<usize>(a.width) * static_cast<usize>(a.height) * 4u, u8{0}) {}

    usize stride() const { return static_cast<usize>(area.width) * 4u; }
    u8* data() { return px.data(); }

    Color8 at(i32 x, i32 y) const {
        const usize o =
            static_cast<usize>(y - area.y) * stride() + static_cast<usize>(x - area.x) * 4u;
        return Color8{px[o], px[o + 1], px[o + 2], px[o + 3]};
    }
    void set(i32 x, i32 y, Color8 c) {
        const usize o =
            static_cast<usize>(y - area.y) * stride() + static_cast<usize>(x - area.x) * 4u;
        px[o] = c.r;
        px[o + 1] = c.g;
        px[o + 2] = c.b;
        px[o + 3] = c.a;
    }
};

} // namespace

MARI_TEST(empty_tree_flattens_to_transparent) {
    auto tree = makeLayerTree(Size{128, 128}).value();
    Buffer buf{Rect{0, 0, 128, 128}};
    buf.set(5, 5, Color8{9, 9, 9, 9}); // 쓰레기를 심어 둔다
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK(buf.at(5, 5) == (Color8{0, 0, 0, 0})); // 합성기가 지운다
}

MARI_TEST(bottom_to_top_order_and_opacity) {
    auto tree = makeLayerTree(Size{64, 64}).value();
    auto bottom = tree->addRaster("아래").value();
    auto top = tree->addRaster("위").value();
    fillTile(*bottom, TileCoord{0, 0}, Color8{0, 0, 0, 255});    // 검정
    fillTile(*top, TileCoord{0, 0}, Color8{255, 255, 255, 255}); // 흰색

    Buffer buf{Rect{0, 0, 64, 64}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK(buf.at(10, 10) == (Color8{255, 255, 255, 255})); // 인덱스 1이 위다

    // 위 레이어를 50% 로 → 중간 회색
    top->setOpacity(0.5f);
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_NEAR(buf.at(10, 10).r, 128, 1.0);

    // 숨기면 아래만 남는다
    top->setVisible(false);
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK(buf.at(10, 10) == (Color8{0, 0, 0, 255}));
}

MARI_TEST(layer_blend_mode_is_applied) {
    auto tree = makeLayerTree(Size{64, 64}).value();
    auto bottom = tree->addRaster("아래").value();
    auto top = tree->addRaster("위").value();
    fillTile(*bottom, TileCoord{0, 0}, Color8{128, 128, 128, 255});
    fillTile(*top, TileCoord{0, 0}, Color8{128, 128, 128, 255});
    top->setBlendMode(BlendMode::Multiply);

    Buffer buf{Rect{0, 0, 64, 64}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_EQ(static_cast<int>(buf.at(1, 1).r), 64);
}

MARI_TEST(group_is_composited_as_one_unit) {
    // 그룹 안에 [검정, 흰색] 두 장. 그룹 불투명도 50% 로 투명 배경 위에 올린다.
    // 그룹을 먼저 합치면 흰색이 되고, 그 덩어리가 50% 로 올라간다 → 알파 128.
    auto tree = makeLayerTree(Size{64, 64}).value();
    auto group = tree->addGroup("g").value();
    auto black = tree->addRaster("검정", group->id()).value();
    auto white = tree->addRaster("흰색", group->id()).value();
    fillTile(*black, TileCoord{0, 0}, Color8{0, 0, 0, 255});
    fillTile(*white, TileCoord{0, 0}, Color8{255, 255, 255, 255});
    group->setOpacity(0.5f);

    Buffer buf{Rect{0, 0, 64, 64}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_EQ(static_cast<int>(buf.at(3, 3).r), 255);
    CHECK_NEAR(buf.at(3, 3).a, 128, 1.0);

    // 빈 그룹은 아무 일도 하지 않는다.
    auto empty = tree->addGroup("빈그룹").value();
    empty->setOpacity(1.0f);
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_EQ(static_cast<int>(buf.at(3, 3).r), 255);
}

MARI_TEST(mask_hides_where_it_is_zero) {
    auto tree = makeLayerTree(Size{128, 64}).value();
    auto layer = tree->addRaster("l").value();
    fillTile(*layer, TileCoord{0, 0}, Color8{255, 0, 0, 255});
    fillTile(*layer, TileCoord{1, 0}, Color8{255, 0, 0, 255});

    auto mask = makeTileMap(PixelFormat::Gray8).value();
    fillMaskTile(*mask, TileCoord{0, 0}, 128); // 왼쪽 타일만 절반 통과
    layer->setMask(mask);                      // 오른쪽 타일에는 마스크 타일이 없다

    Buffer buf{Rect{0, 0, 128, 64}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_NEAR(buf.at(10, 10).a, 128, 1.0);
    CHECK_EQ(static_cast<int>(buf.at(100, 10).a), 0); // 마스크 타일 없음 = 완전히 가림
}

MARI_TEST(sparse_layer_only_touches_allocated_tiles) {
    // 4000×4000 캔버스, 점 하나. 나머지는 투명하게 남는다.
    auto tree = makeLayerTree(Size{4000, 4000}).value();
    auto layer = tree->addRaster("l").value();
    fillTile(*layer, TileCoord{62, 62}, Color8{1, 2, 3, 255});
    CHECK_EQ(layer->tiles()->tileCount(), 1u);

    Buffer buf{Rect{0, 0, 256, 256}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK(buf.at(0, 0) == (Color8{0, 0, 0, 0}));

    Buffer far{Rect{62 * 64, 62 * 64, 64, 64}};
    CHECK(tree->flatten(far.area, far.data(), far.stride()).ok());
    CHECK(far.at(62 * 64 + 5, 62 * 64 + 5) == (Color8{1, 2, 3, 255}));
}

// ★ 더티 타일만 다시 합성한다 — docs/02 4절 [5]단계
MARI_TEST(composite_dirty_touches_only_dirty_tiles) {
    auto tree = makeLayerTree(Size{256, 128}).value();
    auto layer = tree->addRaster("l").value();
    fillTile(*layer, TileCoord{0, 0}, Color8{10, 10, 10, 255});
    fillTile(*layer, TileCoord{1, 0}, Color8{20, 20, 20, 255});
    fillTile(*layer, TileCoord{2, 0}, Color8{30, 30, 30, 255});

    Buffer buf{Rect{0, 0, 256, 128}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_EQ(static_cast<int>(buf.at(70, 10).r), 20);

    // 타일 (1,0) 만 바꾼다. 다른 타일 자리에는 감시용 쓰레기를 심어 둔다.
    fillTile(*layer, TileCoord{1, 0}, Color8{200, 200, 200, 255});
    buf.set(10, 10, Color8{99, 99, 99, 99});  // 타일 (0,0)
    buf.set(140, 10, Color8{88, 88, 88, 88}); // 타일 (2,0)

    DirtyTiles dirty;
    dirty.push_back(TileCoord{1, 0});
    dirty.push_back(TileCoord{1, 0});   // 엔진은 중복을 허용한다
    dirty.push_back(TileCoord{50, 50}); // 화면 밖 더티는 무시돼야 한다
    CHECK(compositeDirty(*tree, dirty, buf.area, buf.data(), buf.stride()).ok());

    CHECK_EQ(static_cast<int>(buf.at(70, 10).r), 200); // 더티 타일은 갱신됐다
    CHECK(buf.at(10, 10) == (Color8{99, 99, 99, 99})); // 나머지는 손대지 않았다
    CHECK(buf.at(140, 10) == (Color8{88, 88, 88, 88}));
}

MARI_TEST(composite_dirty_clears_before_recompositing) {
    // 레이어를 지웠는데 더티 경로가 예전 픽셀을 남기면 안 된다.
    auto tree = makeLayerTree(Size{64, 64}).value();
    auto layer = tree->addRaster("l").value();
    fillTile(*layer, TileCoord{0, 0}, Color8{255, 0, 0, 255});

    Buffer buf{Rect{0, 0, 64, 64}};
    CHECK(tree->flatten(buf.area, buf.data(), buf.stride()).ok());
    CHECK_EQ(static_cast<int>(buf.at(1, 1).r), 255);

    layer->tiles()->erase(TileCoord{0, 0});
    DirtyTiles dirty{TileCoord{0, 0}};
    CHECK(compositeDirty(*tree, dirty, buf.area, buf.data(), buf.stride()).ok());
    CHECK(buf.at(1, 1) == (Color8{0, 0, 0, 0}));
}

MARI_TEST(dirty_helpers_dedup_and_bound) {
    DirtyTiles d{TileCoord{1, 1}, TileCoord{0, 0}, TileCoord{1, 1}, TileCoord{0, 0}};
    dedupDirtyTiles(d);
    CHECK_EQ(d.size(), 2u);
    CHECK(d[0] == (TileCoord{0, 0}));
    CHECK(d[1] == (TileCoord{1, 1}));
    CHECK_EQ(dirtyTilesBounds(d), (Rect{0, 0, 128, 128}));
    CHECK_EQ(dirtyTilesBounds(DirtyTiles{}), (Rect{}));
}

MARI_TEST(composite_rejects_bad_arguments) {
    auto tree = makeLayerTree(Size{64, 64}).value();
    std::vector<u8> px(64 * 64 * 4, u8{0});
    CHECK(!compositeArea(*tree, Rect{0, 0, 64, 64}, nullptr, 256).ok());
    CHECK(!compositeArea(*tree, Rect{0, 0, 64, 64}, px.data(), 10).ok()); // stride 부족
    CHECK(compositeArea(*tree, Rect{}, px.data(), 256).ok());             // 빈 영역은 무해
}

MARI_TEST_MAIN()
