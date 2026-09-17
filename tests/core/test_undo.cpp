// 실행취소 — 변경된 타일만 보관하는 스냅샷, 그리고 되돌리기/다시하기 왕복.
#include <mari/core/layer.hpp>
#include <mari/core/undo.hpp>
#include <mari/test/harness.hpp>

#include <string>

using namespace mari;

namespace {

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

Color8 peek(const TileMap& map, i32 x, i32 y) {
    const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
    ConstTilePtr t = map.at(tc);
    if (!t)
        return Color8{0, 0, 0, 0};
    const u8* p = t->pixels() + static_cast<usize>(y - tileOrigin(tc.ty)) * t->stride() +
                  static_cast<usize>(x - tileOrigin(tc.tx)) * 4u;
    return Color8{p[0], p[1], p[2], p[3]};
}

/// 스트로크 흉내: 더티 타일을 before 로 잡고, 칠하고, after 로 잡는다.
UndoCommandPtr strokeOnce(TileMap& map, std::string name, i32 x, i32 y, Color8 c) {
    auto cmd = TileSnapshotCommand::begin(std::move(name), &map);
    DirtyTiles dirty{TileCoord{tileIndexFor(x), tileIndexFor(y)}};
    cmd->captureBefore(dirty);
    dot(map, x, y, c);
    cmd->captureAfter(dirty);
    return cmd;
}

} // namespace

MARI_TEST(begin_requires_a_target) {
    CHECK(TileSnapshotCommand::begin("x", nullptr) == nullptr);
}

// ★ 변경된 타일만 보관한다 — 4000×4000 캔버스라도 스트로크 하나는 타일 몇 개다.
MARI_TEST(snapshot_keeps_only_changed_tiles) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    for (i32 i = 0; i < 10; ++i)
        dot(*map, i * 64, 0, Color8{1, 1, 1, 255}); // 타일 10개를 미리 채워 둔다
    CHECK_EQ(map->tileCount(), 10u);

    auto cmd = TileSnapshotCommand::begin("브러시", map.get());
    DirtyTiles dirty{TileCoord{3, 0}};
    cmd->captureBefore(dirty);
    cmd->captureBefore(dirty); // 중복 호출해도 한 번만 담는다
    dot(*map, 3 * 64 + 1, 1, Color8{9, 9, 9, 255});
    cmd->captureAfter(dirty);

    CHECK_EQ(cmd->changedTileCount(), 1u);
    DirtyTiles touched;
    cmd->affectedTiles(touched);
    CHECK_EQ(touched.size(), 1u);
    CHECK(touched[0] == (TileCoord{3, 0}));

    // 아무것도 안 바뀌면 비어 있다고 말한다 — 스택에 넣을 가치가 없다.
    auto noop = TileSnapshotCommand::begin("빈 스트로크", map.get());
    noop->captureBefore(dirty);
    noop->captureAfter(dirty);
    CHECK(noop->empty());
}

MARI_TEST(undo_redo_roundtrip_restores_pixels) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    dot(*map, 10, 10, Color8{10, 10, 10, 255});

    UndoStack stack;
    stack.push(strokeOnce(*map, "1획", 10, 10, Color8{99, 0, 0, 255}));
    CHECK(peek(*map, 10, 10) == (Color8{99, 0, 0, 255}));
    CHECK(stack.canUndo());
    CHECK(!stack.canRedo());
    CHECK_EQ(stack.undoText(), std::string("1획"));

    DirtyTiles dirty;
    CHECK(stack.undo(&dirty).ok());
    CHECK(peek(*map, 10, 10) == (Color8{10, 10, 10, 255})); // 원래대로
    CHECK_EQ(dirty.size(), 1u);
    CHECK(stack.canRedo());
    CHECK_EQ(stack.redoText(), std::string("1획"));

    CHECK(stack.redo().ok());
    CHECK(peek(*map, 10, 10) == (Color8{99, 0, 0, 255})); // 다시 칠해진다
    CHECK(!stack.canRedo());

    // 여러 번 왕복해도 흔들리지 않는다.
    for (int i = 0; i < 3; ++i) {
        CHECK(stack.undo().ok());
        CHECK(peek(*map, 10, 10) == (Color8{10, 10, 10, 255}));
        CHECK(stack.redo().ok());
        CHECK(peek(*map, 10, 10) == (Color8{99, 0, 0, 255}));
    }
}

MARI_TEST(undo_removes_tiles_that_did_not_exist) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    CHECK_EQ(map->tileCount(), 0u);

    UndoStack stack;
    stack.push(strokeOnce(*map, "첫 점", 500, 500, Color8{1, 2, 3, 255}));
    CHECK_EQ(map->tileCount(), 1u);

    CHECK(stack.undo().ok());
    CHECK_EQ(map->tileCount(), 0u); // 없던 타일은 메모리째 되돌린다
    CHECK(stack.redo().ok());
    CHECK_EQ(map->tileCount(), 1u);
    CHECK(peek(*map, 500, 500) == (Color8{1, 2, 3, 255}));
}

MARI_TEST(multi_step_undo_unwinds_in_order) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    UndoStack stack;
    stack.push(strokeOnce(*map, "a", 0, 0, Color8{1, 0, 0, 255}));
    stack.push(strokeOnce(*map, "b", 0, 0, Color8{2, 0, 0, 255}));
    stack.push(strokeOnce(*map, "c", 0, 0, Color8{3, 0, 0, 255}));
    CHECK_EQ(stack.undoCount(), 3u);

    CHECK(stack.undo().ok());
    CHECK_EQ(static_cast<int>(peek(*map, 0, 0).r), 2);
    CHECK(stack.undo().ok());
    CHECK_EQ(static_cast<int>(peek(*map, 0, 0).r), 1);
    CHECK(stack.undo().ok());
    CHECK_EQ(static_cast<int>(peek(*map, 0, 0).a), 0); // 첫 획 이전 = 빈 타일
    CHECK(!stack.canUndo());
    CHECK(!stack.undo().ok());
    CHECK_EQ(stack.redoCount(), 3u);

    CHECK(stack.redo().ok());
    CHECK(stack.redo().ok());
    CHECK(stack.redo().ok());
    CHECK_EQ(static_cast<int>(peek(*map, 0, 0).r), 3);
    CHECK(!stack.redo().ok());
}

MARI_TEST(new_command_drops_the_redo_branch) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    UndoStack stack;
    stack.push(strokeOnce(*map, "a", 0, 0, Color8{1, 0, 0, 255}));
    stack.push(strokeOnce(*map, "b", 0, 0, Color8{2, 0, 0, 255}));
    CHECK(stack.undo().ok());
    CHECK_EQ(stack.redoCount(), 1u);

    stack.push(strokeOnce(*map, "c", 0, 0, Color8{3, 0, 0, 255}));
    CHECK_EQ(stack.redoCount(), 0u); // 새 가지가 났으니 b 는 버린다
    CHECK(!stack.canRedo());
    CHECK_EQ(stack.undoText(), std::string("c"));
}

MARI_TEST(stack_honours_its_limit) {
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    UndoStack stack(2);
    CHECK_EQ(stack.limit(), 2u);
    stack.push(strokeOnce(*map, "a", 0, 0, Color8{1, 0, 0, 255}));
    stack.push(strokeOnce(*map, "b", 0, 0, Color8{2, 0, 0, 255}));
    stack.push(strokeOnce(*map, "c", 0, 0, Color8{3, 0, 0, 255}));
    CHECK_EQ(stack.undoCount(), 2u); // 가장 오래된 a 가 밀려났다
    CHECK(stack.undo().ok());
    CHECK(stack.undo().ok());
    CHECK(!stack.canUndo());
    CHECK_EQ(static_cast<int>(peek(*map, 0, 0).r), 1); // a 직후 상태까지만 돌아간다

    stack.setLimit(1);
    CHECK_EQ(stack.limit(), 1u);
    stack.clear();
    CHECK(!stack.canUndo());
    CHECK(!stack.canRedo());

    UndoStack zero(0);
    CHECK_EQ(zero.limit(), 1u); // 0은 1로 올린다
}

MARI_TEST(undo_does_not_disturb_other_layers_sharing_tiles) {
    // 복제된 레이어(COW 공유)에서 한쪽을 되돌려도 다른 쪽은 그대로여야 한다.
    auto map = makeTileMap(PixelFormat::RGBA8).value();
    dot(*map, 5, 5, Color8{50, 50, 50, 255});
    auto shared = map->snapshot();

    UndoStack stack;
    stack.push(strokeOnce(*map, "획", 5, 5, Color8{200, 0, 0, 255}));
    CHECK(peek(*shared, 5, 5) == (Color8{50, 50, 50, 255}));

    CHECK(stack.undo().ok());
    CHECK(peek(*map, 5, 5) == (Color8{50, 50, 50, 255}));
    CHECK(peek(*shared, 5, 5) == (Color8{50, 50, 50, 255}));

    CHECK(stack.redo().ok());
    CHECK(peek(*map, 5, 5) == (Color8{200, 0, 0, 255}));
    CHECK(peek(*shared, 5, 5) == (Color8{50, 50, 50, 255})); // 끝까지 안 샌다
}

MARI_TEST(undo_works_through_a_layer) {
    auto tree = makeLayerTree(Size{256, 256}).value();
    auto layer = tree->addRaster("l").value();
    UndoStack stack;
    stack.push(strokeOnce(*layer->tiles(), "레이어 획", 100, 100, Color8{7, 8, 9, 255}));
    CHECK(peek(*layer->tiles(), 100, 100) == (Color8{7, 8, 9, 255}));
    CHECK(stack.undo().ok());
    CHECK_EQ(layer->tiles()->tileCount(), 0u);
    CHECK_EQ(layer->bounds(), (Rect{}));
}

MARI_TEST_MAIN()
