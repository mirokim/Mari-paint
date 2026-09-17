// Mari Paint — COM 서버 등록 (docs/02 6.3)
//
// 🔴 **LocalServer32 로 등록한다. InprocServer32 가 아니다.**
//    프로세스 분리가 설계의 핵심이다(설계 원칙 3). InprocServer32 로 바꾸면
//    스크립트·플러그인이 우리 주소 공간에서 돌고, 그리기 스레드가 남의 코드에 막힌다.
//    → 이 파일에 InprocServer32 를 쓰는 코드가 들어오면 그건 설계 위반이다.
//
// 등록 위치: 기본은 **HKCU** (`HKEY_CURRENT_USER\Software\Classes`).
//    관리자 권한 없이 등록되고, 사용자별로 깔린 Mari 가 서로 안 싸운다.
//    `--admin` 을 주면 HKLM 에 쓴다(전사 배포용).
//
// ⚠️ Windows 전용.
#ifndef MARI_WIN_COM_REGISTRATION_HPP
#define MARI_WIN_COM_REGISTRATION_HPP

#if !defined(_WIN32)
#error "mari/win/com/registration.hpp 는 Windows 전용이다"
#endif

#include <mari/core/result.hpp>

#include <objbase.h>

#include <cstddef>
#include <string>

namespace mari::win::com {

/// 어느 하이브에 쓸 것인가.
enum class RegistryScope {
    CurrentUser, ///< HKCU\Software\Classes — 기본. 관리자 권한 불필요
    LocalMachine ///< HKLM\Software\Classes — 전사 배포용. 관리자 권한 필요
};

/// 등록할 코클래스 하나의 정보.
struct CoClassInfo {
    const GUID* clsid;
    const wchar_t* progId;            ///< "MariPaint.Application"
    const wchar_t* versionIndepProgId; ///< "MariPaint.Application.1" 의 버전 무관 이름
    const wchar_t* description;
    /// 🔴 여기가 LocalServer32 다. 실행파일 경로 + " /Embedding" 이 들어간다.
    const wchar_t* appIdSuffix; ///< nullptr 이면 AppId 를 쓰지 않는다
};

/// 이 서버가 등록하는 코클래스 목록.
[[nodiscard]] const CoClassInfo* coClasses(size_t& outCount) noexcept;

/// 레지스트리에 등록한다(self-registration `--regserver`).
/// 타입 라이브러리도 같이 등록한다(RegisterTypeLibForUser / RegisterTypeLib).
[[nodiscard]] mari::Result<void> registerServer(RegistryScope scope);

/// 등록을 지운다(`--unregserver`).
/// **실패해도 끝까지 간다** — 반쯤 지워진 채로 남기지 않는다. 마지막 오류만 돌려준다.
[[nodiscard]] mari::Result<void> unregisterServer(RegistryScope scope);

/// 등록 대신 `.reg` 파일을 만든다(`--writereg <path>`).
///
/// 왜 필요한가: MSI·Chocolatey·winget 같은 패키저는 **실행파일을 돌리지 않고**
/// 레지스트리만 넣고 싶어 한다. self-registration 을 강요하면 사일런트 설치가 깨진다.
[[nodiscard]] mari::Result<void> writeRegFile(const std::wstring& outPath, RegistryScope scope);

/// 지금 실행 중인 모듈의 전체 경로. 등록에 쓴다.
[[nodiscard]] std::wstring modulePath();

/// 명령줄을 보고 등록 관련 동작을 처리한다.
///
/// 처리하는 스위치(`-`, `--`, `/` 전부 받는다 — COM 관례가 `/` 다):
///   `/regserver`   `/unregserver`   `/writereg <path>`   `/admin`   `/embedding`
///
/// 돌려주는 값:
///   · `handled == true`  → 등록 작업을 했다. 프로세스는 `exitCode` 로 끝내야 한다.
///   · `handled == false` → 평범한 실행이다. 계속 가라.
struct CommandLineAction {
    bool handled = false;
    bool embedding = false; ///< COM 이 띄운 것이다(`/Embedding`). UI 를 띄우지 않는다
    int exitCode = 0;
};
[[nodiscard]] CommandLineAction handleRegistrationCommandLine(int argc, wchar_t** argv);

} // namespace mari::win::com

#endif // MARI_WIN_COM_REGISTRATION_HPP
