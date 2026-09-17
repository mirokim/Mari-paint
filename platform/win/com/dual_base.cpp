// Mari Paint — dual 베이스 지원 구현 (서버 락 · 타입 라이브러리 · 오류 정보)
#include <mari/win/com/dual_base.hpp>

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

// MIDL 산출물. 빌드 디렉터리에 생성된다(platform/win/CMakeLists.txt 참조).
#include "mari.h"

namespace mari::win::com {
namespace {

LONG g_objects = 0;      ///< 살아 있는 COM 객체 수
LONG g_locks = 0;        ///< LockServer 로 잡힌 수
void (*g_quitFn)() = nullptr;

std::once_flag g_tlOnce;
ITypeLib* g_typeLib = nullptr;
std::mutex g_tiMutex;
std::vector<std::pair<IID, ITypeInfo*>> g_tiCache;

/// 마지막 참조가 사라졌는지 보고, 그렇다면 종료를 알린다.
void maybeQuit() noexcept {
    if (::InterlockedCompareExchange(&g_objects, 0, 0) == 0 &&
        ::InterlockedCompareExchange(&g_locks, 0, 0) == 0 && g_quitFn != nullptr) {
        g_quitFn();
    }
}

/// 타입 라이브러리를 로드한다. 세 곳을 순서대로 본다.
void loadTypeLibOnce() {
    // 1) 등록된 것 — 정상 설치 상태.
    if (SUCCEEDED(::LoadRegTypeLib(LIBID_MariPaintLib, 1, 0, LOCALE_SYSTEM_DEFAULT, &g_typeLib))) {
        return;
    }
    // 2) 실행파일에 박힌 리소스 — 등록 전(예: /regserver 를 도는 중)에도 읽어야 한다.
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (SUCCEEDED(::LoadTypeLibEx(path, REGKIND_NONE, &g_typeLib))) {
            return;
        }
        // 3) 실행파일 옆의 mari.tlb — 개발 빌드 배치.
        std::wstring dir(path, n);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash + 1);
            dir += L"mari.tlb";
            ::LoadTypeLibEx(dir.c_str(), REGKIND_NONE, &g_typeLib);
        }
    }
}

} // namespace

// ── ServerLock ──────────────────────────────────────────────────────────────

void ServerLock::addObject() noexcept { ::InterlockedIncrement(&g_objects); }

void ServerLock::releaseObject() noexcept {
    if (::InterlockedDecrement(&g_objects) == 0) {
        maybeQuit();
    }
}

void ServerLock::lockServer(bool lock) noexcept {
    if (lock) {
        ::InterlockedIncrement(&g_locks);
    } else if (::InterlockedDecrement(&g_locks) == 0) {
        maybeQuit();
    }
}

long ServerLock::count() noexcept {
    return static_cast<long>(::InterlockedCompareExchange(&g_objects, 0, 0) +
                             ::InterlockedCompareExchange(&g_locks, 0, 0));
}

void ServerLock::setQuitHandler(void (*fn)()) noexcept { g_quitFn = fn; }

// ── 타입 라이브러리 ─────────────────────────────────────────────────────────

HRESULT preloadTypeLib() {
    std::call_once(g_tlOnce, loadTypeLibOnce);
    return g_typeLib != nullptr ? S_OK : TYPE_E_CANTLOADLIBRARY;
}

HRESULT typeInfoFor(REFIID iid, ITypeInfo** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;

    std::call_once(g_tlOnce, loadTypeLibOnce);
    if (g_typeLib == nullptr) {
        // 🔴 여기서 실패해도 vtable 호출은 계속 된다. IDispatch 클라이언트만 못 붙는다.
        //    C++/C# 스크립트를 살려 두는 게 전부 죽이는 것보다 낫다.
        return TYPE_E_CANTLOADLIBRARY;
    }

    std::lock_guard<std::mutex> lk(g_tiMutex);
    for (auto& [cachedIid, ti] : g_tiCache) {
        if (cachedIid == iid) {
            ti->AddRef();
            *out = ti;
            return S_OK;
        }
    }
    ITypeInfo* ti = nullptr;
    const HRESULT hr = g_typeLib->GetTypeInfoOfGuid(iid, &ti);
    if (FAILED(hr)) {
        return hr;
    }
    ti->AddRef(); // 캐시 몫
    g_tiCache.emplace_back(iid, ti);
    *out = ti;
    return S_OK;
}

void releaseTypeLibCache() noexcept {
    std::lock_guard<std::mutex> lk(g_tiMutex);
    for (auto& [iid, ti] : g_tiCache) {
        (void)iid;
        if (ti != nullptr) {
            ti->Release();
        }
    }
    g_tiCache.clear();
    if (g_typeLib != nullptr) {
        g_typeLib->Release();
        g_typeLib = nullptr;
    }
}

// ── 오류 정보 ───────────────────────────────────────────────────────────────

HRESULT setErrorInfo(REFIID iid, const std::string& utf8Message, HRESULT hr) {
    ICreateErrorInfo* cei = nullptr;
    if (FAILED(::CreateErrorInfo(&cei)) || cei == nullptr) {
        return hr;
    }
    const Bstr msg = bstrFromUtf8(utf8Message);
    const Bstr src(L"MariPaint");
    cei->SetGUID(iid);
    cei->SetSource(src.get());
    cei->SetDescription(msg.get());

    IErrorInfo* ei = nullptr;
    if (SUCCEEDED(cei->QueryInterface(IID_IErrorInfo, reinterpret_cast<void**>(&ei)))) {
        ::SetErrorInfo(0, ei);
        ei->Release();
    }
    cei->Release();
    return hr;
}

HRESULT hresultFromError(REFIID iid, const mari::Error& e) {
    HRESULT hr = E_FAIL;
    switch (e.code) {
    case mari::ErrorCode::InvalidArgument:
        hr = E_INVALIDARG;
        break;
    case mari::ErrorCode::NotFound:
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        break;
    case mari::ErrorCode::IoError:
        hr = HRESULT_FROM_WIN32(ERROR_IO_DEVICE);
        break;
    case mari::ErrorCode::ParseError:
        hr = E_UNEXPECTED;
        break;
    case mari::ErrorCode::Unsupported:
        hr = E_NOTIMPL;
        break;
    case mari::ErrorCode::OutOfMemory:
        hr = E_OUTOFMEMORY;
        break;
    case mari::ErrorCode::Cancelled:
        hr = E_ABORT;
        break;
    case mari::ErrorCode::Unknown:
    default:
        hr = E_FAIL;
        break;
    }
    // 🔴 메시지를 반드시 같이 싣는다. 'Unspecified error' 만 보면 아무도 못 고친다.
    return setErrorInfo(iid, e.message, hr);
}

} // namespace mari::win::com
