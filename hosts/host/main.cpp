// Mari Paint — mari-8bf-host-{x86,x64}.exe 진입점
//
// 🔴 이 프로세스가 죽는 것은 **정상 동작의 일부**다.
//    남의 .8bf 코드가 여기서 돈다. 죽으면 본체는 RPC 오류를 받고 필터만 실패로
//    처리한다. 그림은 그대로 있다(docs/02 6.2).
//
//    그래서 여기에는 일부러 넣지 않은 것이 있다:
//      · 크래시 핸들러 — 죽는 걸 막으려 애쓰지 않는다. 깔끔하게 죽는 게 낫다
//      · 전역 예외 필터로 계속 진행하기 — 깨진 상태로 픽셀을 되돌려주면 더 나쁘다
//      · UI — 이 프로세스는 창을 만들지 않는다
#include "plugin_loader.hpp"

#include <mari/win/com/dual_base.hpp>
#include <mari/win/com/registration.hpp>
#include <mari/win/com/server.hpp>

#include <windows.h>

#include <cstdio>

#include "mari.h"

namespace {

/// 호스트가 놀고 있으면 이만큼 뒤에 스스로 종료한다.
/// 본체가 깜빡 잊고 참조를 안 놓아도 좀비가 안 남는다.
constexpr UINT kIdleExitMs = 60 * 1000;
constexpr UINT_PTR kIdleTimerId = 1;

void CALLBACK idleTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (mari::win::com::ServerLock::count() == 0) {
        ::PostQuitMessage(0);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // 등록/해제 명령이면 여기서 끝난다.
    const auto act = mari::win::com::handleRegistrationCommandLine(argc, argv);
    if (act.handled) {
        return act.exitCode;
    }

    // 🔴 `/Embedding` 없이 직접 실행하면 아무것도 안 한다.
    //    이 실행파일을 사용자가 더블클릭해도 할 일이 없다 — 본체가 COM 으로 띄운다.
    if (!act.embedding) {
        std::fwprintf(stderr,
                      L"mari-8bf-host: COM 이 띄우는 프로세스다. 직접 실행할 것이 아니다.\n"
                      L"  등록:   mari-8bf-host.exe /regserver\n"
                      L"  해제:   mari-8bf-host.exe /unregserver\n");
        return 2;
    }

    // STA 로 연다. 8bf 플러그인은 UI(대화상자)를 띄울 수 있고, 그건 STA 를 요구한다.
    const HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrInit)) {
        return 3;
    }

    mari::win::com::preloadTypeLib();
    mari::win::com::addClassObject(CLSID_Mari8bfHost, &mari::win::com::createMari8bfHost);
    mari::win::com::installQuitOnIdle(::GetCurrentThreadId());

    const auto reg = mari::win::com::registerClassObjects();
    if (!reg.ok()) {
        std::fwprintf(stderr, L"클래스 객체 등록 실패: %hs\n", reg.message().c_str());
        ::CoUninitialize();
        return 4;
    }

    ::SetTimer(nullptr, kIdleTimerId, kIdleExitMs, idleTimer);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::KillTimer(nullptr, kIdleTimerId);
    mari::win::com::revokeClassObjects();
    mari::win::com::releaseTypeLibCache();
    ::CoUninitialize();
    return 0;
}
