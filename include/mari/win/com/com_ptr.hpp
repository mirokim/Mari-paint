// Mari Paint — 최소 COM 스마트 포인터 + BSTR 래퍼
//
// ATL·_com_ptr_t 를 쓰지 않는 이유: ATL 은 Visual Studio 의 특정 워크로드에만 들어 있고,
// `#import` 기반 `_com_ptr_t` 는 MinGW 에서 안 돈다. Mari 는 MSVC/clang-cl 둘 다로
// 빌드되어야 하므로 필요한 만큼만 직접 만든다. 100줄이면 충분하다.
//
// ⚠️ 이 헤더는 Windows 전용이다. Linux 빌드에서는 컴파일되지 않는다
//    (platform/win/CMakeLists.txt 가 if(WIN32) 로 감싸고 있다).
#ifndef MARI_WIN_COM_COM_PTR_HPP
#define MARI_WIN_COM_COM_PTR_HPP

#if !defined(_WIN32)
#error "mari/win/com/com_ptr.hpp 는 Windows 전용이다"
#endif

#include <objbase.h>
#include <oleauto.h>

#include <string>
#include <utility>

namespace mari::win::com {

/// 참조 카운트를 대신 세 주는 포인터. COM 규칙 그대로다.
template <class T> class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}

    /// 이미 AddRef 된 포인터를 **넘겨받는다**(AddRef 하지 않는다).
    static ComPtr attach(T* p) noexcept {
        ComPtr c;
        c.p_ = p;
        return c;
    }

    /// 남의 포인터를 **공유한다**(AddRef 한다).
    explicit ComPtr(T* p) noexcept : p_(p) {
        if (p_ != nullptr) {
            p_->AddRef();
        }
    }

    ComPtr(const ComPtr& o) noexcept : p_(o.p_) {
        if (p_ != nullptr) {
            p_->AddRef();
        }
    }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& o) noexcept {
        if (this != &o) {
            ComPtr tmp(o);
            swap(tmp);
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        T* p = p_;
        p_ = nullptr;
        if (p != nullptr) {
            p->Release();
        }
    }
    void swap(ComPtr& o) noexcept { std::swap(p_, o.p_); }

    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    /// `IID_PPV_ARGS` 처럼 out 파라미터로 넘길 때. **기존 값을 버린다.**
    T** put() noexcept {
        reset();
        return &p_;
    }
    void** putVoid() noexcept { return reinterpret_cast<void**>(put()); }

    /// 소유권을 호출자에게 넘긴다(Release 하지 않는다). [out, retval] 채울 때 쓴다.
    [[nodiscard]] T* detach() noexcept {
        T* p = p_;
        p_ = nullptr;
        return p;
    }

    /// 다른 인터페이스로 갈아탄다.
    template <class U> HRESULT as(ComPtr<U>& out) const noexcept {
        if (p_ == nullptr) {
            return E_POINTER;
        }
        return p_->QueryInterface(__uuidof(U), out.putVoid());
    }

private:
    T* p_ = nullptr;
};

/// BSTR 을 들고 있다가 알아서 푸는 래퍼. **SysFreeString 을 잊는 것이 COM 누수 1위다.**
class Bstr {
public:
    Bstr() noexcept = default;
    explicit Bstr(const wchar_t* s) : b_(s != nullptr ? ::SysAllocString(s) : nullptr) {}
    explicit Bstr(const std::wstring& s)
        : b_(::SysAllocStringLen(s.c_str(), static_cast<UINT>(s.size()))) {}

    Bstr(const Bstr& o) : b_(o.b_ != nullptr ? ::SysAllocStringLen(o.b_, ::SysStringLen(o.b_))
                                             : nullptr) {}
    Bstr(Bstr&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }

    ~Bstr() {
        if (b_ != nullptr) {
            ::SysFreeString(b_);
        }
    }

    Bstr& operator=(const Bstr& o) {
        if (this != &o) {
            Bstr tmp(o);
            std::swap(b_, tmp.b_);
        }
        return *this;
    }
    Bstr& operator=(Bstr&& o) noexcept {
        if (this != &o) {
            if (b_ != nullptr) {
                ::SysFreeString(b_);
            }
            b_ = o.b_;
            o.b_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] BSTR get() const noexcept { return b_; }
    [[nodiscard]] BSTR detach() noexcept {
        BSTR b = b_;
        b_ = nullptr;
        return b;
    }
    [[nodiscard]] bool empty() const noexcept { return b_ == nullptr || ::SysStringLen(b_) == 0u; }
    [[nodiscard]] std::wstring str() const {
        return b_ != nullptr ? std::wstring(b_, ::SysStringLen(b_)) : std::wstring();
    }

private:
    BSTR b_ = nullptr;
};

/// UTF-8 → UTF-16. COM 은 전부 UTF-16 이고 우리 코어는 UTF-8 이다. 경계에서 한 번만 바꾼다.
[[nodiscard]] std::wstring utf8ToWide(const std::string& s);
/// UTF-16 → UTF-8.
[[nodiscard]] std::string wideToUtf8(const std::wstring& s);
/// UTF-8 문자열을 BSTR 로. 실패하면 빈 Bstr.
[[nodiscard]] inline Bstr bstrFromUtf8(const std::string& s) { return Bstr(utf8ToWide(s)); }

/// `[out, retval] BSTR*` 을 안전하게 채운다. **null 포인터 검사를 빼먹지 마라** —
/// 스크립트 언어가 넘기는 포인터는 언제든 null 일 수 있다.
[[nodiscard]] inline HRESULT returnBstr(BSTR* out, const std::string& utf8) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = bstrFromUtf8(utf8).detach();
    return *out != nullptr || utf8.empty() ? S_OK : E_OUTOFMEMORY;
}

/// `[out, retval] LONG*` 등 간단한 스칼라용.
template <class T> [[nodiscard]] inline HRESULT returnScalar(T* out, T v) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = v;
    return S_OK;
}

/// 바이트 배열을 SAFEARRAY(BYTE) 로 만든다. 픽셀 내보내기에 쓴다.
///
/// ⚠️ 큰 픽셀은 이 경로로 보내지 마라. 8bf 는 공유 메모리를 쓴다(docs/02 6.2).
///    여기는 스크립트가 작은 레이어를 뽑아 갈 때 쓰는 편의 경로다.
[[nodiscard]] SAFEARRAY* safeArrayFromBytes(const unsigned char* data, size_t len);

/// SAFEARRAY(BYTE) 에서 바이트를 읽는다. 잠금·해제를 책임진다.
/// 1차원·VT_UI1 이 아니면 실패한다 — **남이 넘긴 배열을 믿지 않는다.**
[[nodiscard]] HRESULT bytesFromSafeArray(SAFEARRAY* sa, const unsigned char** outData,
                                         size_t* outLen);

/// bytesFromSafeArray 로 잠근 배열을 푼다.
void unlockSafeArray(SAFEARRAY* sa);

} // namespace mari::win::com

#endif // MARI_WIN_COM_COM_PTR_HPP
