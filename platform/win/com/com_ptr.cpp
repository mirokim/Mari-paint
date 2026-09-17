// Mari Paint — COM 문자열·배열 변환 구현
#include <mari/win/com/com_ptr.hpp>

#include <windows.h>

namespace mari::win::com {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) {
        // 잘못된 UTF-8 이다. 조용히 빈 문자열로 만들지 않고, 대체 문자를 허용해
        // 다시 시도한다 — 파일 이름 하나 때문에 스크립트 전체가 죽으면 안 된다.
        const int need2 = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                                nullptr, 0);
        if (need2 <= 0) {
            return {};
        }
        std::wstring out(static_cast<size_t>(need2), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need2);
        return out;
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

SAFEARRAY* safeArrayFromBytes(const unsigned char* data, size_t len) {
    if (len > static_cast<size_t>(ULONG_MAX)) {
        return nullptr;
    }
    SAFEARRAY* sa = ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(len));
    if (sa == nullptr) {
        return nullptr;
    }
    if (len == 0u || data == nullptr) {
        return sa;
    }
    void* dst = nullptr;
    if (FAILED(::SafeArrayAccessData(sa, &dst))) {
        ::SafeArrayDestroy(sa);
        return nullptr;
    }
    ::memcpy(dst, data, len);
    ::SafeArrayUnaccessData(sa);
    return sa;
}

HRESULT bytesFromSafeArray(SAFEARRAY* sa, const unsigned char** outData, size_t* outLen) {
    if (sa == nullptr || outData == nullptr || outLen == nullptr) {
        return E_POINTER;
    }
    *outData = nullptr;
    *outLen = 0;

    // 🔴 남이 넘긴 배열을 믿지 않는다. 차원·원소 타입을 확인한다.
    //    스크립트에서 실수로 2차원 VARIANT 배열을 넘기면 여기서 막아야 한다 —
    //    안 막으면 엉뚱한 바이트를 픽셀로 읽는다.
    if (::SafeArrayGetDim(sa) != 1u) {
        return E_INVALIDARG;
    }
    VARTYPE vt = VT_EMPTY;
    if (FAILED(::SafeArrayGetVartype(sa, &vt)) || vt != VT_UI1) {
        return E_INVALIDARG;
    }
    LONG lb = 0, ub = 0;
    if (FAILED(::SafeArrayGetLBound(sa, 1, &lb)) || FAILED(::SafeArrayGetUBound(sa, 1, &ub))) {
        return E_INVALIDARG;
    }
    if (ub < lb) {
        return E_INVALIDARG;
    }
    const size_t n = static_cast<size_t>(ub - lb) + 1u;

    void* p = nullptr;
    const HRESULT hr = ::SafeArrayAccessData(sa, &p);
    if (FAILED(hr)) {
        return hr;
    }
    *outData = static_cast<const unsigned char*>(p);
    *outLen = n;
    return S_OK;
}

void unlockSafeArray(SAFEARRAY* sa) {
    if (sa != nullptr) {
        ::SafeArrayUnaccessData(sa);
    }
}

} // namespace mari::win::com
