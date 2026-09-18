// Mari Paint — 실행취소가 붙은 레이어 구조 연산 검증 (app/layer_commands)
//
//   · 평탄화: 루트가 하나가 되고 합성 결과가 같다. 실행취소 한 번에 원래 트리로 돌아온다.
//   · 레이어 마스크: 선택에서 만들기 → 합성에 반영 → 적용(알파에 굽기) → 실행취소.
#include <mari/app/document.hpp>
#include <mari/app/layer_commands.hpp>
#include <mari/core/selection.hpp>
#include <mari/test/harness.hpp>

#include <vector>

using namespace mari;
using namespace mari::app;

namespace {

std::unique_ptr<Document> makeDoc() {
    Result<std::unique_ptr<Document>> d = Document::create(Size{128, 96});
    return d.ok() ? std::move(d).value() : nullptr;
}

/// 레이어 하나를 통째로 한 색으로 칠한다.
void fill(Document& doc, LayerId id, Color8 c) {
    const Size cs = doc.canvasSize();
    std::vector<u8> px(static_cast<usize>(cs.width) * static_cast<usize>(cs.height) * 4u);
    for (usize i = 0; i < px.size(); i += 4) {
        px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; px[i + 3] = c.a;
    }
    (void)doc.paintPixels(id, Rect{0, 0, cs.width, cs.height}, px.data(), px.size(), "칠");
}

std::vector<u8> flat(Document& doc) {
    const Size cs = doc.canvasSize();
    std::vector<u8> px(static_cast<usize>(cs.width) * static_cast<usize>(cs.height) * 4u);
    (void)doc.layers().flatten(Rect{0, 0, cs.width, cs.height}, px.data(), static_cast<usize>(cs.width) * 4u);
    return px;
}

u8 alphaAt(const std::vector<u8>& px, i32 w, i32 x, i32 y) {
    return px[(static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4u + 3u];
}

} // namespace

MARI_TEST(flatten_all_collapses_tree_and_undoes) {
    auto doc = makeDoc();
    CHECK(doc != nullptr);
    const LayerId base = doc->layers().activeLayer();
    fill(*doc, base, Color8::rgba(255, 0, 0));
    const Result<LayerPtr> g = doc->layers().addGroup("g");
    CHECK(g.ok());
    const Result<LayerPtr> top = doc->layers().addRaster("top", g.value()->id());
    CHECK(top.ok());
    fill(*doc, top.value()->id(), Color8::rgba(0, 0, 255, 128));
    CHECK_EQ(doc->layers().roots().size(), usize{2});
    const std::vector<u8> before = flat(*doc);

    const Result<LayerId> made = flattenAll(*doc);
    CHECK(made.ok());
    CHECK_EQ(doc->layers().roots().size(), usize{1});
    CHECK_EQ(doc->layers().roots()[0]->id(), made.value());
    CHECK_EQ(doc->layers().activeLayer(), made.value());
    CHECK(flat(*doc) == before);

    CHECK(doc->undo().ok());
    CHECK_EQ(doc->layers().roots().size(), usize{2});
    CHECK(doc->layers().find(top.value()->id()) != nullptr);
    CHECK(doc->layers().find(g.value()->id()) != nullptr);
    CHECK(flat(*doc) == before);

    CHECK(doc->redo().ok());
    CHECK_EQ(doc->layers().roots().size(), usize{1});
    CHECK(flat(*doc) == before);
}

MARI_TEST(layer_mask_from_selection_apply_and_undo) {
    auto doc = makeDoc();
    CHECK(doc != nullptr);
    const LayerId id = doc->layers().activeLayer();
    fill(*doc, id, Color8::rgba(0, 255, 0));
    Result<SelectionMask> sel = SelectionMask::fromRect(doc->canvasSize(), Rect{0, 0, 64, 96});
    CHECK(sel.ok());
    doc->setSelectionMask(std::move(sel).value());

    // 선택에서 마스크: 왼쪽 반만 보인다.
    CHECK(addLayerMask(*doc, id, true).ok());
    CHECK(doc->layers().find(id)->mask() != nullptr);
    std::vector<u8> px = flat(*doc);
    CHECK_EQ(alphaAt(px, 128, 10, 10), u8{255});
    CHECK_EQ(alphaAt(px, 128, 100, 10), u8{0});

    // 선택을 반전해 "가리기" 로 칠하면 왼쪽도 가려진다... 대신 오른쪽을 "보이기" 로 칠해 본다.
    Result<SelectionMask> right = SelectionMask::fromRect(doc->canvasSize(), Rect{64, 0, 64, 96});
    CHECK(right.ok());
    doc->setSelectionMask(std::move(right).value());
    CHECK(paintMaskWithSelection(*doc, id, 255).ok());
    px = flat(*doc);
    CHECK_EQ(alphaAt(px, 128, 100, 10), u8{255});
    CHECK(doc->undo().ok());
    px = flat(*doc);
    CHECK_EQ(alphaAt(px, 128, 100, 10), u8{0});

    // 적용: 마스크가 알파에 구워지고 마스크는 사라진다. 실행취소하면 둘 다 돌아온다.
    CHECK(applyLayerMask(*doc, id).ok());
    CHECK(doc->layers().find(id)->mask() == nullptr);
    px = flat(*doc);
    CHECK_EQ(alphaAt(px, 128, 100, 10), u8{0});
    CHECK_EQ(alphaAt(px, 128, 10, 10), u8{255});
    CHECK(doc->undo().ok());
    CHECK(doc->layers().find(id)->mask() != nullptr);
    CHECK(doc->undo().ok()); // 마스크 추가 취소
    CHECK(doc->layers().find(id)->mask() == nullptr);
    px = flat(*doc);
    CHECK_EQ(alphaAt(px, 128, 100, 10), u8{255});

    // 삭제
    CHECK(addLayerMask(*doc, id, false).ok());
    CHECK(removeLayerMask(*doc, id).ok());
    CHECK(doc->layers().find(id)->mask() == nullptr);
    CHECK(doc->undo().ok());
    CHECK(doc->layers().find(id)->mask() != nullptr);
}

MARI_TEST_MAIN()
