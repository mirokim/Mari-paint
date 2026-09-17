// Mari Paint — Windows Ink(WM_POINTER) 입력 구현
//
// 🔴 이 파일에 `wintab32` 라는 문자열이 없다는 것을 확인해라.
//    LoadLibrary 도, 지연 로딩 임포트도, 그 어떤 우회로도 없다(docs/03 3절).
#include <mari/win/input/pointer_input.hpp>

#include <windowsx.h>

namespace mari::win {
namespace {

/// 이벤트 단계.
enum Phase { kDown = 0, kMove = 1, kUp = 2 };

/// 시각 피드백(펜이 닿을 때 퍼지는 동그라미)을 끈다.
/// 그림 그리는 중에 화면에 동그라미가 뜨면 방해되고, 스냅샷 해시에도 안 좋다.
void disableVisualFeedback(HWND hwnd) noexcept {
    BOOL off = FALSE;
    // 실패해도 치명적이지 않다 — 구버전 Windows 에는 없는 설정이다.
    ::SetWindowFeedbackSetting(hwnd, FEEDBACK_TOUCH_CONTACTVISUALIZATION, 0, sizeof(off), &off);
    ::SetWindowFeedbackSetting(hwnd, FEEDBACK_PEN_TAP, 0, sizeof(off), &off);
    ::SetWindowFeedbackSetting(hwnd, FEEDBACK_PEN_DOUBLETAP, 0, sizeof(off), &off);
    ::SetWindowFeedbackSetting(hwnd, FEEDBACK_PEN_PRESSANDHOLD, 0, sizeof(off), &off);
    ::SetWindowFeedbackSetting(hwnd, FEEDBACK_PEN_RIGHTTAP, 0, sizeof(off), &off);
    ::SetWindowFeedbackSetting(hwnd, FEEDBACK_PEN_BARRELVISUALIZATION, 0, sizeof(off), &off);
}

/// POINTER_PEN_INFO → 우리 구조체.
PenRawFields toPenFields(const POINTER_PEN_INFO& p) noexcept {
    PenRawFields f;
    f.penMask = static_cast<u32>(p.penMask);
    f.penFlags = static_cast<u32>(p.penFlags);
    f.pressure = static_cast<u32>(p.pressure);
    f.rotation = static_cast<u32>(p.rotation);
    f.tiltX = static_cast<i32>(p.tiltX);
    f.tiltY = static_cast<i32>(p.tiltY);
    return f;
}

/// 마우스에는 펜 필드가 없다. 마스크를 비우면 정규화가 필압 1.0 으로 채운다.
PenRawFields mousePenFields() noexcept { return PenRawFields{}; }

PointerKind kindOf(POINTER_INPUT_TYPE t) noexcept {
    switch (t) {
    case PT_PEN:
        return PointerKind::Pen;
    case PT_TOUCH:
        return PointerKind::Touch;
    case PT_MOUSE:
    case PT_TOUCHPAD:
        return PointerKind::Mouse;
    default:
        return PointerKind::Unknown;
    }
}

} // namespace

PointerInput::~PointerInput() { detach(); }

Result<void> PointerInput::attach(HWND hwnd, IPointerTarget* target) {
    if (hwnd == nullptr || target == nullptr) {
        return Err("창 핸들과 입력 대상이 필요하다", ErrorCode::InvalidArgument);
    }
    hwnd_ = hwnd;
    target_ = target;

    // 🔴 단조 시계 주파수. 벽시계를 쓰지 않는다(docs/03 5.6).
    LARGE_INTEGER freq{};
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        qpcFreq_ = static_cast<u64>(freq.QuadPart);
    } else {
        qpcFreq_ = 0; // 0 이면 monotonicNowNs() 로 대체한다
    }

    // 마우스도 포인터 메시지로 받는다. 경로가 하나면 "마우스에서만 나는 버그"가 없다.
    if (::EnableMouseInPointer(TRUE)) {
        enabledMouseInPointer_ = true;
    }
    // ⚠️ 이미 다른 컴포넌트가 껐으면 실패한다. 치명적이지 않다 —
    //    그 경우 마우스는 WM_LBUTTONDOWN 으로 오고, 그건 UI 레이어가 처리한다.

    disableVisualFeedback(hwnd);
    return Ok();
}

void PointerInput::detach() noexcept {
    hwnd_ = nullptr;
    target_ = nullptr;
    activeId_ = 0;
}

bool PointerInput::screenToClient(HWND hwnd, const POINTER_INFO& pi, f64& cx, f64& cy) noexcept {
    // ① 서브픽셀 경로: HIMETRIC 좌표를 장치/화면 사각형으로 사상한다.
    //    이게 없으면 800% 확대에서 선이 계단처럼 보인다.
    RECT devRect{};
    RECT dispRect{};
    bool haveSub = false;
    f64 sx = 0.0, sy = 0.0;

    if (pi.sourceDevice != nullptr &&
        ::GetPointerDeviceRects(pi.sourceDevice, &devRect, &dispRect)) {
        haveSub = himetricToScreen(pi.ptHimetricLocationRaw.x, pi.ptHimetricLocationRaw.y,
                                   devRect.left, devRect.top, devRect.right, devRect.bottom,
                                   dispRect.left, dispRect.top, dispRect.right, dispRect.bottom,
                                   sx, sy);
    }
    if (!haveSub) {
        // ② 대체 경로: 정수 화면 좌표. 정밀도를 잃지만 입력이 끊기는 것보다 낫다.
        sx = static_cast<f64>(pi.ptPixelLocationRaw.x);
        sy = static_cast<f64>(pi.ptPixelLocationRaw.y);
        ++stats_.himetricFallbacks;
    }

    // ③ 화면 → 클라이언트. ScreenToClient 는 정수만 받으므로 원점만 옮기고
    //    소수부는 우리가 보존한다. (창 원점은 항상 정수 픽셀이다.)
    POINT origin{0, 0};
    if (!::ClientToScreen(hwnd, &origin)) {
        return false;
    }
    cx = sx - static_cast<f64>(origin.x);
    cy = sy - static_cast<f64>(origin.y);
    return true;
}

bool PointerInput::makeSample(HWND hwnd, const POINTER_INFO& pi, const PenRawFields& pen,
                              bool fromHistory, PointerSample& out) noexcept {
    f64 clientX = 0.0, clientY = 0.0;
    if (!screenToClient(hwnd, pi, clientX, clientY)) {
        return false;
    }

    // 🔴 화면 좌표 → **캔버스 좌표**. docs/03 4.1 의 핵심.
    //    여기서부터 아래로는 줌·회전·팬과 무관한 불변 좌표만 흐른다.
    f64 canvasX = 0.0, canvasY = 0.0;
    view_.toCanvasD(clientX, clientY, canvasX, canvasY);

    // 🔴 시간은 단조 시계다. PerformanceCount 가 QPC 틱이다.
    //    dwTime(벽시계)을 쓰면 NTP 동기화가 획 시간을 뒤로 돌린다(docs/03 5.6).
    u64 ns = qpcToNs(static_cast<u64>(pi.PerformanceCount), qpcFreq_);
    if (ns == 0u) {
        ns = stroke::monotonicNowNs();
    }

    out.event = makeRawEvent(canvasX, canvasY, pen, ns);
    out.pen = pen;
    out.kind = kindOf(pi.pointerType);
    out.eraser = isEraserTip(pen);
    out.barrel = (pen.penFlags & kPenFlagBarrel) != 0u;
    out.fromHistory = fromHistory;
    out.pointerId = static_cast<u32>(pi.pointerId);
    return true;
}

void PointerInput::dispatch(HWND hwnd, u32 pointerId, int phase) noexcept {
    if (target_ == nullptr) {
        return;
    }

    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!::GetPointerType(pointerId, &type)) {
        return;
    }
    const PointerKind kind = kindOf(type);

    // 받을 장치인가.
    if (kind == PointerKind::Touch && !acceptTouch_) {
        ++stats_.ignoredTouch;
        return;
    }
    if (kind == PointerKind::Mouse && !acceptMouse_) {
        return;
    }

    // ── 펜: 이력까지 전부 건진다 ─────────────────────────────────────────
    //
    // 🔴 여기가 무손실의 핵심이다(docs/03 5.2).
    //    펜은 200~300Hz 로 보고하는데 WM_POINTERUPDATE 는 60~120Hz 로 온다.
    //    이력 API 를 안 쓰면 **점의 절반 이상을 그냥 버린다.** 선이 각지고,
    //    Sigan 의 정본 기록에도 구멍이 난다.
    if (kind == PointerKind::Pen) {
        UINT32 count = kHistoryCap;
        if (::GetPointerPenInfoHistory(pointerId, &count, penHistory_) && count > 0u) {
            if (count > kHistoryCap) {
                count = kHistoryCap; // API 가 상한을 안 지킬 경우 방어
            }
            // 🔴 이력은 **최신이 0번**이다. 시간 순서로 흘리려면 거꾸로 돈다.
            //    순서를 틀리면 선이 앞뒤로 튄다.
            for (UINT32 i = count; i > 0u; --i) {
                const POINTER_PEN_INFO& p = penHistory_[i - 1u];
                const bool isFirstInTime = (i == count);
                const bool isLastInTime = (i == 1u);
                PointerSample s;
                if (!makeSample(hwnd, p.pointerInfo, toPenFields(p),
                                !(isFirstInTime && isLastInTime), s)) {
                    continue;
                }
                if (!isLastInTime) {
                    ++stats_.historyRecovered;
                }
                // 🔴 down 은 **시간상 첫 점**에, up 은 **시간상 마지막 점**에 붙인다.
                //    나머지는 전부 move 다. 이걸 뒤집으면 획의 시작점·끝점이
                //    엉뚱한 좌표로 기록되고, Sigan 의 down/up 플래그가 어긋난다.
                if (phase == kDown && isFirstInTime) {
                    ++stats_.down;
                    target_->onPointerDown(s);
                } else if (phase == kUp && isLastInTime) {
                    ++stats_.up;
                    target_->onPointerUp(s);
                } else {
                    ++stats_.move;
                    target_->onPointerMove(s);
                }
            }
            return;
        }
        ++stats_.historyFailures;

        // 이력이 없으면 현재 점 하나만이라도 처리한다.
        POINTER_PEN_INFO one{};
        if (!::GetPointerPenInfo(pointerId, &one)) {
            return;
        }
        PointerSample s;
        if (!makeSample(hwnd, one.pointerInfo, toPenFields(one), false, s)) {
            return;
        }
        if (phase == kDown) {
            ++stats_.down;
            target_->onPointerDown(s);
        } else if (phase == kUp) {
            ++stats_.up;
            target_->onPointerUp(s);
        } else {
            ++stats_.move;
            target_->onPointerMove(s);
        }
        return;
    }

    // ── 마우스·터치: 펜 필드가 없다 ──────────────────────────────────────
    POINTER_INFO pi{};
    if (!::GetPointerInfo(pointerId, &pi)) {
        return;
    }
    PointerSample s;
    if (!makeSample(hwnd, pi, mousePenFields(), false, s)) {
        return;
    }
    if (phase == kDown) {
        ++stats_.down;
        target_->onPointerDown(s);
    } else if (phase == kUp) {
        ++stats_.up;
        target_->onPointerUp(s);
    } else {
        ++stats_.move;
        target_->onPointerMove(s);
    }
}

LRESULT PointerInput::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    bool& handled) noexcept {
    handled = false;
    if (target_ == nullptr || hwnd != hwnd_) {
        return 0;
    }

    const u32 id = static_cast<u32>(GET_POINTERID_WPARAM(wParam));

    switch (msg) {
    case WM_POINTERDOWN: {
        // 획 하나에 포인터 하나. 두 번째 펜·손가락은 무시한다 —
        // 멀티터치로 두 줄을 동시에 긋는 기능은 없다.
        if (activeId_ != 0u && activeId_ != id) {
            handled = true;
            return 0;
        }
        activeId_ = id;
        // ⚠️ 여기서 SetCapture() 를 부르지 않는다.
        //    WM_POINTERDOWN 부터 WM_POINTERUP 까지는 Windows 가 **암묵적으로**
        //    포인터를 이 창에 붙여 준다. 창 밖으로 끌고 나가도 메시지가 계속 온다.
        //    SetCapture 를 섞으면 마우스 캡처와 포인터 캡처가 엇갈려서
        //    WM_POINTERCAPTURECHANGED 가 엉뚱하게 뜬다.
        dispatch(hwnd, id, kDown);
        handled = true;
        return 0;
    }

    case WM_POINTERUPDATE: {
        if (activeId_ != 0u && activeId_ != id) {
            handled = true;
            return 0;
        }
        // 버튼이 안 눌린 호버 이동도 여기로 온다. 획 중이 아니면 흘려보낸다
        // (커서 미리보기는 UI 레이어가 따로 처리한다).
        if (activeId_ == 0u) {
            handled = false;
            return 0;
        }
        dispatch(hwnd, id, kMove);
        handled = true;
        return 0;
    }

    case WM_POINTERUP: {
        if (activeId_ != id) {
            handled = true;
            return 0;
        }
        dispatch(hwnd, id, kUp);
        activeId_ = 0;
        handled = true;
        return 0;
    }

    case WM_POINTERCAPTURECHANGED: {
        // 🔴 다른 창이 포인터를 가져갔다. 획을 안전하게 끝낸다.
        //    여기서 아무것도 안 하면 **영원히 안 끝나는 획**이 남는다 —
        //    Sigan 의 저널에 `up` 없는 획이 남고 복구가 꼬인다(docs/03 5.4).
        if (activeId_ != 0u) {
            ++stats_.cancel;
            target_->onPointerCancel(activeId_);
            activeId_ = 0;
        }
        handled = true;
        return 0;
    }

    case WM_POINTERLEAVE:
        // 창을 벗어났다. 획 중이면 캡처 덕분에 계속 온다 — 여기서 끝내지 않는다.
        handled = false;
        return 0;

    case WM_KILLFOCUS:
    case WM_CANCELMODE: {
        if (activeId_ != 0u) {
            ++stats_.cancel;
            target_->onPointerCancel(activeId_);
            activeId_ = 0;
        }
        handled = false; // 다른 핸들러도 봐야 한다
        return 0;
    }

    default:
        break;
    }

    (void)lParam;
    return 0;
}

} // namespace mari::win
