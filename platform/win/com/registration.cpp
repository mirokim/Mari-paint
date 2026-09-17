// Mari Paint — COM 서버 등록 구현 (docs/02 6.3)
//
// 🔴 **LocalServer32 만 쓴다. InprocServer32 는 이 파일 어디에도 없다.**
//    프로세스 분리가 설계의 핵심이다(설계 원칙 3).
#include <mari/win/com/registration.hpp>

#include <mari/win/com/com_ptr.hpp>

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "mari.h"

namespace mari::win::com {
namespace {

/// CLSID 를 "{XXXXXXXX-...}" 문자열로.
std::wstring guidToString(const GUID& g) {
    wchar_t buf[64] = {};
    const int n = ::StringFromGUID2(g, buf, 64);
    return n > 0 ? std::wstring(buf, static_cast<size_t>(n - 1)) : std::wstring();
}

HKEY rootFor(RegistryScope scope) noexcept {
    return scope == RegistryScope::LocalMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

/// 하이브 안의 Classes 경로. HKCU 든 HKLM 든 `Software\Classes` 아래다.
constexpr const wchar_t* kClassesPath = L"Software\\Classes";

/// 키를 만들고 값 하나를 쓴다. 중간 경로도 만든다.
LSTATUS writeKeyValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                      const std::wstring& data) {
    HKEY key = nullptr;
    LSTATUS st = ::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                   KEY_WRITE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        return st;
    }
    st = ::RegSetValueExW(key, valueName, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(data.c_str()),
                          static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
    return st;
}

/// 키 하나와 그 아래 전부를 지운다. 없으면 성공으로 친다.
LSTATUS deleteTree(HKEY root, const std::wstring& subKey) {
    const LSTATUS st = ::RegDeleteTreeW(root, subKey.c_str());
    return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : st;
}

/// ProgId 버전 접미사. 인터페이스가 깨지는 변경을 하면 여기를 올린다.
constexpr const wchar_t* kVersion = L"1";

const CoClassInfo kCoClasses[] = {
    {&CLSID_MariApplication, L"MariPaint.Application.1", L"MariPaint.Application",
     L"Mari Paint 애플리케이션", L"MariPaint"},
    {&CLSID_Mari8bfHost, L"MariPaint.Host8bf.1", L"MariPaint.Host8bf",
     L"Mari .8bf 격리 호스트", L"MariPaint"},
};

} // namespace

const CoClassInfo* coClasses(size_t& outCount) noexcept {
    outCount = sizeof(kCoClasses) / sizeof(kCoClasses[0]);
    return kCoClasses;
}

std::wstring modulePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size() - 1) {
            return std::wstring(buf.data(), n);
        }
        buf.resize(buf.size() * 2); // 긴 경로(\\?\ 접두사 포함)에서도 살아남는다
    }
}

mari::Result<void> registerServer(RegistryScope scope) {
    const std::wstring exe = modulePath();
    if (exe.empty()) {
        return mari::Err("실행파일 경로를 알아내지 못했다", mari::ErrorCode::IoError);
    }
    // 🔴 LocalServer32 의 값은 **따옴표로 감싼 경로 + " /Embedding"** 이다.
    //    따옴표를 빼면 "C:\Program Files\..." 의 공백에서 잘려 엉뚱한 걸 실행한다.
    const std::wstring localServer = L"\"" + exe + L"\" /Embedding";

    const HKEY root = rootFor(scope);
    const std::wstring classes = kClassesPath;

    size_t n = 0;
    const CoClassInfo* cls = coClasses(n);
    for (size_t i = 0; i < n; ++i) {
        const CoClassInfo& c = cls[i];
        const std::wstring clsidStr = guidToString(*c.clsid);
        if (clsidStr.empty()) {
            return mari::Err("CLSID 문자열 변환에 실패했다", mari::ErrorCode::Unknown);
        }
        const std::wstring clsKey = classes + L"\\CLSID\\" + clsidStr;

        LSTATUS st = writeKeyValue(root, clsKey, nullptr, c.description);
        if (st != ERROR_SUCCESS) {
            return mari::Err("CLSID 키를 쓰지 못했다(권한을 확인해라)", mari::ErrorCode::IoError);
        }
        // 🔴 여기. InprocServer32 가 아니다.
        st = writeKeyValue(root, clsKey + L"\\LocalServer32", nullptr, localServer);
        if (st != ERROR_SUCCESS) {
            return mari::Err("LocalServer32 를 쓰지 못했다", mari::ErrorCode::IoError);
        }
        writeKeyValue(root, clsKey + L"\\ProgID", nullptr, c.progId);
        writeKeyValue(root, clsKey + L"\\VersionIndependentProgID", nullptr,
                      c.versionIndepProgId);
        writeKeyValue(root, clsKey + L"\\TypeLib", nullptr, guidToString(LIBID_MariPaintLib));
        writeKeyValue(root, clsKey + L"\\Version", nullptr, L"1.0");

        // ProgId → CLSID (스크립트가 `CreateObject("MariPaint.Application")` 로 찾는 길)
        writeKeyValue(root, classes + L"\\" + c.progId, nullptr, c.description);
        writeKeyValue(root, classes + L"\\" + std::wstring(c.progId) + L"\\CLSID", nullptr,
                      clsidStr);
        writeKeyValue(root, classes + L"\\" + c.versionIndepProgId, nullptr, c.description);
        writeKeyValue(root, classes + L"\\" + std::wstring(c.versionIndepProgId) + L"\\CLSID",
                      nullptr, clsidStr);
        writeKeyValue(root, classes + L"\\" + std::wstring(c.versionIndepProgId) + L"\\CurVer",
                      nullptr, c.progId);
    }

    // 타입 라이브러리 등록. **이게 없으면 IDispatch 가 안 돌아** —
    // Python·VBA·PowerShell 이 전부 못 붙는다(docs/02 6.1).
    ITypeLib* tl = nullptr;
    HRESULT hr = ::LoadTypeLibEx(exe.c_str(), REGKIND_NONE, &tl);
    if (FAILED(hr) || tl == nullptr) {
        // 실행파일에 리소스로 안 박혀 있으면 옆의 mari.tlb 를 찾는다(개발 빌드).
        std::wstring tlb = exe;
        const size_t slash = tlb.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            tlb.resize(slash + 1);
        }
        tlb += L"mari.tlb";
        hr = ::LoadTypeLibEx(tlb.c_str(), REGKIND_NONE, &tl);
    }
    if (SUCCEEDED(hr) && tl != nullptr) {
        const std::wstring help = exe.substr(0, exe.find_last_of(L"\\/") + 1);
        hr = scope == RegistryScope::LocalMachine
                 ? ::RegisterTypeLib(tl, exe.c_str(), help.c_str())
                 : ::RegisterTypeLibForUser(tl, const_cast<OLECHAR*>(exe.c_str()),
                                            const_cast<OLECHAR*>(help.c_str()));
        tl->Release();
        if (FAILED(hr)) {
            return mari::Err("타입 라이브러리를 등록하지 못했다 — "
                             "IDispatch 클라이언트(Python/VBA/PowerShell)가 못 붙는다",
                             mari::ErrorCode::IoError);
        }
    } else {
        return mari::Err("타입 라이브러리(mari.tlb)를 찾지 못했다", mari::ErrorCode::NotFound);
    }

    return mari::Ok();
}

mari::Result<void> unregisterServer(RegistryScope scope) {
    // 🔴 실패해도 끝까지 간다. 반쯤 지워진 등록을 남기면 다음 설치가 더 꼬인다.
    const HKEY root = rootFor(scope);
    const std::wstring classes = kClassesPath;
    LSTATUS worst = ERROR_SUCCESS;

    size_t n = 0;
    const CoClassInfo* cls = coClasses(n);
    for (size_t i = 0; i < n; ++i) {
        const CoClassInfo& c = cls[i];
        const std::wstring clsidStr = guidToString(*c.clsid);
        LSTATUS st = deleteTree(root, classes + L"\\CLSID\\" + clsidStr);
        if (st != ERROR_SUCCESS) {
            worst = st;
        }
        st = deleteTree(root, classes + L"\\" + c.progId);
        if (st != ERROR_SUCCESS) {
            worst = st;
        }
        st = deleteTree(root, classes + L"\\" + c.versionIndepProgId);
        if (st != ERROR_SUCCESS) {
            worst = st;
        }
    }

    const HRESULT hr =
        scope == RegistryScope::LocalMachine
            ? ::UnRegisterTypeLib(LIBID_MariPaintLib, 1, 0, LOCALE_NEUTRAL, SYS_WIN64)
            : ::UnRegisterTypeLibForUser(LIBID_MariPaintLib, 1, 0, LOCALE_NEUTRAL, SYS_WIN64);
    (void)hr; // 없으면 없는 대로 둔다

    if (worst != ERROR_SUCCESS) {
        return mari::Err("등록 해제 중 일부 키를 지우지 못했다", mari::ErrorCode::IoError);
    }
    return mari::Ok();
}

mari::Result<void> writeRegFile(const std::wstring& outPath, RegistryScope scope) {
    const std::wstring exe = modulePath();
    if (exe.empty()) {
        return mari::Err("실행파일 경로를 알아내지 못했다", mari::ErrorCode::IoError);
    }

    // .reg 에서는 역슬래시를 두 번 쓴다.
    std::wstring escaped;
    escaped.reserve(exe.size() * 2);
    for (wchar_t ch : exe) {
        if (ch == L'\\') {
            escaped += L"\\\\";
        } else if (ch == L'"') {
            escaped += L"\\\"";
        } else {
            escaped += ch;
        }
    }
    const std::wstring localServer = L"\\\"" + escaped + L"\\\" /Embedding";
    const wchar_t* hive =
        scope == RegistryScope::LocalMachine ? L"HKEY_LOCAL_MACHINE" : L"HKEY_CURRENT_USER";

    FILE* f = nullptr;
    if (::_wfopen_s(&f, outPath.c_str(), L"wb") != 0 || f == nullptr) {
        return mari::Err("`.reg` 파일을 열지 못했다", mari::ErrorCode::IoError);
    }
    // UTF-16LE BOM — regedit 이 요구한다.
    const unsigned char bom[2] = {0xFF, 0xFE};
    std::fwrite(bom, 1, 2, f);

    auto put = [f](const std::wstring& s) {
        std::fwrite(s.data(), sizeof(wchar_t), s.size(), f);
    };

    put(L"Windows Registry Editor Version 5.00\r\n\r\n");
    put(L"; Mari Paint COM 등록\r\n");
    put(L"; 🔴 LocalServer32 다 (InprocServer32 아님) — docs/02 6.3\r\n");
    put(L"; 프로세스 분리가 설계의 핵심이다. 이 파일을 손으로 고쳐 InprocServer32 로\r\n");
    put(L"; 바꾸면 스크립트가 Mari 주소 공간에서 돌고 그리기 스레드가 막힌다.\r\n\r\n");

    size_t n = 0;
    const CoClassInfo* cls = coClasses(n);
    for (size_t i = 0; i < n; ++i) {
        const CoClassInfo& c = cls[i];
        const std::wstring g = guidToString(*c.clsid);
        const std::wstring base = std::wstring(hive) + L"\\Software\\Classes";

        put(L"[" + base + L"\\CLSID\\" + g + L"]\r\n");
        put(L"@=\"" + std::wstring(c.description) + L"\"\r\n\r\n");

        put(L"[" + base + L"\\CLSID\\" + g + L"\\LocalServer32]\r\n");
        put(L"@=\"" + localServer + L"\"\r\n\r\n");

        put(L"[" + base + L"\\CLSID\\" + g + L"\\ProgID]\r\n");
        put(L"@=\"" + std::wstring(c.progId) + L"\"\r\n\r\n");

        put(L"[" + base + L"\\CLSID\\" + g + L"\\VersionIndependentProgID]\r\n");
        put(L"@=\"" + std::wstring(c.versionIndepProgId) + L"\"\r\n\r\n");

        put(L"[" + base + L"\\CLSID\\" + g + L"\\TypeLib]\r\n");
        put(L"@=\"" + guidToString(LIBID_MariPaintLib) + L"\"\r\n\r\n");

        put(L"[" + base + L"\\" + c.progId + L"]\r\n");
        put(L"@=\"" + std::wstring(c.description) + L"\"\r\n\r\n");
        put(L"[" + base + L"\\" + c.progId + L"\\CLSID]\r\n");
        put(L"@=\"" + g + L"\"\r\n\r\n");

        put(L"[" + base + L"\\" + c.versionIndepProgId + L"]\r\n");
        put(L"@=\"" + std::wstring(c.description) + L"\"\r\n\r\n");
        put(L"[" + base + L"\\" + c.versionIndepProgId + L"\\CLSID]\r\n");
        put(L"@=\"" + g + L"\"\r\n\r\n");
        put(L"[" + base + L"\\" + c.versionIndepProgId + L"\\CurVer]\r\n");
        put(L"@=\"" + std::wstring(c.progId) + L"\"\r\n\r\n");
    }

    put(L"; ⚠️ 타입 라이브러리(TypeLib 키)는 .reg 로 안 쓴다.\r\n");
    put(L";    RegisterTypeLib 이 만드는 키가 OS 버전·비트수마다 다르다.\r\n");
    put(L";    설치 프로그램은 `mari-paint.exe /regserver` 를 한 번 부르거나,\r\n");
    put(L";    MSI 의 TypeLib 테이블을 써라. 여기서 추측해서 쓰면 IDispatch 가 깨진다.\r\n");

    std::fclose(f);
    (void)kVersion;
    return mari::Ok();
}

CommandLineAction handleRegistrationCommandLine(int argc, wchar_t** argv) {
    CommandLineAction act;
    RegistryScope scope = RegistryScope::CurrentUser;
    bool doRegister = false;
    bool doUnregister = false;
    std::wstring regFileOut;

    auto matches = [](const wchar_t* arg, const wchar_t* name) {
        if (arg == nullptr) {
            return false;
        }
        if (arg[0] != L'-' && arg[0] != L'/') {
            return false;
        }
        const wchar_t* p = arg + 1;
        if (*p == L'-') {
            ++p;
        }
        return ::_wcsicmp(p, name) == 0;
    };

    for (int i = 1; i < argc; ++i) {
        if (matches(argv[i], L"admin")) {
            scope = RegistryScope::LocalMachine;
        } else if (matches(argv[i], L"regserver")) {
            doRegister = true;
        } else if (matches(argv[i], L"unregserver")) {
            doUnregister = true;
        } else if (matches(argv[i], L"embedding")) {
            // 🔴 COM 이 우리를 띄웠다. UI 를 띄우지 말고 조용히 클래스 팩토리만 등록한다.
            act.embedding = true;
        } else if (matches(argv[i], L"writereg") && i + 1 < argc) {
            regFileOut = argv[++i];
        }
    }

    if (doUnregister) {
        const auto r = unregisterServer(scope);
        act.handled = true;
        act.exitCode = r.ok() ? 0 : 1;
        if (!r.ok()) {
            std::fwprintf(stderr, L"등록 해제 실패: %hs\n", r.message().c_str());
        }
        return act;
    }
    if (doRegister) {
        const auto r = registerServer(scope);
        act.handled = true;
        act.exitCode = r.ok() ? 0 : 1;
        if (!r.ok()) {
            std::fwprintf(stderr, L"등록 실패: %hs\n", r.message().c_str());
        }
        return act;
    }
    if (!regFileOut.empty()) {
        const auto r = writeRegFile(regFileOut, scope);
        act.handled = true;
        act.exitCode = r.ok() ? 0 : 1;
        if (!r.ok()) {
            std::fwprintf(stderr, L".reg 생성 실패: %hs\n", r.message().c_str());
        }
        return act;
    }
    return act;
}

} // namespace mari::win::com
