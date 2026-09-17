// Mari Paint — COM LocalServer 진입점
//
// 🔴 LocalServer32 다(docs/02 6.3). 별도 프로세스에서 돌고, 마지막 참조가 사라지면
//    스스로 종료한다. InprocServer32 의 DllGetClassObject/DllCanUnloadNow 는 없다 —
//    있으면 안 된다.
//
// ⚠️ Windows 전용.
#ifndef MARI_WIN_COM_SERVER_HPP
#define MARI_WIN_COM_SERVER_HPP

#if !defined(_WIN32)
#error "mari/win/com/server.hpp 는 Windows 전용이다"
#endif

#include <mari/core/result.hpp>

#include <objbase.h>

namespace mari::win::com {

/// 코클래스 하나를 만드는 함수. 클래스 팩토리가 부른다.
using CreateInstanceFn = HRESULT (*)(REFIID riid, void** ppv);

/// MariApplication 을 만든다(application.cpp).
HRESULT createMariApplication(REFIID riid, void** ppv);
/// Mari8bfHost 를 만든다(hosts/host/host_object.cpp — 호스트 실행파일에만 링크된다).
HRESULT createMari8bfHost(REFIID riid, void** ppv);

/// 클래스 팩토리를 등록한다. `/Embedding` 으로 떴을 때 부른다.
///
/// `REGCLS_MULTIPLEUSE` 로 등록한다 — 스크립트가 `CreateObject` 를 여러 번 불러도
/// **프로세스를 여러 개 띄우지 않는다.** 그리기 툴은 하나만 떠 있어야 한다.
[[nodiscard]] mari::Result<void> registerClassObjects();

/// 등록한 클래스 팩토리를 뗀다. 종료 직전에 부른다.
void revokeClassObjects() noexcept;

/// 이 프로세스가 등록할 코클래스를 더한다. 실행파일마다 다르다
/// (본체는 MariApplication, 호스트는 Mari8bfHost).
void addClassObject(const CLSID& clsid, CreateInstanceFn fn);

/// COM 참조가 0 이 되면 불린다. 기본 구현은 메인 스레드에 WM_QUIT 을 던진다.
void installQuitOnIdle(DWORD mainThreadId) noexcept;

} // namespace mari::win::com

#endif // MARI_WIN_COM_SERVER_HPP
