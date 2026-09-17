// Mari Paint — IMari8bfHost 구현 (호스트 프로세스 쪽)
//
// 🔴 픽셀은 이 인터페이스를 타지 않는다. 매핑 이름과 크기만 오간다(docs/02 6.2).
#include "plugin_loader.hpp"

#include <mari/win/com/dual_base.hpp>
#include <mari/win/com/server.hpp>

#include <new>

#include "mari.h"

namespace mari::host8bf {

/// plugin_loader.cpp 가 들고 있는, 지금 처리 중인 공유 메모리 헤더.
extern ShmHeader* g_header;
/// 진행률을 읽기 위한 접근자(호스트 안에서만 쓴다).
[[nodiscard]] ShmHeader* progressHeader() noexcept;

namespace {

using mari::win::com::Bstr;
using mari::win::com::bstrFromUtf8;
using mari::win::com::DualBase;
using mari::win::com::returnScalar;
using mari::win::com::setErrorInfo;
using mari::win::com::wideToUtf8;

std::string fromBstr(BSTR b) {
    return b == nullptr ? std::string() : wideToUtf8(std::wstring(b, ::SysStringLen(b)));
}

/// 문자열 목록을 SAFEARRAY(BSTR) 로. note 를 본체에 넘길 때 쓴다.
SAFEARRAY* safeArrayOfStrings(const std::vector<std::string>& v) {
    SAFEARRAY* sa = ::SafeArrayCreateVector(VT_BSTR, 0, static_cast<ULONG>(v.size()));
    if (sa == nullptr) {
        return nullptr;
    }
    for (LONG i = 0; i < static_cast<LONG>(v.size()); ++i) {
        Bstr b = bstrFromUtf8(v[static_cast<size_t>(i)]);
        BSTR raw = b.detach();
        if (FAILED(::SafeArrayPutElement(sa, &i, raw))) {
            ::SysFreeString(raw);
            ::SafeArrayDestroy(sa);
            return nullptr;
        }
        ::SysFreeString(raw); // PutElement 가 자기 사본을 만든다
    }
    return sa;
}

class Host8bfImpl final : public DualBase<IMari8bfHost, &IID_IMari8bfHost> {
public:
    HRESULT STDMETHODCALLTYPE get_Is64Bit(VARIANT_BOOL* pVal) override {
        return returnScalar<VARIANT_BOOL>(pVal, kHost64Bit ? VARIANT_TRUE : VARIANT_FALSE);
    }

    HRESULT STDMETHODCALLTYPE Enumerate(BSTR pluginPath, SAFEARRAY** pNotes,
                                        SAFEARRAY** pFilterNames) override {
        if (pNotes == nullptr || pFilterNames == nullptr) {
            return E_POINTER;
        }
        *pNotes = nullptr;
        *pFilterNames = nullptr;

        auto man = scanPlugin(fromBstr(pluginPath));
        if (!man.ok()) {
            // 🔴 실패해도 이유를 문자열로 돌려준다. 사용자가 "왜 안 뜨지" 를 알아야 한다.
            std::vector<std::string> notes{man.error().message};
            *pNotes = safeArrayOfStrings(notes);
            return setErrorInfo(IID_IMari8bfHost, man.error().message, E_FAIL);
        }
        std::vector<std::string> names;
        for (const PluginEntry& e : man.value().entries) {
            names.push_back(e.category.empty() ? e.name : (e.category + "/" + e.name));
        }
        *pFilterNames = safeArrayOfStrings(names);
        *pNotes = safeArrayOfStrings(man.value().notes);
        if (*pFilterNames == nullptr || *pNotes == nullptr) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Apply(BSTR pluginPath, BSTR filterName, BSTR shmName,
                                    LONGLONG shmSize, LONG timeoutMs, SAFEARRAY** pNotes,
                                    MariHostStatus* pStatus) override {
        if (pStatus == nullptr || pNotes == nullptr) {
            return E_POINTER;
        }
        *pNotes = nullptr;
        *pStatus = mariHostShmError;

        if (shmSize <= 0) {
            return setErrorInfo(IID_IMari8bfHost, "공유 메모리 크기가 0 이하다", E_INVALIDARG);
        }

        FilterRequest req;
        req.pluginPath = fromBstr(pluginPath);
        req.filterName = fromBstr(filterName);
        req.shmName = fromBstr(shmName);
        req.shmSize = static_cast<u64>(shmSize);
        req.timeoutMs = timeoutMs > 0 ? static_cast<u32>(timeoutMs) : kDefaultTimeoutMs;

        // 🔴 여기서 남의 네이티브 코드가 돈다. 죽으면 **이 프로세스만** 죽는다.
        //    본체는 RPC_S_SERVER_UNAVAILABLE 을 받고 살아 있다(docs/02 6.2).
        const FilterResult r = runFilter(req);

        *pStatus = static_cast<MariHostStatus>(r.status);
        *pNotes = safeArrayOfStrings(r.notes);
        return *pNotes != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Cancel() override {
        // 다른 스레드(COM 이 만든 RPC 스레드)에서 불릴 수 있다. atomic 이라 안전하다.
        abortSignal().requestCancel();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Progress(LONG* pVal) override {
        const ShmHeader* h = progressHeader();
        return returnScalar<LONG>(pVal, h != nullptr ? static_cast<LONG>(h->progress) : 0);
    }

    HRESULT STDMETHODCALLTYPE Ping() override {
        // 살아 있다는 것 자체가 답이다. 죽었으면 본체는 RPC_* 오류를 받는다.
        return S_OK;
    }
};

} // namespace

ShmHeader* progressHeader() noexcept { return g_header; }

} // namespace mari::host8bf

namespace mari::win::com {

HRESULT createMari8bfHost(REFIID riid, void** ppv) {
    auto* obj = new (std::nothrow) mari::host8bf::Host8bfImpl();
    if (obj == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = obj->QueryInterface(riid, ppv);
    obj->Release();
    return hr;
}

} // namespace mari::win::com
