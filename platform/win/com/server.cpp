// Mari Paint — 클래스 팩토리 + LocalServer 생명 주기
//
// 🔴 이 파일에 DllGetClassObject / DllCanUnloadNow 가 없다는 게 중요하다.
//    LocalServer32 는 DLL 이 아니다. 프로세스 분리가 설계의 핵심이다(docs/02 6.3).
#include <mari/win/com/server.hpp>

#include <mari/win/com/dual_base.hpp>

#include <windows.h>

#include <new>
#include <vector>

namespace mari::win::com {
namespace {

struct Registered {
    CLSID clsid{};
    CreateInstanceFn fn = nullptr;
    DWORD cookie = 0;
};

std::vector<Registered>& registry() {
    static std::vector<Registered> r;
    return r;
}

DWORD g_mainThreadId = 0;

void quitMainThread() {
    if (g_mainThreadId != 0) {
        ::PostThreadMessageW(g_mainThreadId, WM_QUIT, 0, 0);
    }
}

/// 코클래스 하나짜리 표준 클래스 팩토리.
class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(CreateInstanceFn fn) noexcept : fn_(fn) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
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

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        // 집합(aggregation)은 지원하지 않는다. 표준 거절 코드로 정직하게 답한다.
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        if (fn_ == nullptr) {
            return E_UNEXPECTED;
        }
        return fn_(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        ServerLock::lockServer(lock != FALSE);
        return S_OK;
    }

private:
    ~ClassFactory() = default;
    LONG refs_ = 1;
    CreateInstanceFn fn_;
};

} // namespace

void addClassObject(const CLSID& clsid, CreateInstanceFn fn) {
    Registered r;
    r.clsid = clsid;
    r.fn = fn;
    registry().push_back(r);
}

void installQuitOnIdle(DWORD mainThreadId) noexcept {
    g_mainThreadId = mainThreadId;
    ServerLock::setQuitHandler(&quitMainThread);
}

mari::Result<void> registerClassObjects() {
    for (Registered& r : registry()) {
        auto* cf = new (std::nothrow) ClassFactory(r.fn);
        if (cf == nullptr) {
            revokeClassObjects();
            return mari::Err("클래스 팩토리를 만들지 못했다", mari::ErrorCode::OutOfMemory);
        }
        // 🔴 REGCLS_MULTIPLEUSE: CreateObject 를 여러 번 불러도 프로세스는 하나다.
        //    REGCLS_SINGLEUSE 로 두면 스크립트가 Mari 를 여러 개 띄운다.
        const HRESULT hr = ::CoRegisterClassObject(r.clsid, static_cast<IClassFactory*>(cf),
                                                   CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE,
                                                   &r.cookie);
        cf->Release();
        if (FAILED(hr)) {
            revokeClassObjects();
            return mari::Err("클래스 객체를 등록하지 못했다(이미 떠 있는 인스턴스가 있나?)",
                             mari::ErrorCode::IoError);
        }
    }
    // 등록이 끝났다고 알린다. 이게 없으면 클라이언트가 최대 타임아웃까지 기다린다.
    ::CoResumeClassObjects();
    return mari::Ok();
}

void revokeClassObjects() noexcept {
    ::CoSuspendClassObjects();
    for (Registered& r : registry()) {
        if (r.cookie != 0) {
            ::CoRevokeClassObject(r.cookie);
            r.cookie = 0;
        }
    }
}

} // namespace mari::win::com
