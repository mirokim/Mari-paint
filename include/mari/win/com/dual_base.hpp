// Mari Paint — dual 인터페이스 구현 베이스 (IUnknown + IDispatch)
//
// 🔴 왜 dual 인가 (docs/02 6.1)
//    vtable 만 있으면 C++/C# 만 붙는다. IDispatch 가 있어야 Python(pywin32)·VBA·
//    PowerShell·AutoHotkey 가 붙는다. 그게 "다양한 프로그램과 호환"의 실체다.
//
// 🔴 IDispatch 를 손으로 구현하지 않는다.
//    타입 라이브러리(mari.tlb)를 로드해서 `ITypeInfo::Invoke` 에 넘긴다. 그러면
//    IDL 에 메서드를 추가할 때 디스패치 테이블을 따로 손볼 필요가 없다 —
//    **두 곳을 고치다 한 곳을 빠뜨리는 버그가 원천적으로 안 생긴다.**
//
// ⚠️ Windows 전용.
#ifndef MARI_WIN_COM_DUAL_BASE_HPP
#define MARI_WIN_COM_DUAL_BASE_HPP

#if !defined(_WIN32)
#error "mari/win/com/dual_base.hpp 는 Windows 전용이다"
#endif

#include <mari/core/result.hpp>
#include <mari/win/com/com_ptr.hpp>

namespace mari::win::com {

/// 서버가 살아 있어야 하는 이유의 개수(객체 + 락). 0 이 되면 LocalServer 가 종료한다.
/// 🔴 LocalServer32 의 생명 주기 관리다. 이걸 틀리면 **스크립트가 끝나도 프로세스가
///    안 죽거나, 스크립트가 쓰는 중에 프로세스가 죽는다.**
class ServerLock {
public:
    static void addObject() noexcept;
    static void releaseObject() noexcept;
    static void lockServer(bool lock) noexcept;
    [[nodiscard]] static long count() noexcept;
    /// 마지막 참조가 사라지면 불린다. 메시지 루프에 WM_QUIT 을 던진다.
    static void setQuitHandler(void (*fn)()) noexcept;
};

/// 우리 타입 라이브러리에서 인터페이스 하나의 ITypeInfo 를 얻는다.
/// 처음 한 번만 로드하고 캐시한다. 실패하면 IDispatch 쪽만 죽고 vtable 은 산다 —
/// **타입 라이브러리가 깨져도 C++/C# 클라이언트는 계속 돈다.**
[[nodiscard]] HRESULT typeInfoFor(REFIID iid, ITypeInfo** out);

/// 타입 라이브러리를 미리 로드한다(서버 시작 시 한 번). 실패해도 치명적이지 않다.
HRESULT preloadTypeLib();
/// 캐시를 비운다(서버 종료 시).
void releaseTypeLibCache() noexcept;

/// dual 인터페이스 하나를 구현하는 객체의 베이스.
///
///     class MariApplication final : public DualBase<IMariApplication, &IID_IMariApplication> {
///         // IMariApplication 메서드만 구현하면 된다
///     };
///
/// `Derived` 가 인터페이스를 더 내놓아야 하면 `queryExtra()` 를 오버라이드한다.
template <class TInterface, const IID* kIid> class DualBase : public TInterface {
public:
    DualBase() noexcept { ServerLock::addObject(); }

    DualBase(const DualBase&) = delete;
    DualBase& operator=(const DualBase&) = delete;

    // ── IUnknown ────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(static_cast<TInterface*>(this));
        } else if (riid == IID_IDispatch) {
            *ppv = static_cast<IDispatch*>(static_cast<TInterface*>(this));
        } else if (riid == *kIid) {
            *ppv = static_cast<TInterface*>(this);
        } else {
            return queryExtra(riid, ppv);
        }
        static_cast<IUnknown*>(*ppv)->AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = ::InterlockedDecrement(&refs_);
        if (n == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(n);
    }

    // ── IDispatch — 전부 타입 라이브러리에 위임한다 ──────────────────────
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override {
        if (pctinfo == nullptr) {
            return E_POINTER;
        }
        *pctinfo = 1u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID, ITypeInfo** ppTInfo) override {
        if (ppTInfo == nullptr) {
            return E_POINTER;
        }
        *ppTInfo = nullptr;
        if (iTInfo != 0u) {
            return DISP_E_BADINDEX;
        }
        return typeInfoFor(*kIid, ppTInfo);
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* rgszNames, UINT cNames, LCID,
                                            DISPID* rgDispId) override {
        ComPtr<ITypeInfo> ti;
        const HRESULT hr = typeInfoFor(*kIid, ti.put());
        if (FAILED(hr)) {
            return hr;
        }
        return ::DispGetIDsOfNames(ti.get(), rgszNames, cNames, rgDispId);
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID, LCID, WORD wFlags,
                                     DISPPARAMS* pDispParams, VARIANT* pVarResult,
                                     EXCEPINFO* pExcepInfo, UINT* puArgErr) override {
        ComPtr<ITypeInfo> ti;
        const HRESULT hr = typeInfoFor(*kIid, ti.put());
        if (FAILED(hr)) {
            return hr;
        }
        // this 를 인터페이스 포인터로 정확히 캐스팅해서 넘긴다. void* 로 넘기면
        // 다중 상속에서 vtable 이 어긋난다.
        return ::DispInvoke(static_cast<TInterface*>(this), ti.get(), dispIdMember, wFlags,
                            pDispParams, pVarResult, pExcepInfo, puArgErr);
    }

protected:
    virtual ~DualBase() { ServerLock::releaseObject(); }

    /// 파생 클래스가 인터페이스를 더 내놓고 싶으면 오버라이드한다.
    virtual HRESULT queryExtra(REFIID, void** ppv) {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

private:
    LONG refs_ = 1;
};

/// 오류를 스크립트가 읽을 수 있는 메시지로 올린다.
///
/// 🔴 이게 중요한 이유: Python 에서 `doc.SaveAs(...)` 가 실패했을 때
///    `COMError(-2147467259, 'Unspecified error')` 만 뜨면 아무도 못 고친다.
///    `ICreateErrorInfo` 로 한국어 메시지를 실어 보내면 pywin32 가 그대로 보여 준다.
HRESULT setErrorInfo(REFIID iid, const std::string& utf8Message, HRESULT hr);

/// core 의 `mari::Error` 를 HRESULT 로 옮긴다. 메시지도 같이 싣는다.
/// ErrorCode → HRESULT 대응:
///   InvalidArgument→E_INVALIDARG · NotFound→HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
///   IoError→HRESULT_FROM_WIN32(ERROR_IO_DEVICE) · ParseError→E_UNEXPECTED
///   Unsupported→E_NOTIMPL · OutOfMemory→E_OUTOFMEMORY · Cancelled→E_ABORT
HRESULT hresultFromError(REFIID iid, const mari::Error& e);

} // namespace mari::win::com

#endif // MARI_WIN_COM_DUAL_BASE_HPP
