// Mari Paint — 이미지 연산 검증 (app/image_ops): 색 보정 · 자유 변형 · 캔버스 연산, 전부 실행취소가 정확하다.
#include <mari/app/document.hpp>
#include <mari/app/image_ops.hpp>
#include <mari/core/selection.hpp>
#include <mari/test/harness.hpp>

#include <cstdio>
#include <vector>

using namespace mari;
using namespace mari::app;

namespace {

std::unique_ptr<Document> makeDoc(i32 w = 64, i32 h = 48) {
    Result<std::unique_ptr<Document>> d = Document::create(Size{w, h});
    return d.ok() ? std::move(d).value() : nullptr;
}

void fillRect(Document& doc, LayerId id, Rect r, Color8 c) {
    std::vector<u8> px(static_cast<usize>(r.width) * static_cast<usize>(r.height) * 4u);
    for (usize i = 0; i < px.size(); i += 4) {
        px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; px[i + 3] = c.a;
    }
    (void)doc.paintPixels(id, r, px.data(), px.size(), "칠");
}

Color8 pixel(Document& doc, i32 x, i32 y) {
    std::vector<u8> px(4);
    const Size cs = doc.canvasSize();
    std::vector<u8> all(static_cast<usize>(cs.width) * static_cast<usize>(cs.height) * 4u);
    (void)doc.layers().flatten(Rect{0, 0, cs.width, cs.height}, all.data(), static_cast<usize>(cs.width) * 4u);
    const usize i = (static_cast<usize>(y) * static_cast<usize>(cs.width) + static_cast<usize>(x)) * 4u;
    return Color8{all[i], all[i + 1], all[i + 2], all[i + 3]};
}

const StrokeSource kSrc = StrokeSource::humanPen();

/// 임시 Result 의 value() 참조를 매크로에 넘기면 대상이 먼저 죽는다 — 복사해서 준다.
std::string hashNow(Document& doc) {
    Result<std::string> h = doc.canvasHash();
    return h.ok() ? h.value() : std::string("<hash 실패>");
}

} // namespace

MARI_TEST(adjust_invert_respects_selection_and_undoes) {
    auto doc = makeDoc();
    CHECK(doc != nullptr);
    const LayerId id = doc->layers().activeLayer();
    fillRect(*doc, id, Rect{0, 0, 64, 48}, Color8::rgba(200, 100, 50));
    Result<SelectionMask> sel = SelectionMask::fromRect(doc->canvasSize(), Rect{0, 0, 32, 48});
    CHECK(sel.ok());
    doc->setSelectionMask(std::move(sel).value());
    AdjustParams p;
    p.kind = AdjustKind::Invert;
    const Result<u32> r = adjustLayer(*doc, kSrc, id, p);
    CHECK(r.ok());
    CHECK(r.value() > 0);
    CHECK_EQ(pixel(*doc, 5, 5).r, u8{55});   // 선택 안: 뒤집힘
    CHECK_EQ(pixel(*doc, 50, 5).r, u8{200}); // 선택 밖: 그대로
    CHECK_EQ(pixel(*doc, 5, 5).a, u8{255});  // 알파는 그대로
    CHECK(doc->undo().ok());
    CHECK_EQ(pixel(*doc, 5, 5).r, u8{200});
}

MARI_TEST(adjust_kinds_do_what_they_say) {
    u8 px[8] = {200, 100, 50, 255, 0, 0, 0, 255};
    AdjustParams p;
    p.kind = AdjustKind::Desaturate;
    applyAdjust(p, px, 2);
    CHECK(px[0] == px[1] && px[1] == px[2]);
    CHECK(px[0] > 100 && px[0] < 140); // 0.299·200 + 0.587·100 + 0.114·50 ≈ 124

    u8 q[4] = {100, 100, 100, 255};
    p.kind = AdjustKind::BrightnessContrast;
    p.brightness = 20.0f;
    p.contrast = 0.0f;
    applyAdjust(p, q, 1);
    CHECK_EQ(int(q[0]), 151);

    u8 lv[4] = {128, 128, 128, 255};
    p = AdjustParams{};
    p.kind = AdjustKind::Levels;
    p.inBlack = 128;
    applyAdjust(p, lv, 1);
    CHECK_EQ(int(lv[0]), 0);

    u8 cv[4] = {128, 128, 128, 255};
    p = AdjustParams{};
    p.kind = AdjustKind::Curves;
    p.curve = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 1.0f}};
    applyAdjust(p, cv, 1);
    CHECK(cv[0] >= 254);

    u8 th[4] = {100, 100, 100, 255};
    p = AdjustParams{};
    p.kind = AdjustKind::Threshold;
    p.threshold = 128;
    applyAdjust(p, th, 1);
    CHECK_EQ(int(th[0]), 0);

    u8 hs[4] = {255, 0, 0, 255};
    p = AdjustParams{};
    p.kind = AdjustKind::HueSaturation;
    p.hue = 120.0f;
    applyAdjust(p, hs, 1);
    CHECK(hs[1] > 200 && hs[0] < 40 && hs[2] < 40); // 빨강 → 초록
}

MARI_TEST(transform_moves_scales_and_undoes_exactly) {
    auto doc = makeDoc(64, 64);
    CHECK(doc != nullptr);
    const LayerId id = doc->layers().activeLayer();
    fillRect(*doc, id, Rect{10, 10, 10, 10}, Color8::rgba(0, 0, 255));
    const Result<std::string> before = doc->canvasHash();

    TransformParams t;
    t.dx = 20;
    t.dy = 0;
    const Result<Rect> r = transformLayer(*doc, kSrc, id, t);
    CHECK(r.ok());
    CHECK_EQ(pixel(*doc, 15, 15).a, u8{0});   // 원래 자리는 비었다
    CHECK_EQ(pixel(*doc, 35, 15).a, u8{255}); // 옮겨졌다
    CHECK_EQ(pixel(*doc, 35, 15).b, u8{255});
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), before.value());

    // 2배 확대(중심 기준): 10×10 → 20×20, 중심 (15,15) 유지.
    TransformParams s;
    s.scaleX = s.scaleY = 2.0;
    CHECK(transformLayer(*doc, kSrc, id, s).ok());
    CHECK_EQ(pixel(*doc, 15, 15).a, u8{255});
    CHECK_EQ(pixel(*doc, 7, 7).a, u8{255});  // 5..25 안
    CHECK_EQ(pixel(*doc, 3, 3).a, u8{0});
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), before.value());

    // 90° 회전 + 뒤집기도 실행취소가 정확하다. (정사각형은 돌려도 같아서 "바뀐 것 없음" = 실행취소 항목 없음이 된다 —
    // 그래서 납작한 막대로 검사한다.)
    fillRect(*doc, id, Rect{10, 10, 10, 4}, Color8::rgba(255, 0, 0));
    const std::string bar = hashNow(*doc);
    TransformParams rot;
    rot.rotateDeg = 90.0;
    rot.flipH = true;
    const Result<Rect> rr = transformLayer(*doc, kSrc, id, rot);
    CHECK(rr.ok());
    CHECK_EQ(pixel(*doc, 12, 15).a, u8{255}); // 세로 막대가 됐다(중심 (15,12) 기준 → 13..17 × 7..17)
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), bar);
}

MARI_TEST(transform_with_selection_only_moves_selected_pixels) {
    auto doc = makeDoc(64, 64);
    CHECK(doc != nullptr);
    const LayerId id = doc->layers().activeLayer();
    fillRect(*doc, id, Rect{0, 0, 64, 64}, Color8::rgba(0, 255, 0));
    Result<SelectionMask> sel = SelectionMask::fromRect(doc->canvasSize(), Rect{0, 0, 16, 16});
    CHECK(sel.ok());
    doc->setSelectionMask(std::move(sel).value());
    TransformParams t;
    t.dx = 30;
    t.dy = 30;
    CHECK(transformLayer(*doc, kSrc, id, t).ok());
    CHECK_EQ(pixel(*doc, 5, 5).a, u8{0});     // 잘라 낸 자리
    CHECK_EQ(pixel(*doc, 40, 40).a, u8{255}); // 원래도 초록이었고 위에 초록이 덮였다
    CHECK_EQ(pixel(*doc, 20, 5).a, u8{255});  // 선택 밖은 그대로
}

MARI_TEST(canvas_flip_rotate_resize_crop_scale_with_single_undo) {
    auto doc = makeDoc(40, 20);
    CHECK(doc != nullptr);
    const LayerId id = doc->layers().activeLayer();
    fillRect(*doc, id, Rect{0, 0, 10, 20}, Color8::rgba(255, 0, 0)); // 왼쪽 띠
    const Result<std::string> before = doc->canvasHash();

    CHECK(flipCanvas(*doc, kSrc, true).ok());
    CHECK_EQ(pixel(*doc, 35, 5).r, u8{255});
    CHECK_EQ(pixel(*doc, 5, 5).a, u8{0});
    CHECK_EQ(doc->undoStack().undoCount(), usize{2}); // 칠 1 + 뒤집기 1
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), before.value());

    CHECK(rotateCanvas(*doc, kSrc, 1).ok());
    CHECK(doc->canvasSize() == (Size{20, 40}));
    CHECK_EQ(pixel(*doc, 15, 5).r, u8{255}); // 왼쪽 띠 → 위쪽 띠(시계 90°)
    CHECK(doc->undo().ok());
    CHECK(doc->canvasSize() == (Size{40, 20}));
    CHECK_EQ(hashNow(*doc), before.value());

    CHECK(resizeCanvas(*doc, kSrc, Size{60, 20}, 2, 1).ok()); // 오른쪽 앵커: 내용이 20 만큼 밀린다
    CHECK_EQ(pixel(*doc, 25, 5).r, u8{255});
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), before.value());

    CHECK(cropCanvas(*doc, kSrc, Rect{5, 0, 20, 20}).ok());
    CHECK(doc->canvasSize() == (Size{20, 20}));
    CHECK_EQ(pixel(*doc, 2, 5).r, u8{255});
    CHECK_EQ(pixel(*doc, 10, 5).a, u8{0});
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), before.value());

    CHECK(scaleImage(*doc, kSrc, Size{80, 40}).ok());
    CHECK(doc->canvasSize() == (Size{80, 40}));
    CHECK_EQ(pixel(*doc, 10, 10).r, u8{255});
    CHECK_EQ(pixel(*doc, 60, 10).a, u8{0});
    CHECK(doc->undo().ok());
    CHECK_EQ(hashNow(*doc), before.value());
}

MARI_TEST_MAIN()
