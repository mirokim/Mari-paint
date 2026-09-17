// Mari Paint — Windows Ink(WM_POINTER) 펜 필드 정규화
//
// 🔴 WinTab 은 구현하지 않는다(docs/03 3절). 이 헤더에도, 이 모듈 어디에도
//    `wintab32` 라는 문자열조차 넣지 않는다. CI 가 임포트 테이블을 검사한다
//    (scripts/ci/check-no-wintab.ps1).
//
// 이 헤더는 **Win32 를 포함하지 않는다.** POINTER_PEN_INFO 의 값만 그대로 옮긴
// 평범한 구조체를 받아 장치 독립 값으로 접는다. 덕분에 Linux 에서도
// 컴파일·테스트된다(tests/win/test_pen_normalize.cpp).
//
// 값 범위는 Microsoft 공개 문서 기준이다:
//   · pressure : 0..1024  (미지원이면 0)
//   · rotation : 0..359   (배럴 회전)
//   · tiltX/Y  : -90..90  (도)
#ifndef MARI_WIN_INPUT_PEN_NORMALIZE_HPP
#define MARI_WIN_INPUT_PEN_NORMALIZE_HPP

#include <mari/core/types.hpp>
#include <mari/stroke/input.hpp>

#include <cmath>

namespace mari::win {

/// 장치가 실제로 채워 준 필드(POINTER_PEN_INFO::penMask 와 값이 같다).
/// 비트를 직접 비교하므로 **뒤에만 더해라.**
enum PenMaskBits : u32 {
    kPenMaskNone = 0x00000000u,
    kPenMaskPressure = 0x00000001u, ///< PEN_MASK_PRESSURE
    kPenMaskRotation = 0x00000002u, ///< PEN_MASK_ROTATION
    kPenMaskTiltX = 0x00000004u,    ///< PEN_MASK_TILT_X
    kPenMaskTiltY = 0x00000008u,    ///< PEN_MASK_TILT_Y
};

/// 펜 상태 플래그(POINTER_PEN_INFO::penFlags 와 값이 같다).
enum PenFlagBits : u32 {
    kPenFlagNone = 0x00000000u,
    kPenFlagBarrel = 0x00000001u,   ///< PEN_FLAG_BARREL — 배럴 버튼
    kPenFlagInverted = 0x00000002u, ///< PEN_FLAG_INVERTED — 펜을 뒤집었다
    kPenFlagEraser = 0x00000004u,   ///< PEN_FLAG_ERASER — 지우개 촉이 닿았다
};

/// 장치 최대 필압. 문서상 WM_POINTER 는 0..1024 로 접어서 준다.
inline constexpr f32 kPointerPressureMax = 1024.0f;

/// POINTER_PEN_INFO 를 Win32 타입 없이 옮겨 담은 것. 값·의미는 1:1 이다.
struct PenRawFields {
    u32 penMask = kPenMaskNone;
    u32 penFlags = kPenFlagNone;
    u32 pressure = 0; ///< 0..1024
    u32 rotation = 0; ///< 0..359 (도)
    i32 tiltX = 0;    ///< -90..90 (도)
    i32 tiltY = 0;    ///< -90..90 (도)
};

/// 값을 범위 안으로 자른다.
template <class T> [[nodiscard]] constexpr T clampTo(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// 필압을 0..1 로 접는다. 장치가 필압을 안 주면(마스크 없음 또는 0) 1.0 으로 본다.
///
/// ⚠️ pressure==0 은 "미지원"과 "정말 0" 을 구분할 수 없다. 마우스·미지원 펜에서
///    선이 아예 안 나오는 것보다 1.0 이 낫다 — 포토샵·CSP 도 같은 선택을 한다.
[[nodiscard]] inline f32 normalizePressure(const PenRawFields& p) noexcept {
    if ((p.penMask & kPenMaskPressure) == 0u || p.pressure == 0u) {
        return 1.0f;
    }
    const f32 v = static_cast<f32>(p.pressure) / kPointerPressureMax;
    return clampTo(v, 0.0f, 1.0f);
}

/// 기울기를 도 단위로 꺼낸다(-90..90). 마스크가 없으면 0.
[[nodiscard]] inline f32 normalizeTiltDeg(i32 rawTilt, u32 penMask, u32 maskBit) noexcept {
    if ((penMask & maskBit) == 0u) {
        return 0.0f;
    }
    return clampTo(static_cast<f32>(rawTilt), -90.0f, 90.0f);
}

/// 배럴 회전(0..360). 마스크가 없으면 0.
[[nodiscard]] inline f32 normalizeRotationDeg(const PenRawFields& p) noexcept {
    if ((p.penMask & kPenMaskRotation) == 0u) {
        return 0.0f;
    }
    return static_cast<f32>(p.rotation % 360u);
}

/// tiltX/tiltY → 방위각(0..360). **WM_POINTER 는 방위각을 직접 주지 않는다.**
/// W3C Pointer Events 의 tilt→azimuth 변환을 그대로 쓴다:
///     azimuth = atan2(tan(tiltY), tan(tiltX))
/// 0° = 캔버스 +x 방향, 각도는 화면상 반시계 방향으로 증가한다.
/// 기울기가 둘 다 0(펜이 수직)이면 방위각이 정의되지 않으므로 0 을 준다.
[[nodiscard]] inline f32 azimuthFromTiltDeg(f32 tiltXDeg, f32 tiltYDeg) noexcept {
    if (tiltXDeg == 0.0f && tiltYDeg == 0.0f) {
        return 0.0f;
    }
    constexpr f64 kDeg2Rad = 3.14159265358979323846 / 180.0;
    const f64 tx = std::tan(static_cast<f64>(tiltXDeg) * kDeg2Rad);
    const f64 ty = std::tan(static_cast<f64>(tiltYDeg) * kDeg2Rad);
    f64 deg = std::atan2(ty, tx) / kDeg2Rad;
    if (deg < 0.0) {
        deg += 360.0;
    }
    return static_cast<f32>(deg);
}

/// 지우개 촉인가. 뒤집힌 펜(inverted)도 지우개로 본다 — 와콤 펜 뒤쪽이 그렇다.
[[nodiscard]] inline bool isEraserTip(const PenRawFields& p) noexcept {
    return (p.penFlags & (kPenFlagEraser | kPenFlagInverted)) != 0u;
}

/// 펜 필드를 스트로크 파이프라인의 `RawInputEvent` 로 접는다.
///
/// 좌표는 여기서 채우지 않는다 — 호출자가 ViewTransform::toCanvasD() 로 만든
/// **캔버스 좌표**를 넣는다. 화면 좌표를 넣으면 안 된다.
///
/// 핫 패스. noexcept, 할당 없음.
[[nodiscard]] inline stroke::RawInputEvent makeRawEvent(f64 canvasX, f64 canvasY,
                                                        const PenRawFields& p,
                                                        u64 timestampNs) noexcept {
    stroke::RawInputEvent e;
    e.x = canvasX;
    e.y = canvasY;
    e.hasPressure = (p.penMask & kPenMaskPressure) != 0u;
    e.pressure = normalizePressure(p);
    e.pressureMax = 1.0f; ///< 이미 0..1 로 접어서 넣는다
    e.hasTilt = (p.penMask & (kPenMaskTiltX | kPenMaskTiltY)) != 0u;
    e.tiltXDeg = normalizeTiltDeg(p.tiltX, p.penMask, kPenMaskTiltX);
    e.tiltYDeg = normalizeTiltDeg(p.tiltY, p.penMask, kPenMaskTiltY);
    e.azimuthDeg = azimuthFromTiltDeg(e.tiltXDeg, e.tiltYDeg);
    e.rotationDeg = normalizeRotationDeg(p);
    e.timestampNs = timestampNs;
    return e;
}

/// QueryPerformanceCounter 틱 → ns. **단조 시계다**(docs/03 5.6).
/// POINTER_INFO::PerformanceCount 가 이 틱으로 들어온다 — 벽시계(dwTime)를 쓰지 마라.
/// freq 가 0 이면 0 을 돌려준다(호출자가 monotonicNowNs() 로 대체한다).
[[nodiscard]] inline u64 qpcToNs(u64 ticks, u64 freq) noexcept {
    if (freq == 0u) {
        return 0u;
    }
    // 오버플로 없이: 초 단위와 나머지를 나눠서 곱한다.
    const u64 whole = ticks / freq;
    const u64 rem = ticks % freq;
    return whole * 1000000000ull + (rem * 1000000000ull) / freq;
}

/// HIMETRIC 서브픽셀 좌표 → 물리 화면 px(배정밀도).
///
/// GetPointerDeviceRects() 가 주는 두 사각형으로 선형 사상한다.
///   · `devLeft/devTop/devRight/devBottom`     = pointerDeviceRect (디지타이저, HIMETRIC)
///   · `dispLeft/dispTop/dispRight/dispBottom` = displayRect       (화면, px)
///
/// 이걸 쓰는 이유: ptPixelLocation 은 정수라 고해상도 태블릿의 서브픽셀 정밀도를
/// 버린다. 저줌에서는 티가 안 나지만 **800% 확대에서는 선이 계단처럼 보인다.**
[[nodiscard]] inline bool himetricToScreen(i32 hx, i32 hy, i32 devLeft, i32 devTop, i32 devRight,
                                           i32 devBottom, i32 dispLeft, i32 dispTop, i32 dispRight,
                                           i32 dispBottom, f64& outX, f64& outY) noexcept {
    const f64 devW = static_cast<f64>(devRight) - static_cast<f64>(devLeft);
    const f64 devH = static_cast<f64>(devBottom) - static_cast<f64>(devTop);
    if (devW <= 0.0 || devH <= 0.0) {
        return false;
    }
    const f64 dispW = static_cast<f64>(dispRight) - static_cast<f64>(dispLeft);
    const f64 dispH = static_cast<f64>(dispBottom) - static_cast<f64>(dispTop);
    outX = static_cast<f64>(dispLeft) + (static_cast<f64>(hx) - static_cast<f64>(devLeft)) * dispW / devW;
    outY = static_cast<f64>(dispTop) + (static_cast<f64>(hy) - static_cast<f64>(devTop)) * dispH / devH;
    return true;
}

} // namespace mari::win

#endif // MARI_WIN_INPUT_PEN_NORMALIZE_HPP
