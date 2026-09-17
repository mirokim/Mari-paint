// Mari Paint — 픽셀 스냅샷 해시 검증 (규약은 include/mari/crypto/canvas_hash.hpp 주석)
//
// 여기서 지키려는 것 넷:
//   1. 같은 그림 → 같은 해시 (결정성). 트리를 새로 만들어도 같아야 한다.
//   2. 픽셀 하나만 달라도 → 다른 해시.
//   3. **빈 타일이 일관되게 기여한다** — 할당 여부라는 구현 세부가 해시로 새면 안 된다.
//   4. 도메인 분리 — 캔버스 해시와 레이어 해시가 우연히 같아지지 않는다.
#include <mari/core/compositor.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/tile.hpp>
#include <mari/crypto/canvas_hash.hpp>
#include <mari/test/harness.hpp>

#include <string>
#include <vector>

using namespace mari;
using namespace mari::crypto;

namespace {

/// 타일맵의 사각 영역을 색 하나로 덮는다(합성 아님, 덮어쓰기).
void fillRect(TileMap& map, const Rect& area, Color8 c) {
    for (i32 y = area.y; y < area.bottom(); ++y) {
        for (i32 x = area.x; x < area.right(); ++x) {
            const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
            Result<TilePtr> t = map.writable(tc);
            if (!t.ok()) {
                return;
            }
            const i32 lx = x - tileOrigin(tc.tx);
            const i32 ly = y - tileOrigin(tc.ty);
            u8* px = t.value()->mutablePixels() + static_cast<usize>(ly) * t.value()->stride() +
                     static_cast<usize>(lx) * 4u;
            px[0] = c.r;
            px[1] = c.g;
            px[2] = c.b;
            px[3] = c.a;
        }
    }
}

/// 레이어 한 장짜리 트리를 만든다. 실패하면 nullptr.
LayerTreePtr makeTree(Size sz, LayerPtr* outLayer) {
    Result<LayerTreePtr> tree = makeLayerTree(sz);
    if (!tree.ok()) {
        return nullptr;
    }
    Result<LayerPtr> l = tree.value()->addRaster("칠하기");
    if (!l.ok()) {
        return nullptr;
    }
    if (outLayer != nullptr) {
        *outLayer = l.value();
    }
    return tree.value();
}

std::string hashOf(const LayerTree& t) {
    const Result<std::string> r = canvasHashHex(t);
    return r.ok() ? r.value() : std::string("<오류: " + r.message() + ">");
}

std::string layerHashOf(const Layer& l) {
    const Result<std::string> r = layerHashHex(l);
    return r.ok() ? r.value() : std::string("<오류: " + r.message() + ">");
}

} // namespace

// ── 결정성 ───────────────────────────────────────────────────────────────

MARI_TEST(same_canvas_same_hash) {
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{200, 120}, &a);
    const LayerTreePtr tb = makeTree(Size{200, 120}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    fillRect(*a->tiles(), Rect{10, 10, 50, 40}, Color8::rgba(200, 30, 40));
    fillRect(*b->tiles(), Rect{10, 10, 50, 40}, Color8::rgba(200, 30, 40));

    CHECK_EQ(hashOf(*ta), hashOf(*tb));
    // 같은 트리를 두 번 해시해도 같다(상태가 남지 않는다).
    CHECK_EQ(hashOf(*ta), hashOf(*ta));
    CHECK_EQ(hashOf(*ta).size(), static_cast<usize>(64));
}

MARI_TEST(one_pixel_changes_the_hash) {
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{128, 128}, &a);
    const LayerTreePtr tb = makeTree(Size{128, 128}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    fillRect(*a->tiles(), Rect{0, 0, 128, 128}, Color8::rgba(10, 20, 30));
    fillRect(*b->tiles(), Rect{0, 0, 128, 128}, Color8::rgba(10, 20, 30));
    CHECK_EQ(hashOf(*ta), hashOf(*tb));

    // 딱 한 픽셀. 파란 채널만 1 차이.
    fillRect(*b->tiles(), Rect{77, 99, 1, 1}, Color8::rgba(10, 20, 31));
    CHECK_NE(hashOf(*ta), hashOf(*tb));
}

MARI_TEST(canvas_size_is_inside_the_hash) {
    // 픽셀이 똑같아도 캔버스 크기가 다르면 다른 그림이다.
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{64, 32}, &a);
    const LayerTreePtr tb = makeTree(Size{32, 64}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    CHECK_NE(hashOf(*ta), hashOf(*tb));
}

MARI_TEST(band_streaming_matches_flat_buffer) {
    // 🔴 캔버스 해시는 가로 띠 단위로 스트리밍한다. 띠 경계에서 값이 틀어지면
    //    통짜 버퍼로 계산한 값과 달라진다. 타일 경계(64)를 넘는 높이로 확인한다.
    LayerPtr l;
    const LayerTreePtr t = makeTree(Size{37, 200}, &l);
    CHECK(t != nullptr);
    if (t == nullptr) {
        return;
    }
    fillRect(*l->tiles(), Rect{3, 5, 30, 190}, Color8::rgba(9, 200, 77, 128));

    std::vector<u8> flat(static_cast<usize>(37) * 200u * 4u, static_cast<u8>(0));
    const Result<void> comp =
        compositeArea(*t, Rect{0, 0, 37, 200}, flat.data(), static_cast<usize>(37) * 4u);
    CHECK(comp.ok());
    const Result<std::string> direct = canvasHashHexOfPixels(37, 200, flat.data());
    CHECK(direct.ok());
    if (direct.ok()) {
        CHECK_EQ(hashOf(*t), direct.value());
    }
}

// ── 빈 타일 ──────────────────────────────────────────────────────────────

MARI_TEST(blank_tiles_contribute_consistently_to_canvas_hash) {
    // 🔴 캔버스 해시는 "보이는 그림" 전체를 먹는다. 빈 곳은 (0,0,0,0) 으로 들어간다.
    //    따라서 **칠하지도 않은 타일을 할당했다고 해서 해시가 달라지면 안 된다.**
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{192, 192}, &a);
    const LayerTreePtr tb = makeTree(Size{192, 192}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    fillRect(*a->tiles(), Rect{0, 0, 10, 10}, Color8::rgba(1, 2, 3));
    fillRect(*b->tiles(), Rect{0, 0, 10, 10}, Color8::rgba(1, 2, 3));

    // b 에만 완전 투명 타일을 하나 억지로 할당한다(writable() 은 만들어 준다).
    const Result<TilePtr> forced = b->tiles()->writable(TileCoord{2, 2});
    CHECK(forced.ok());
    CHECK(b->tiles()->tileCount() > a->tiles()->tileCount());

    CHECK_EQ(hashOf(*ta), hashOf(*tb));
}

MARI_TEST(blank_tiles_do_not_contribute_to_layer_hash) {
    // 레이어 해시는 기여 타일만 싣는다. 빈 타일은 아예 안 실린다 —
    // 그래서 할당했다고 값이 바뀌지 않는다.
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{256, 256}, &a);
    const LayerTreePtr tb = makeTree(Size{256, 256}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    fillRect(*a->tiles(), Rect{70, 70, 5, 5}, Color8::rgba(255, 0, 0));
    fillRect(*b->tiles(), Rect{70, 70, 5, 5}, Color8::rgba(255, 0, 0));
    const Result<TilePtr> forced = b->tiles()->writable(TileCoord{3, 3});
    CHECK(forced.ok());

    CHECK_EQ(layerHashOf(*a), layerHashOf(*b));
}

MARI_TEST(empty_layer_has_a_stable_hash) {
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{64, 64}, &a);
    const LayerTreePtr tb = makeTree(Size{4096, 4096}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    // 레이어 픽셀 해시는 캔버스 크기를 모른다 — 빈 레이어는 어디서나 같은 값이다.
    CHECK_EQ(layerHashOf(*a), layerHashOf(*b));
    CHECK_EQ(layerHashOf(*a).size(), static_cast<usize>(64));
}

MARI_TEST(transparent_rgb_residue_is_normalized) {
    // 🔴 알파 0 정규화: 보이지 않는 RGB 찌꺼기는 해시를 흔들지 않는다.
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{64, 64}, &a);
    const LayerTreePtr tb = makeTree(Size{64, 64}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    fillRect(*a->tiles(), Rect{0, 0, 8, 8}, Color8::rgba(0, 0, 0, 0));
    fillRect(*b->tiles(), Rect{0, 0, 8, 8}, Color8::rgba(200, 100, 50, 0)); // 안 보이는 색

    CHECK_EQ(layerHashOf(*a), layerHashOf(*b));
    CHECK_EQ(hashOf(*ta), hashOf(*tb));
}

// ── 레이어 해시의 성질 ───────────────────────────────────────────────────

MARI_TEST(layer_hash_is_pixels_only) {
    // 이름·불투명도·블렌드 모드는 픽셀 해시에 들어가지 않는다(규약 명시).
    LayerPtr l;
    const LayerTreePtr t = makeTree(Size{64, 64}, &l);
    CHECK(t != nullptr);
    if (t == nullptr) {
        return;
    }
    fillRect(*l->tiles(), Rect{1, 1, 20, 20}, Color8::rgba(7, 8, 9));
    const std::string before = layerHashOf(*l);

    l->setName("다른 이름");
    l->setOpacity(0.25f);
    l->setBlendMode(BlendMode::Multiply);
    l->setVisible(false);
    CHECK_EQ(layerHashOf(*l), before);

    // 반대로 픽셀이 바뀌면 값이 바뀐다.
    fillRect(*l->tiles(), Rect{1, 1, 1, 1}, Color8::rgba(7, 8, 10));
    CHECK_NE(layerHashOf(*l), before);
}

MARI_TEST(layer_hash_covers_tiles_outside_the_canvas) {
    // 레이어는 캔버스 밖으로 나갈 수 있다. 레이어 픽셀 해시는 그것도 센다.
    // (캔버스 해시는 반대로 안 센다 — 아래 테스트가 그걸 본다.)
    LayerPtr l;
    const LayerTreePtr t = makeTree(Size{64, 64}, &l);
    CHECK(t != nullptr);
    if (t == nullptr) {
        return;
    }
    const std::string empty = layerHashOf(*l);
    const std::string canvasBefore = hashOf(*t);

    fillRect(*l->tiles(), Rect{200, 200, 4, 4}, Color8::rgba(1, 1, 1)); // 캔버스 밖
    CHECK_NE(layerHashOf(*l), empty);
    CHECK_EQ(hashOf(*t), canvasBefore); // 캔버스 해시는 보이는 것만 본다
}

MARI_TEST(tile_order_is_canonical) {
    // 칠한 순서가 달라도 같은 그림이면 같은 해시다(정렬 규약).
    LayerPtr a;
    LayerPtr b;
    const LayerTreePtr ta = makeTree(Size{256, 256}, &a);
    const LayerTreePtr tb = makeTree(Size{256, 256}, &b);
    CHECK(ta != nullptr && tb != nullptr);
    if (ta == nullptr || tb == nullptr) {
        return;
    }
    fillRect(*a->tiles(), Rect{200, 10, 4, 4}, Color8::rgba(1, 2, 3));
    fillRect(*a->tiles(), Rect{10, 200, 4, 4}, Color8::rgba(4, 5, 6));
    // b 는 반대 순서로 칠한다.
    fillRect(*b->tiles(), Rect{10, 200, 4, 4}, Color8::rgba(4, 5, 6));
    fillRect(*b->tiles(), Rect{200, 10, 4, 4}, Color8::rgba(1, 2, 3));

    CHECK_EQ(layerHashOf(*a), layerHashOf(*b));
}

// ── 도메인 분리 ──────────────────────────────────────────────────────────

MARI_TEST(canvas_and_layer_hashes_live_in_different_domains) {
    LayerPtr l;
    const LayerTreePtr t = makeTree(Size{0, 0}, &l);
    CHECK(t != nullptr);
    if (t == nullptr) {
        return;
    }
    // 둘 다 "아무 픽셀도 없음"이지만 도메인 접두사가 달라 값이 같을 수 없다.
    CHECK_NE(hashOf(*t), layerHashOf(*l));
}

MARI_TEST(mask_format_is_refused_not_guessed) {
    // Gray8 마스크를 RGBA8 인 척 해시하지 않는다. 정직하게 거절한다.
    Result<TileMapPtr> gray = makeTileMap(PixelFormat::Gray8);
    CHECK(gray.ok());
    if (!gray.ok()) {
        return;
    }
    const Result<std::string> r = tileMapHashHex(*gray.value());
    CHECK(!r.ok());
    if (!r.ok()) {
        CHECK(r.code() == ErrorCode::Unsupported);
    }
}

MARI_TEST_MAIN()
