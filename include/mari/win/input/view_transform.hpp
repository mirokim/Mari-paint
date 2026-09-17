// Mari Paint — 뷰 변환: 화면 좌표 ↔ 캔버스 좌표 (docs/03 4.1 의 핵심)
//
// 🔴 왜 이 파일이 중요한가
//    Sigan 은 지금 화면 좌표밖에 모른다. 작가가 캔버스를 회전·줌하면 같은 획이
//    다른 궤적으로 남는다. 그래서 Vase9.ViewProbe 로 화면을 뜯어보고 있다.
//    Mari 가 **캔버스 좌표**를 주면 그 문제 자체가 사라진다(docs/03 4.1).
//    → 그러려면 화면 좌표를 캔버스 좌표로 되돌리는 역변환이 정확해야 한다.
//       이 파일이 그 역변환이다.
//
// 좌표계 두 개를 절대 섞지 않는다(기반 계층 규약):
//   · **캔버스 좌표** — 좌상단 원점, y 아래로 증가, px. 줌·회전·팬·미러와 무관한 불변값.
//   · **화면(클라이언트) 좌표** — 창 클라이언트 영역 기준 px. DPI·줌·회전·팬에 따라 변한다.
//
// 이 헤더는 **Win32 를 포함하지 않는다.** 순수 수학이라 Linux 에서도 컴파일·테스트된다.
// (tests/win/test_view_transform.cpp 가 실제로 그렇게 돌고 있다.)
#ifndef MARI_WIN_INPUT_VIEW_TRANSFORM_HPP
#define MARI_WIN_INPUT_VIEW_TRANSFORM_HPP

#include <mari/core/types.hpp>

#include <cmath>

namespace mari::win {

/// 2×3 아핀 변환. 행 우선.
///     x' = a*x + b*y + tx
///     y' = c*x + d*y + ty
struct Affine2 {
    f64 a = 1.0, b = 0.0, tx = 0.0;
    f64 c = 0.0, d = 1.0, ty = 0.0;

    /// 행렬식. 0 이면 역행렬이 없다.
    [[nodiscard]] f64 det() const noexcept { return a * d - b * c; }

    [[nodiscard]] PointF apply(f64 x, f64 y) const noexcept {
        return PointF{static_cast<f32>(a * x + b * y + tx), static_cast<f32>(c * x + d * y + ty)};
    }

    /// 배정밀도로 그대로 돌려준다. 역변환 왕복 오차를 재는 테스트가 쓴다.
    void applyD(f64 x, f64 y, f64& outX, f64& outY) const noexcept {
        outX = a * x + b * y + tx;
        outY = c * x + d * y + ty;
    }

    /// 역행렬. det 가 0 이면 항등을 돌려준다(호출자가 invertible() 로 먼저 걸러라).
    [[nodiscard]] Affine2 inverse() const noexcept {
        const f64 dt = det();
        if (dt == 0.0) {
            return Affine2{};
        }
        const f64 inv = 1.0 / dt;
        Affine2 m;
        m.a = d * inv;
        m.b = -b * inv;
        m.c = -c * inv;
        m.d = a * inv;
        m.tx = (b * ty - d * tx) * inv;
        m.ty = (c * tx - a * ty) * inv;
        return m;
    }

    [[nodiscard]] bool invertible() const noexcept {
        const f64 dt = det();
        return std::isfinite(dt) && dt != 0.0;
    }
};

/// 뷰 상태 하나. 캔버스를 화면에 어떻게 얹고 있는가.
///
/// 적용 순서(캔버스 → 화면):
///   1. 캔버스 좌표에서 `anchorCanvas` 를 뺀다  (회전·확대의 중심)
///   2. 미러(mirrorX) → 회전(rotationDeg, 시계방향 양수) → 확대(zoom)
///   3. `anchorScreen` 을 더한다                (그 중심이 화면 어디에 있나)
///
/// `zoom` 은 "캔버스 1px 이 화면 몇 px 인가"다. **DPI 배율을 여기 섞지 마라.**
/// DPI 는 PointerInput 이 화면 좌표를 만들 때 이미 풀었다고 본다.
struct ViewState {
    f64 zoom = 1.0;          ///< > 0. 1.0 = 100%
    f64 rotationDeg = 0.0;   ///< 캔버스를 시계방향으로 돌린 각도
    bool mirrorX = false;    ///< 좌우 반전(미러 보기)
    PointF anchorCanvas{};   ///< 회전·확대의 중심(캔버스 좌표)
    PointF anchorScreen{};   ///< 그 중심이 놓인 화면 좌표

    friend bool operator==(const ViewState&, const ViewState&) = default;
};

/// 뷰 변환. `ViewState` 에서 행렬 두 개를 미리 구워 둔다.
///
/// 🔴 핫 패스 규칙: toCanvas() 는 펜 이벤트마다 불린다. 할당·삼각함수 없이
///    미리 구운 역행렬만 곱한다. 상태가 바뀌면 setState() 로 다시 굽는다.
class ViewTransform {
public:
    ViewTransform() noexcept { rebuild(); }
    explicit ViewTransform(const ViewState& s) noexcept : state_(s) { rebuild(); }

    void setState(const ViewState& s) noexcept {
        state_ = s;
        rebuild();
    }
    [[nodiscard]] const ViewState& state() const noexcept { return state_; }

    /// 캔버스 → 화면.
    [[nodiscard]] const Affine2& canvasToScreen() const noexcept { return fwd_; }
    /// 화면 → 캔버스. **펜 입력이 쓰는 쪽이다.**
    [[nodiscard]] const Affine2& screenToCanvas() const noexcept { return inv_; }

    /// 캔버스 좌표 → 화면 좌표.
    [[nodiscard]] PointF toScreen(PointF canvas) const noexcept {
        return fwd_.apply(canvas.x, canvas.y);
    }

    /// 화면 좌표 → **캔버스 좌표**. 핫 패스. noexcept, 할당 없음.
    [[nodiscard]] PointF toCanvas(PointF screen) const noexcept {
        return inv_.apply(screen.x, screen.y);
    }

    /// 배정밀도 버전. WM_POINTER 의 HIMETRIC 서브픽셀 좌표를 잃지 않으려면 이쪽을 쓴다.
    void toCanvasD(f64 sx, f64 sy, f64& cx, f64& cy) const noexcept { inv_.applyD(sx, sy, cx, cy); }

    /// 화면 거리 → 캔버스 거리(회전과 무관한 등방 배율). 브러시 커서 크기 계산용.
    [[nodiscard]] f64 screenLengthToCanvas(f64 px) const noexcept {
        return state_.zoom > 0.0 ? px / state_.zoom : px;
    }

    /// 뷰가 성립하는가(zoom > 0, 각도가 유한, 역행렬 존재).
    [[nodiscard]] bool valid() const noexcept {
        return state_.zoom > 0.0 && std::isfinite(state_.zoom) &&
               std::isfinite(state_.rotationDeg) && fwd_.invertible();
    }

private:
    void rebuild() noexcept {
        const f64 rad = state_.rotationDeg * 3.14159265358979323846 / 180.0;
        const f64 cs = std::cos(rad);
        const f64 sn = std::sin(rad);
        const f64 z = state_.zoom;
        const f64 mx = state_.mirrorX ? -1.0 : 1.0;

        // 미러 → 회전 → 확대 를 한 행렬로 접는다.
        //   R * M = [ cs*mx  -sn ]
        //           [ sn*mx   cs ]
        fwd_.a = z * cs * mx;
        fwd_.b = -z * sn;
        fwd_.c = z * sn * mx;
        fwd_.d = z * cs;

        // 앵커: fwd(anchorCanvas) == anchorScreen 이 되도록 평행이동을 잡는다.
        const f64 ax = static_cast<f64>(state_.anchorCanvas.x);
        const f64 ay = static_cast<f64>(state_.anchorCanvas.y);
        fwd_.tx = static_cast<f64>(state_.anchorScreen.x) - (fwd_.a * ax + fwd_.b * ay);
        fwd_.ty = static_cast<f64>(state_.anchorScreen.y) - (fwd_.c * ax + fwd_.d * ay);

        inv_ = fwd_.inverse();
    }

    ViewState state_{};
    Affine2 fwd_{};
    Affine2 inv_{};
};

/// 뷰 변환 보고용 스냅샷(docs/03 8절 `ViewTransform` 필드 · COM `OnViewChanged`).
/// Sigan 이 이걸로 ①Mari 캔버스 좌표 ↔ ②RawInput 화면 좌표를 상호 변환한다(docs/03 7절).
struct ViewReport {
    f64 zoom = 1.0;
    f64 rotationDeg = 0.0;
    bool mirrorX = false;
    f64 a = 1.0, b = 0.0, tx = 0.0; ///< canvasToScreen 행렬을 그대로 싣는다
    f64 c = 0.0, d = 1.0, ty = 0.0;
};

/// 현재 뷰를 보고용 스냅샷으로 만든다.
[[nodiscard]] inline ViewReport makeViewReport(const ViewTransform& v) noexcept {
    const Affine2& m = v.canvasToScreen();
    ViewReport r;
    r.zoom = v.state().zoom;
    r.rotationDeg = v.state().rotationDeg;
    r.mirrorX = v.state().mirrorX;
    r.a = m.a;
    r.b = m.b;
    r.tx = m.tx;
    r.c = m.c;
    r.d = m.d;
    r.ty = m.ty;
    return r;
}

} // namespace mari::win

#endif // MARI_WIN_INPUT_VIEW_TRANSFORM_HPP
