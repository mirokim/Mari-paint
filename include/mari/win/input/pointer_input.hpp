// Mari Paint — Windows Ink 입력 (WM_POINTER)
//
// ════════════════════════════════════════════════════════════════════════════
// 🔴 WinTab 은 구현하지 않는다. 타협 없다. (docs/03 3절)
//
//    Sigan 의 `Core/Foreground.cs` 에 `WinTabKeywords` 목록이 있다. 거기 올라간 앱은
//    "RawInput 이 침묵하니 프록시 DLL 을 주입해야 하는 앱" 이다. CSP·포토샵·**Krita**
//    가 올라가 있다. Krita 는 오픈소스인데도 `wintab32.dll` 을 로드한 채로 돈다.
//
//    Mari 의 목표: **`WinTabKeywords` 에 영원히 올라가지 않는 최초의 드로잉 앱.**
//    그래서 이 모듈에는 `wintab32.dll` 을 로드하는 코드가 한 줄도 없다.
//    LoadLibrary 로도, 지연 로딩으로도, 어떤 우회로도 없다.
//    CI 가 빌드 산출물의 임포트 테이블을 검사한다(scripts/ci/check-no-wintab.ps1).
//
//    이게 값싼 이유: Windows Ink 만 쓰면 Sigan 의 RawInput 경로가 Mari 사용 중에도
//    살아 있다 → **연동 코드 한 줄 없이도 Sigan 이 오늘 당장 Mari 를 기록한다**
//    (docs/03 9절 S0). 나머지 연동은 전부 품질 향상일 뿐이다.
// ════════════════════════════════════════════════════════════════════════════
//
// 좌표 규약:
//   WM_POINTER 는 **물리 화면 좌표**를 준다. 우리는 그걸
//     ① 서브픽셀 HIMETRIC → 화면 px  (GetPointerDeviceRects)
//     ② ScreenToClient                 → 클라이언트 px
//     ③ ViewTransform::toCanvasD()     → **캔버스 좌표**
//   순서로 푼다. 파이프라인 아래로는 캔버스 좌표만 흐른다.
//
// ⚠️ 창은 **Per-Monitor-V2 DPI 인식**이어야 한다. 그래야 클라이언트 좌표가
//    물리 픽셀과 1:1 이고 ② 단계에서 DPI 배율을 따로 곱할 필요가 없다.
//    매니페스트에 `<dpiAwareness>PerMonitorV2</dpiAwareness>` 를 넣어라.
//    안 넣으면 고DPI 화면에서 펜이 커서와 어긋난다.
//
// ⚠️ Windows 전용.
#ifndef MARI_WIN_INPUT_POINTER_INPUT_HPP
#define MARI_WIN_INPUT_POINTER_INPUT_HPP

#if !defined(_WIN32)
#error "mari/win/input/pointer_input.hpp 는 Windows 전용이다"
#endif

#include <mari/core/result.hpp>
#include <mari/stroke/input.hpp>
#include <mari/win/input/pen_normalize.hpp>
#include <mari/win/input/view_transform.hpp>

#include <windows.h>

namespace mari::win {

/// 입력 장치 종류. 마우스도 같은 경로로 흐른다(EnableMouseInPointer).
enum class PointerKind : u8 {
    Unknown = 0,
    Pen,
    Touch,
    Mouse,
};

/// 정규화까지 끝난 입력 한 건. 여기부터는 장치를 모른다.
struct PointerSample {
    stroke::RawInputEvent event{}; ///< 캔버스 좌표 · 0..1 필압 · 단조 ns
    PenRawFields pen{};            ///< 원본 펜 필드(진단·리포트용)
    PointerKind kind = PointerKind::Unknown;
    bool eraser = false;  ///< 지우개 촉 또는 뒤집힌 펜
    bool barrel = false;  ///< 배럴 버튼
    bool fromHistory = false; ///< 이력에서 꺼낸 점인가(아래 설명 참조)
    u32 pointerId = 0;
};

/// 입력을 받아 갈 쪽. 보통 StrokePipeline 을 감싼 어댑터가 구현한다.
///
/// 🔴 스레드 규약: 이 콜백은 **메시지 펌프 스레드**에서 불린다.
///    여기서 무거운 일을 하면 입력이 밀린다. 파이프라인에 넘기고 즉시 돌아와라
///    (docs/02 4절: [1]~[4]는 UI 스레드 밖에서 돈다).
class IPointerTarget {
public:
    virtual ~IPointerTarget() = default;
    virtual void onPointerDown(const PointerSample& s) = 0;
    virtual void onPointerMove(const PointerSample& s) = 0;
    virtual void onPointerUp(const PointerSample& s) = 0;
    /// 포커스·캡처를 잃었다. 획을 안전하게 끝내라.
    virtual void onPointerCancel(u32 pointerId) = 0;
};

/// 통계. 회귀를 잡는 용도다 — "점을 잃었나?" 는 눈으로 못 본다.
struct PointerStats {
    u64 down = 0;
    u64 move = 0;
    u64 up = 0;
    u64 cancel = 0;
    /// 🔴 이력에서 건진 점 수. **이게 0 이면 이력 API 가 안 먹고 있다는 뜻이고,
    ///    펜 보고율(200~300Hz)과 메시지율(60~120Hz)의 차이만큼 점을 버리고 있다.**
    u64 historyRecovered = 0;
    /// 이력 API 가 실패한 횟수.
    u64 historyFailures = 0;
    /// HIMETRIC 서브픽셀 경로를 못 써서 정수 좌표로 떨어진 횟수.
    u64 himetricFallbacks = 0;
    /// 펜이 아닌 포인터(터치)를 무시한 횟수.
    u64 ignoredTouch = 0;
};

/// WM_POINTER 핸들러.
///
/// 쓰는 법:
///   1. 창을 만든 뒤 `attach(hwnd)`.
///   2. 뷰가 바뀔 때마다 `setView(viewTransform)`.
///   3. WndProc 에서 `handleMessage()` 를 먼저 부르고, handled 면 그 값을 돌려준다.
class PointerInput {
public:
    PointerInput() noexcept = default;
    ~PointerInput();

    PointerInput(const PointerInput&) = delete;
    PointerInput& operator=(const PointerInput&) = delete;

    /// 창에 붙는다. target 은 이 객체보다 오래 살아야 한다.
    ///
    /// 여기서 하는 것:
    ///   · `EnableMouseInPointer(TRUE)` — 마우스도 같은 경로로 흐르게 한다.
    ///     경로가 하나면 "마우스에서만 나는 버그"가 안 생긴다.
    ///   · 시각 피드백(펜 터치 시 퍼지는 동그라미)을 끈다. 그리는 데 방해된다.
    ///   · QueryPerformanceFrequency 를 한 번 읽어 둔다(단조 시계, docs/03 5.6).
    [[nodiscard]] Result<void> attach(HWND hwnd, IPointerTarget* target);
    void detach() noexcept;

    /// 뷰가 바뀌었다. 다음 입력부터 이 변환으로 캔버스 좌표를 만든다.
    /// 🔴 획 **중간**에 바꾸지 마라 — 같은 획이 두 좌표계에 걸친다.
    void setView(const ViewTransform& v) noexcept { view_ = v; }
    [[nodiscard]] const ViewTransform& view() const noexcept { return view_; }

    /// 터치를 그리기 입력으로 쓸 것인가. 기본은 **끔** — 손바닥으로 그려지면 안 된다.
    void setAcceptTouch(bool v) noexcept { acceptTouch_ = v; }
    /// 마우스를 그리기 입력으로 쓸 것인가. 기본은 켬(필압 1.0).
    void setAcceptMouse(bool v) noexcept { acceptMouse_ = v; }

    /// WndProc 에서 부른다. 우리가 처리했으면 `handled = true`.
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                          bool& handled) noexcept;

    [[nodiscard]] const PointerStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = PointerStats{}; }

    /// 🔴 항상 "windows-ink". 진단 화면에 그대로 띄운다(docs/02 10절 위험 대응).
    [[nodiscard]] static const char* inputApiName() noexcept { return "windows-ink"; }

private:
    /// 포인터 하나의 이벤트를 처리한다. down/move/up 공통.
    void dispatch(HWND hwnd, u32 pointerId, int phase) noexcept;
    /// 포인터 정보 한 건 → PointerSample. 실패하면 false.
    bool makeSample(HWND hwnd, const POINTER_INFO& pi, const PenRawFields& pen, bool fromHistory,
                    PointerSample& out) noexcept;
    /// 화면 좌표(서브픽셀 우선) → 클라이언트 좌표.
    bool screenToClient(HWND hwnd, const POINTER_INFO& pi, f64& cx, f64& cy) noexcept;

    HWND hwnd_ = nullptr;
    IPointerTarget* target_ = nullptr;
    ViewTransform view_{};
    PointerStats stats_{};
    u64 qpcFreq_ = 0;
    u32 activeId_ = 0;   ///< 지금 획을 그리고 있는 포인터. 0 이면 없다
    bool acceptTouch_ = false;
    bool acceptMouse_ = true;
    bool enabledMouseInPointer_ = false;

    /// 이력 버퍼. 🔴 **begin 때 한 번 잡고 핫 패스에서 절대 재할당하지 않는다.**
    /// 펜 한 프레임에 이력이 512개를 넘는 장치는 없다(보고율 300Hz × 60Hz 프레임 ≈ 5).
    /// 넉넉히 잡아 두고, 넘치면 넘치는 만큼만 버리고 통계에 남긴다.
    static constexpr u32 kHistoryCap = 512;
    POINTER_PEN_INFO penHistory_[kHistoryCap]{};
};

} // namespace mari::win

#endif // MARI_WIN_INPUT_POINTER_INPUT_HPP
