// Mari Paint — .psd 왕복 검증: 레이어·그룹·마스크·클리핑·합성 모드·이름·불투명도·가시성이 살아 돌아온다.
#include <mari/core/layer.hpp>
#include <mari/core/selection.hpp>
#include <mari/ora/image.hpp>
#include <mari/psd/psd.hpp>
#include <mari/test/harness.hpp>

#include <vector>

using namespace mari;

namespace {

void fillRect(Layer& l, Rect r, Color8 c) {
    ora::Image8 img = ora::Image8::make(r.width, r.height);
    for (usize i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i] = c.r; img.pixels[i + 1] = c.g; img.pixels[i + 2] = c.b; img.pixels[i + 3] = c.a;
    }
    (void)ora::writeRegion(*l.tiles(), r, img);
}

Color8 pixel(const TileMap& m, i32 x, i32 y) {
    Result<ora::Image8> px = ora::readRegion(m, Rect{x, y, 1, 1});
    if (!px.ok()) return Color8{};
    return Color8{px.value().pixels[0], px.value().pixels[1], px.value().pixels[2], px.value().pixels[3]};
}

} // namespace

MARI_TEST(psd_round_trip_keeps_structure_and_pixels) {
    Result<LayerTreePtr> made = makeLayerTree(Size{100, 80});
    CHECK(made.ok());
    LayerTree& tree = *made.value();
    Result<LayerPtr> base = tree.addRaster("배경 ✎");
    CHECK(base.ok());
    fillRect(*base.value(), Rect{0, 0, 100, 80}, Color8::rgba(10, 20, 30));
    Result<LayerPtr> g = tree.addGroup("그룹");
    CHECK(g.ok());
    g.value()->setOpacity(0.5f);
    Result<LayerPtr> top = tree.addRaster("위", g.value()->id());
    CHECK(top.ok());
    fillRect(*top.value(), Rect{20, 10, 30, 30}, Color8::rgba(200, 100, 50, 128));
    top.value()->setBlendMode(BlendMode::Multiply);
    top.value()->setClipToBelow(true);
    top.value()->setVisible(false);
    // 마스크: 왼쪽 반만 보임.
    Result<TileMapPtr> mask = makeTileMap(PixelFormat::Gray8);
    CHECK(mask.ok());
    for (i32 ty = 0; ty <= tileIndexFor(79); ++ty)
        for (i32 tx = 0; tx <= tileIndexFor(99); ++tx) {
            Result<TilePtr> t = mask.value()->writable(TileCoord{tx, ty});
            CHECK(t.ok());
            u8* p = t.value()->mutablePixels();
            for (i32 y = 0; y < kTileSize; ++y)
                for (i32 x = 0; x < kTileSize; ++x)
                    p[static_cast<usize>(y) * t.value()->stride() + x] = (tileOrigin(tx) + x) < 50 ? 255 : 0;
        }
    top.value()->setMask(mask.value());

    Result<std::vector<u8>> bytes = psd::saveToMemory(tree);
    CHECK(bytes.ok());
    if (!bytes.ok()) return;
    CHECK(bytes.value().size() > 100);
    CHECK(std::string(reinterpret_cast<const char*>(bytes.value().data()), 4) == "8BPS");

    Result<psd::Document> back = psd::loadFromMemory(bytes.value().data(), bytes.value().size());
    CHECK(back.ok());
    if (!back.ok()) return;
    const LayerTree& t2 = *back.value().tree;
    CHECK(t2.canvasSize() == (Size{100, 80}));
    CHECK_EQ(t2.roots().size(), usize{2});
    const LayerPtr b2 = t2.roots()[0];
    const LayerPtr g2 = t2.roots()[1];
    CHECK_EQ(b2->name(), std::string("배경 ✎")); // luni 유니코드
    CHECK(g2->kind() == LayerKind::Group);
    CHECK_EQ(g2->name(), std::string("그룹"));
    CHECK_NEAR(g2->opacity(), 0.5f, 0.01f);
    CHECK_EQ(g2->children().size(), usize{1});
    const LayerPtr t3 = g2->children()[0];
    CHECK_EQ(t3->name(), std::string("위"));
    CHECK(t3->blendMode() == BlendMode::Multiply);
    CHECK(t3->clipToBelow());
    CHECK(!t3->visible());
    CHECK(t3->mask() != nullptr);
    // 픽셀
    const Color8 c = pixel(*b2->tiles(), 5, 5);
    CHECK_EQ(int(c.r), 10); CHECK_EQ(int(c.g), 20); CHECK_EQ(int(c.b), 30); CHECK_EQ(int(c.a), 255);
    const Color8 d = pixel(*t3->tiles(), 25, 15);
    CHECK_EQ(int(d.r), 200); CHECK_EQ(int(d.a), 128);
    CHECK_EQ(int(pixel(*t3->tiles(), 5, 5).a), 0);
    // 마스크 값
    const ConstTilePtr mt = t3->mask()->at(TileCoord{0, 0});
    CHECK(mt != nullptr);
    if (mt) {
        CHECK_EQ(int(mt->pixels()[10]), 255);
        CHECK_EQ(int(mt->pixels()[60]), 0);
    }
    CHECK(back.value().warnings.empty());
}

MARI_TEST(psd_rejects_what_it_cannot_read_honestly) {
    const u8 junk[] = {'8', 'B', 'P', 'S', 0, 2, 0, 0, 0, 0, 0, 0};
    CHECK(!psd::loadFromMemory(junk, sizeof junk).ok()); // PSB
    const u8 notPsd[] = {'P', 'K', 3, 4};
    CHECK(!psd::loadFromMemory(notPsd, sizeof notPsd).ok());
    CHECK(!psd::load("/없는/파일.psd").ok());
}

MARI_TEST(psd_rejects_absurd_layer_rects_instead_of_allocating) {
    // 정상 문서를 만든 뒤 첫 레이어 레코드의 bottom/right 를 거대하게 고친다 → bad_alloc 이 아니라 Err.
    Result<LayerTreePtr> made = makeLayerTree(Size{8, 8});
    CHECK(made.ok());
    Result<LayerPtr> base = made.value()->addRaster("a");
    CHECK(base.ok());
    fillRect(*base.value(), Rect{0, 0, 8, 8}, Color8::rgba(1, 2, 3));
    Result<std::vector<u8>> bytes = psd::saveToMemory(*made.value());
    CHECK(bytes.ok());
    if (!bytes.ok()) return;
    std::vector<u8>& b = bytes.value();
    // 헤더 26 + colorLen(4) + resLen(4) + lmLen(4) + layerInfoLen(4) + count(2) → 레코드 시작. top,left,bottom,right.
    const usize rec = 26 + 4 + 4 + 4 + 4 + 2;
    CHECK(b.size() > rec + 16);
    for (usize k = 0; k < 4; ++k) { b[rec + 8 + k] = 0x7F; b[rec + 12 + k] = 0x7F; } // bottom, right = 0x7F7F7F7F
    const Result<psd::Document> back = psd::loadFromMemory(b.data(), b.size());
    CHECK(!back.ok());
}

MARI_TEST_MAIN()
