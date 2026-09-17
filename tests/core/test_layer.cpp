// 레이어 트리 — 추가/삭제/이동/복제. 복제는 COW 라서 픽셀을 복사하지 않는다.
#include <mari/core/layer.hpp>
#include <mari/core/layer_ops.hpp>
#include <mari/test/harness.hpp>

using namespace mari;

namespace {

void dot(Layer& layer, i32 x, i32 y, Color8 c) {
    TileMap* tm = layer.tiles();
    const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
    auto w = tm->writable(tc);
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

Color8 peek(const Layer& layer, i32 x, i32 y) {
    const TileMap* tm = layer.tiles();
    const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
    ConstTilePtr t = tm->at(tc);
    if (!t)
        return Color8{0, 0, 0, 0};
    const u8* p = t->pixels() + static_cast<usize>(y - tileOrigin(tc.ty)) * t->stride() +
                  static_cast<usize>(x - tileOrigin(tc.tx)) * 4u;
    return Color8{p[0], p[1], p[2], p[3]};
}

} // namespace

MARI_TEST(new_tree_is_empty_and_sized) {
    auto tree = makeLayerTree(Size{1920, 1080}).value();
    CHECK_EQ(tree->canvasSize(), (Size{1920, 1080}));
    CHECK_EQ(tree->roots().size(), 0u);
    CHECK_EQ(tree->activeLayer(), kInvalidLayerId);
    CHECK(!makeLayerTree(Size{-1, 10}).ok());
}

MARI_TEST(index_zero_is_the_bottom_layer) {
    auto tree = makeLayerTree(Size{100, 100}).value();
    auto a = tree->addRaster("아래").value();
    auto b = tree->addRaster("위").value(); // index 기본 -1 = 맨 위

    CHECK_EQ(tree->roots().size(), 2u);
    CHECK_EQ(tree->roots()[0]->name(), std::string("아래"));
    CHECK_EQ(tree->roots()[1]->name(), std::string("위"));

    // index 0 으로 넣으면 가장 아래로 들어간다.
    auto c = tree->addRaster("맨아래", kInvalidLayerId, 0).value();
    CHECK_EQ(tree->roots()[0]->id(), c->id());
    CHECK_EQ(tree->roots()[2]->id(), b->id());
    CHECK_EQ(tree->activeLayer(), a->id()); // 첫 래스터가 자동으로 활성
}

MARI_TEST(groups_nest_and_report_bounds) {
    auto tree = makeLayerTree(Size{4000, 4000}).value();
    auto group = tree->addGroup("그룹").value();
    auto inner = tree->addRaster("속", group->id()).value();

    CHECK_EQ(tree->roots().size(), 1u);
    CHECK_EQ(group->children().size(), 1u);
    CHECK(group->kind() == LayerKind::Group);
    CHECK(group->tiles() == nullptr); // 그룹에는 픽셀 저장소가 없다
    CHECK_EQ(group->bounds(), (Rect{}));

    dot(*inner, 200, 300, Color8{1, 2, 3, 255});
    CHECK_EQ(inner->bounds(), (Rect{192, 256, 64, 64}));
    CHECK_EQ(group->bounds(), inner->bounds()); // 그룹은 자식의 합집합

    // 그룹이 아닌 레이어 밑에는 못 넣는다.
    CHECK(!tree->addRaster("안됨", inner->id()).ok());
    CHECK(!tree->addRaster("없는부모", 9999).ok());
}

MARI_TEST(remove_drops_the_subtree) {
    auto tree = makeLayerTree(Size{100, 100}).value();
    auto group = tree->addGroup("그룹").value();
    auto inner = tree->addRaster("속", group->id()).value();
    const LayerId innerId = inner->id();

    CHECK(tree->find(innerId) != nullptr);
    CHECK(tree->remove(group->id()).ok());
    CHECK(tree->find(group->id()) == nullptr);
    CHECK(tree->find(innerId) == nullptr); // 자식도 같이 사라진다
    CHECK_EQ(tree->roots().size(), 0u);
    CHECK(!tree->remove(group->id()).ok()); // 두 번은 안 된다
}

MARI_TEST(move_reparents_and_rejects_cycles) {
    auto tree = makeLayerTree(Size{100, 100}).value();
    auto g1 = tree->addGroup("g1").value();
    auto g2 = tree->addGroup("g2", g1->id()).value();
    auto leaf = tree->addRaster("leaf").value();

    CHECK(tree->move(leaf->id(), g2->id(), -1).ok());
    CHECK_EQ(g2->children().size(), 1u);
    CHECK_EQ(tree->roots().size(), 1u);

    // 자기 자손 아래로는 못 간다 — 순환 금지
    CHECK(!tree->move(g1->id(), g2->id(), -1).ok());
    CHECK(!tree->move(g1->id(), g1->id(), -1).ok());
    // 그룹이 아닌 곳으로도 못 간다
    CHECK(!tree->move(g2->id(), leaf->id(), -1).ok());
    // 다시 루트로
    CHECK(tree->move(leaf->id(), kInvalidLayerId, 0).ok());
    CHECK_EQ(tree->roots()[0]->id(), leaf->id());
}

// ★ COW 정확성: 복제 후 원본을 고쳐도 복제본은 그대로다.
MARI_TEST(duplicate_is_cow_and_original_edit_does_not_leak) {
    auto tree = makeLayerTree(Size{4000, 4000}).value();
    auto src = tree->addRaster("원본").value();
    src->setOpacity(0.5f);
    src->setBlendMode(BlendMode::Multiply);
    dot(*src, 100, 100, Color8{10, 20, 30, 255});
    dot(*src, 3000, 3000, Color8{40, 50, 60, 255});
    CHECK_EQ(src->tiles()->tileCount(), 2u);

    auto dupR = duplicateLayer(*tree, src->id());
    CHECK(dupR.ok());
    LayerPtr dup = dupR.value();

    // 속성이 따라온다. 이름은 구분된다.
    CHECK_NEAR(dup->opacity(), 0.5f, 1e-6);
    CHECK(dup->blendMode() == BlendMode::Multiply);
    CHECK_EQ(dup->name(), std::string("원본 복사"));
    CHECK_NE(dup->id(), src->id());
    // 원본 바로 위에 들어간다.
    CHECK_EQ(tree->roots().size(), 2u);
    CHECK_EQ(tree->roots()[1]->id(), dup->id());

    // 픽셀은 같지만 저장소는 별개다.
    CHECK_EQ(dup->tiles()->tileCount(), 2u);
    CHECK(peek(*dup, 100, 100) == (Color8{10, 20, 30, 255}));
    CHECK_NE(static_cast<const void*>(dup->tiles()), static_cast<const void*>(src->tiles()));

    // 원본을 고친다 → 복제본은 불변이어야 한다.
    dot(*src, 100, 100, Color8{255, 255, 255, 255});
    CHECK(peek(*src, 100, 100) == (Color8{255, 255, 255, 255}));
    CHECK(peek(*dup, 100, 100) == (Color8{10, 20, 30, 255}));

    // 복제본을 고쳐도 원본은 그대로다.
    dot(*dup, 3000, 3000, Color8{7, 7, 7, 255});
    CHECK(peek(*src, 3000, 3000) == (Color8{40, 50, 60, 255}));

    // 새 타일 추가가 서로에게 새지 않는다.
    dot(*dup, 0, 0, Color8{1, 1, 1, 255});
    CHECK_EQ(dup->tiles()->tileCount(), 3u);
    CHECK_EQ(src->tiles()->tileCount(), 2u);
}

MARI_TEST(duplicate_copies_groups_recursively) {
    auto tree = makeLayerTree(Size{100, 100}).value();
    auto group = tree->addGroup("그룹").value();
    auto inner = tree->addRaster("속", group->id()).value();
    dot(*inner, 1, 1, Color8{9, 9, 9, 255});

    auto dup = duplicateLayer(*tree, group->id(), "복제그룹").value();
    CHECK_EQ(dup->name(), std::string("복제그룹"));
    CHECK_EQ(dup->children().size(), 1u);
    CHECK_NE(dup->children()[0]->id(), inner->id());
    CHECK(peek(*dup->children()[0], 1, 1) == (Color8{9, 9, 9, 255}));
    // 복제본의 자식도 트리 색인에 들어간다.
    CHECK(tree->find(dup->children()[0]->id()) != nullptr);

    // 원본 자식을 고쳐도 복제본 자식은 그대로다.
    dot(*inner, 1, 1, Color8{0, 0, 0, 255});
    CHECK(peek(*dup->children()[0], 1, 1) == (Color8{9, 9, 9, 255}));

    CHECK(!duplicateLayer(*tree, 9999).ok());
}

MARI_TEST(set_layer_tiles_swaps_storage) {
    auto tree = makeLayerTree(Size{100, 100}).value();
    auto layer = tree->addRaster("l").value();
    auto group = tree->addGroup("g").value();

    auto fresh = makeTileMap(PixelFormat::RGBA8).value();
    CHECK(setLayerTiles(*layer, fresh).ok());
    CHECK_EQ(static_cast<const void*>(layer->tiles()), static_cast<const void*>(fresh.get()));
    CHECK(!setLayerTiles(*group, fresh).ok()); // 그룹에는 못 넣는다
    CHECK(!setLayerTiles(*layer, nullptr).ok());
}

MARI_TEST(active_layer_follows_removal) {
    auto tree = makeLayerTree(Size{10, 10}).value();
    auto a = tree->addRaster("a").value();
    auto b = tree->addRaster("b").value();
    CHECK(tree->setActiveLayer(b->id()).ok());
    CHECK_EQ(tree->activeLayer(), b->id());
    CHECK(!tree->setActiveLayer(9999).ok());
    CHECK(tree->remove(b->id()).ok());
    CHECK_EQ(tree->activeLayer(), kInvalidLayerId);
    CHECK(tree->setActiveLayer(a->id()).ok());
}

MARI_TEST_MAIN()
