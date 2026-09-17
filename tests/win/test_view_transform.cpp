// Mari Paint — 뷰 변환 테스트
//
// 🔴 docs/03 10절의 `canvas_xy_invariant` 를 여기서 증명한다:
//    "줌·회전·팬 후 같은 획 → 캔버스 좌표 동일"
//
// Windows 전용 레이어의 코드지만 이 부분은 Win32 를 안 쓰는 순수 수학이라
// Linux CI 에서도 그대로 돈다. 그게 이 파일이 존재하는 이유다 —
// 입력 경로에서 제일 틀리기 쉬운 부분을 Windows 없이 검증한다.
#include <mari/test/harness.hpp>
#include <mari/win/input/view_transform.hpp>

using namespace mari;
using namespace mari::win;

namespace {

/// 캔버스 좌표 → 화면 → 다시 캔버스. 원래 값으로 돌아와야 한다.
void roundTrip(mari::test::Context& mari_ctx, const ViewState& st, f64 cx, f64 cy, const char* what) {
    ViewTransform v(st);
    if (!v.valid()) {
        mari_ctx.fail(__FILE__, __LINE__, std::string("뷰가 성립하지 않는다: ") + what);
        return;
    }
    f64 sx = 0.0, sy = 0.0;
    v.canvasToScreen().applyD(cx, cy, sx, sy);
    f64 bx = 0.0, by = 0.0;
    v.toCanvasD(sx, sy, bx, by);
    if (std::abs(bx - cx) > 1e-6 || std::abs(by - cy) > 1e-6) {
        mari_ctx.fail(__FILE__, __LINE__,
                      std::string("왕복이 어긋났다(") + what + "): " + std::to_string(cx) + "," +
                          std::to_string(cy) + " → " + std::to_string(bx) + "," +
                          std::to_string(by));
    }
}

} // namespace

MARI_TEST(identity_view_is_identity) {
    ViewTransform v;
    const PointF p = v.toCanvas(PointF{123.0f, 456.0f});
    CHECK_NEAR(p.x, 123.0f, 1e-4f);
    CHECK_NEAR(p.y, 456.0f, 1e-4f);
    CHECK(v.valid());
}

MARI_TEST(zoom_scales_around_anchor) {
    ViewState st;
    st.zoom = 2.0;
    st.anchorCanvas = PointF{0.0f, 0.0f};
    st.anchorScreen = PointF{0.0f, 0.0f};
    ViewTransform v(st);
    // 캔버스 (10,20) 은 200% 에서 화면 (20,40).
    const PointF s = v.toScreen(PointF{10.0f, 20.0f});
    CHECK_NEAR(s.x, 20.0f, 1e-4f);
    CHECK_NEAR(s.y, 40.0f, 1e-4f);
    // 역방향.
    const PointF c = v.toCanvas(PointF{20.0f, 40.0f});
    CHECK_NEAR(c.x, 10.0f, 1e-4f);
    CHECK_NEAR(c.y, 20.0f, 1e-4f);
}

MARI_TEST(anchor_point_never_moves) {
    // 앵커는 줌·회전이 뭐든 같은 화면 자리에 남아 있어야 한다.
    // 캔버스를 확대하면 커서 밑 픽셀이 도망가는 버그가 정확히 이걸 어겨서 생긴다.
    for (f64 zoom : {0.1, 0.5, 1.0, 3.7, 32.0}) {
        for (f64 rot : {0.0, 17.0, 90.0, 180.0, -45.0}) {
            ViewState st;
            st.zoom = zoom;
            st.rotationDeg = rot;
            st.anchorCanvas = PointF{512.0f, 384.0f};
            st.anchorScreen = PointF{640.0f, 400.0f};
            ViewTransform v(st);
            const PointF s = v.toScreen(st.anchorCanvas);
            CHECK_NEAR(s.x, st.anchorScreen.x, 1e-3f);
            CHECK_NEAR(s.y, st.anchorScreen.y, 1e-3f);
        }
    }
}

MARI_TEST(rotation_90_maps_x_to_y) {
    // 시계방향 90°: 캔버스 +x 는 화면 +y 로 간다(y 가 아래로 증가하는 좌표계).
    ViewState st;
    st.rotationDeg = 90.0;
    ViewTransform v(st);
    const PointF s = v.toScreen(PointF{10.0f, 0.0f});
    CHECK_NEAR(s.x, 0.0f, 1e-3f);
    CHECK_NEAR(s.y, 10.0f, 1e-3f);
}

MARI_TEST(canvas_xy_invariant_under_zoom_rotate_pan) {
    // 🔴 docs/03 10절 canvas_xy_invariant.
    // 같은 캔버스 점을 뷰를 바꿔 가며 화면에 찍고, 그 화면 좌표를 다시 캔버스로
    // 되돌린다. 뷰가 아무리 달라도 캔버스 좌표는 **하나**여야 한다.
    const f64 cx = 137.25, cy = 902.5;

    const ViewState views[] = {
        ViewState{1.0, 0.0, false, PointF{0, 0}, PointF{0, 0}},
        ViewState{4.0, 0.0, false, PointF{100, 100}, PointF{50, 60}},
        ViewState{0.25, 33.0, false, PointF{512, 512}, PointF{640, 360}},
        ViewState{2.5, -120.0, true, PointF{0, 0}, PointF{1920, 1080}},
        ViewState{16.0, 180.0, false, PointF{4000, 4000}, PointF{-200, -150}},
    };

    PointF first{};
    bool haveFirst = false;
    for (const ViewState& st : views) {
        ViewTransform v(st);
        CHECK(v.valid());
        f64 sx = 0.0, sy = 0.0;
        v.canvasToScreen().applyD(cx, cy, sx, sy);
        f64 bx = 0.0, by = 0.0;
        v.toCanvasD(sx, sy, bx, by);
        CHECK_NEAR(bx, cx, 1e-6);
        CHECK_NEAR(by, cy, 1e-6);
        if (!haveFirst) {
            first = PointF{static_cast<f32>(bx), static_cast<f32>(by)};
            haveFirst = true;
        } else {
            // 모든 뷰가 같은 캔버스 좌표를 돌려준다 — 이게 Sigan 에 주는 값이다.
            CHECK_NEAR(static_cast<f64>(first.x), bx, 1e-3);
            CHECK_NEAR(static_cast<f64>(first.y), by, 1e-3);
        }
    }
}

MARI_TEST(mirror_is_invertible) {
    ViewState st;
    st.mirrorX = true;
    st.zoom = 1.5;
    st.rotationDeg = 25.0;
    st.anchorCanvas = PointF{300, 200};
    st.anchorScreen = PointF{10, 20};
    roundTrip(mari_ctx, st, 0.0, 0.0, "mirror+zoom+rot 원점");
    roundTrip(mari_ctx, st, 1234.5, -678.25, "mirror+zoom+rot 임의점");

    // 미러는 행렬식의 부호를 뒤집는다. 뒤집히지 않으면 좌우 반전이 안 걸린 것이다.
    ViewTransform v(st);
    CHECK(v.canvasToScreen().det() < 0.0);
}

MARI_TEST(degenerate_view_is_rejected) {
    ViewState st;
    st.zoom = 0.0; // 줌 0 = 역행렬 없음
    ViewTransform v(st);
    CHECK(!v.valid());
    // 그래도 크래시하지 않고 항등을 돌려준다 — 핫 패스에서 던지지 않는다.
    const PointF p = v.toCanvas(PointF{5, 5});
    CHECK_NEAR(p.x, 5.0f, 1e-4f);
}

MARI_TEST(screen_length_to_canvas) {
    ViewState st;
    st.zoom = 4.0;
    ViewTransform v(st);
    CHECK_NEAR(v.screenLengthToCanvas(40.0), 10.0, 1e-9);
}

MARI_TEST(view_report_carries_full_matrix) {
    // docs/03 7절: Sigan 이 ①캔버스 좌표 ↔ ②RawInput 화면 좌표를 대조하려면
    // 줌·회전만으로는 부족하다. 행렬 전체를 줘야 한다.
    ViewState st;
    st.zoom = 3.0;
    st.rotationDeg = 45.0;
    st.anchorCanvas = PointF{100, 100};
    st.anchorScreen = PointF{700, 400};
    ViewTransform v(st);
    const ViewReport r = makeViewReport(v);

    CHECK_NEAR(r.zoom, 3.0, 1e-9);
    CHECK_NEAR(r.rotationDeg, 45.0, 1e-9);
    CHECK(!r.mirrorX);

    // 리포트 행렬로 직접 계산한 값이 변환기와 같아야 한다.
    const f64 cx = 250.0, cy = -30.0;
    const f64 sx = r.a * cx + r.b * cy + r.tx;
    const f64 sy = r.c * cx + r.d * cy + r.ty;
    f64 vx = 0.0, vy = 0.0;
    v.canvasToScreen().applyD(cx, cy, vx, vy);
    CHECK_NEAR(sx, vx, 1e-9);
    CHECK_NEAR(sy, vy, 1e-9);
}

MARI_TEST_MAIN()
