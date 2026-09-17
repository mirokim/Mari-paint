// Mari Paint — Windows Ink 펜 필드 정규화 테스트
//
// WM_POINTER 가 주는 장치 값(필압 0..1024, 기울기 -90..90, 회전 0..359)을
// 스트로크 파이프라인이 쓰는 장치 독립 값으로 접는 부분을 검증한다.
//
// 🔴 이 파일에도, 이 모듈 어디에도 WinTab 은 없다(docs/03 3절).
#include <mari/test/harness.hpp>
#include <mari/win/input/pen_normalize.hpp>

using namespace mari;
using namespace mari::win;

MARI_TEST(pressure_folds_to_unit_range) {
    PenRawFields p;
    p.penMask = kPenMaskPressure;

    p.pressure = 1024;
    CHECK_NEAR(normalizePressure(p), 1.0f, 1e-6f);

    p.pressure = 512;
    CHECK_NEAR(normalizePressure(p), 0.5f, 1e-6f);

    // 장치가 범위를 넘겨 줘도 우리는 안 넘는다. 남의 드라이버를 믿지 않는다.
    p.pressure = 5000;
    CHECK_NEAR(normalizePressure(p), 1.0f, 1e-6f);
}

MARI_TEST(missing_pressure_becomes_full) {
    // 마우스·필압 미지원 펜. 선이 아예 안 나오는 것보다 1.0 이 낫다.
    PenRawFields p;
    p.penMask = kPenMaskNone;
    p.pressure = 0;
    CHECK_NEAR(normalizePressure(p), 1.0f, 1e-6f);

    // 마스크는 있는데 값이 0 인 경우도 같게 다룬다(구분할 방법이 없다).
    p.penMask = kPenMaskPressure;
    CHECK_NEAR(normalizePressure(p), 1.0f, 1e-6f);
}

MARI_TEST(tilt_respects_mask) {
    PenRawFields p;
    p.tiltX = 45;
    p.tiltY = -30;

    // 마스크가 없으면 장치가 안 준 것이다. 쓰레기 값을 읽지 않는다.
    p.penMask = kPenMaskNone;
    CHECK_NEAR(normalizeTiltDeg(p.tiltX, p.penMask, kPenMaskTiltX), 0.0f, 1e-6f);

    p.penMask = kPenMaskTiltX | kPenMaskTiltY;
    CHECK_NEAR(normalizeTiltDeg(p.tiltX, p.penMask, kPenMaskTiltX), 45.0f, 1e-6f);
    CHECK_NEAR(normalizeTiltDeg(p.tiltY, p.penMask, kPenMaskTiltY), -30.0f, 1e-6f);

    // 범위 밖은 자른다.
    CHECK_NEAR(normalizeTiltDeg(200, p.penMask, kPenMaskTiltX), 90.0f, 1e-6f);
    CHECK_NEAR(normalizeTiltDeg(-200, p.penMask, kPenMaskTiltX), -90.0f, 1e-6f);
}

MARI_TEST(rotation_wraps) {
    PenRawFields p;
    p.penMask = kPenMaskRotation;
    p.rotation = 359;
    CHECK_NEAR(normalizeRotationDeg(p), 359.0f, 1e-6f);
    p.rotation = 360;
    CHECK_NEAR(normalizeRotationDeg(p), 0.0f, 1e-6f);
    p.rotation = 725;
    CHECK_NEAR(normalizeRotationDeg(p), 5.0f, 1e-6f);
}

MARI_TEST(azimuth_from_tilt) {
    // 펜이 +x 로만 기울면 방위각 0°.
    CHECK_NEAR(azimuthFromTiltDeg(30.0f, 0.0f), 0.0f, 1e-3f);
    // +y 로만 기울면 90°.
    CHECK_NEAR(azimuthFromTiltDeg(0.0f, 30.0f), 90.0f, 1e-3f);
    // -x 로만 기울면 180°.
    CHECK_NEAR(azimuthFromTiltDeg(-30.0f, 0.0f), 180.0f, 1e-3f);
    // -y 로만 기울면 270° (음수로 안 떨어진다).
    CHECK_NEAR(azimuthFromTiltDeg(0.0f, -30.0f), 270.0f, 1e-3f);
    // 대각선은 45°.
    CHECK_NEAR(azimuthFromTiltDeg(30.0f, 30.0f), 45.0f, 1e-3f);
    // 수직으로 세운 펜은 방위각이 정의되지 않는다 — 0 으로 떨어지고 NaN 이 아니다.
    const f32 a = azimuthFromTiltDeg(0.0f, 0.0f);
    CHECK(a == a); // NaN 이면 실패
    CHECK_NEAR(a, 0.0f, 1e-6f);
}

MARI_TEST(eraser_tip_detected) {
    PenRawFields p;
    CHECK(!isEraserTip(p));
    p.penFlags = kPenFlagEraser;
    CHECK(isEraserTip(p));
    // 뒤집은 펜도 지우개로 본다. 와콤 펜 뒤쪽이 그렇게 들어온다.
    p.penFlags = kPenFlagInverted;
    CHECK(isEraserTip(p));
    p.penFlags = kPenFlagBarrel;
    CHECK(!isEraserTip(p));
}

MARI_TEST(raw_event_carries_canvas_coords) {
    PenRawFields p;
    p.penMask = kPenMaskPressure | kPenMaskTiltX | kPenMaskTiltY;
    p.pressure = 256;
    p.tiltX = 10;
    p.tiltY = 10;

    const stroke::RawInputEvent e = makeRawEvent(1234.5, -77.25, p, 999u);
    CHECK_NEAR(e.x, 1234.5, 1e-9);
    CHECK_NEAR(e.y, -77.25, 1e-9);
    CHECK_NEAR(e.pressure, 0.25f, 1e-6f);
    // 이미 0..1 로 접었으므로 pressureMax 는 1 이어야 한다. 아니면 두 번 나눠진다.
    CHECK_NEAR(e.pressureMax, 1.0f, 1e-6f);
    CHECK(e.hasPressure);
    CHECK(e.hasTilt);
    CHECK_EQ(e.timestampNs, 999u);
    CHECK_NEAR(e.azimuthDeg, 45.0f, 1e-3f);
}

MARI_TEST(qpc_to_ns_is_exact_and_overflow_safe) {
    // 10MHz 는 흔한 QPC 주파수다.
    CHECK_EQ(qpcToNs(10000000ull, 10000000ull), 1000000000ull);
    CHECK_EQ(qpcToNs(1ull, 10000000ull), 100ull);
    CHECK_EQ(qpcToNs(0ull, 10000000ull), 0ull);
    // freq 0 = 알 수 없음. 나눗셈으로 죽지 않는다.
    CHECK_EQ(qpcToNs(12345ull, 0ull), 0ull);

    // 기계가 100일 켜져 있어도 오버플로하지 않는다.
    // (틱을 통째로 1e9 배 하면 u64 가 넘친다 — 그래서 몫/나머지로 나눠 곱한다.)
    const u64 freq = 10000000ull;
    const u64 days100 = 100ull * 24ull * 3600ull * freq;
    CHECK_EQ(qpcToNs(days100, freq), 100ull * 24ull * 3600ull * 1000000000ull);
}

MARI_TEST(himetric_maps_into_display_rect) {
    // 디지타이저 0..30000 HIMETRIC 이 화면 0..1920 에 대응한다고 하자.
    f64 x = 0.0, y = 0.0;
    CHECK(himetricToScreen(15000, 10000, 0, 0, 30000, 20000, 0, 0, 1920, 1080, x, y));
    CHECK_NEAR(x, 960.0, 1e-6);
    CHECK_NEAR(y, 540.0, 1e-6);

    // 서브픽셀이 살아 있어야 한다 — 이게 이 경로를 쓰는 이유다.
    CHECK(himetricToScreen(1, 1, 0, 0, 30000, 20000, 0, 0, 1920, 1080, x, y));
    CHECK(x > 0.0 && x < 1.0);

    // 망가진 장치 사각형은 거절한다. 0 으로 나누지 않는다.
    CHECK(!himetricToScreen(1, 1, 0, 0, 0, 0, 0, 0, 1920, 1080, x, y));
}

MARI_TEST_MAIN()
