// Mari Paint — .8bf 격리 호스트 클라이언트 구현 (본체 쪽)
#include "host_client.hpp"

#include <mari/win/com/com_ptr.hpp>

#include <windows.h>

#include <atomic>
#include <thread>

#include "mari.h"

namespace mari::host8bf {
namespace {

using mari::win::com::Bstr;
using mari::win::com::bstrFromUtf8;
using mari::win::com::ComPtr;
using mari::win::com::wideToUtf8;

/// 매핑 이름에 쓸 GUID 문자열. 두 필터가 같은 이름을 쓰면 안 된다.
std::string makeShmName() {
    GUID g{};
    if (FAILED(::CoCreateGuid(&g))) {
        // 실패해도 이름은 필요하다. PID + 틱으로 대체한다(충돌 확률은 충분히 낮다).
        char buf[96];
        ::sprintf_s(buf, "%s%lu-%llu", kShmNamePrefix, ::GetCurrentProcessId(),
                    static_cast<unsigned long long>(::GetTickCount64()));
        return buf;
    }
    wchar_t w[64] = {};
    ::StringFromGUID2(g, w, 64);
    return std::string(kShmNamePrefix) + wideToUtf8(w);
}

/// SAFEARRAY(BSTR) → 문자열 목록.
std::vector<std::string> stringsFromSafeArray(SAFEARRAY* sa) {
    std::vector<std::string> out;
    if (sa == nullptr || ::SafeArrayGetDim(sa) != 1u) {
        return out;
    }
    VARTYPE vt = VT_EMPTY;
    if (FAILED(::SafeArrayGetVartype(sa, &vt)) || vt != VT_BSTR) {
        return out;
    }
    LONG lb = 0, ub = 0;
    if (FAILED(::SafeArrayGetLBound(sa, 1, &lb)) || FAILED(::SafeArrayGetUBound(sa, 1, &ub))) {
        return out;
    }
    for (LONG i = lb; i <= ub; ++i) {
        BSTR b = nullptr;
        if (SUCCEEDED(::SafeArrayGetElement(sa, &i, &b)) && b != nullptr) {
            out.push_back(wideToUtf8(std::wstring(b, ::SysStringLen(b))));
            ::SysFreeString(b);
        }
    }
    return out;
}

/// 호스트를 띄운다.
///
/// 🔴 x86/x64 를 **같은 CLSID 로** 고른다.
///    64비트 Windows 는 32비트 COM 서버를 `Wow6432Node` 레지스트리 뷰에 따로 둔다.
///    `CLSCTX_ACTIVATE_32_BIT_SERVER` / `_64_BIT_SERVER` 가 어느 뷰를 볼지 정한다.
///    그래서 CLSID 를 두 개 만들 필요가 없다 — 이게 Windows 가 의도한 방식이다.
HRESULT createHost(bool want64, ComPtr<IMari8bfHost>& out) {
    const DWORD bits =
        want64 ? CLSCTX_ACTIVATE_64_BIT_SERVER : CLSCTX_ACTIVATE_32_BIT_SERVER;
    return ::CoCreateInstance(CLSID_Mari8bfHost, nullptr, CLSCTX_LOCAL_SERVER | bits,
                              IID_IMari8bfHost, out.putVoid());
}

/// 호스트가 죽었나(살릴 수 없는 RPC 오류인가).
bool isHostDead(HRESULT hr) noexcept {
    return hr == RPC_E_DISCONNECTED || hr == RPC_S_SERVER_UNAVAILABLE ||
           hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == CO_E_OBJNOTCONNECTED ||
           hr == RPC_E_SERVERFAULT;
}

} // namespace

struct Host8bfClient::Impl {
    HANDLE map = nullptr;
    void* base = nullptr;
    u64 mapped = 0;
    ComPtr<IMari8bfHost> host;
    std::atomic<bool> cancelled{false};

    ShmHeader* header() noexcept { return static_cast<ShmHeader*>(base); }

    void closeMap() noexcept {
        if (base != nullptr) {
            ::UnmapViewOfFile(base);
            base = nullptr;
        }
        if (map != nullptr) {
            ::CloseHandle(map);
            map = nullptr;
        }
        mapped = 0;
    }
};

Host8bfClient::~Host8bfClient() {
    if (impl_ != nullptr) {
        impl_->closeMap();
        delete impl_;
        impl_ = nullptr;
    }
}

void Host8bfClient::cancel() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->cancelled.store(true, std::memory_order_relaxed);
    // 공유 메모리 플래그가 1차선(플러그인의 abortProc 이 본다).
    if (ShmHeader* h = impl_->header(); h != nullptr) {
        h->cancel = 1u;
    }
    // COM Cancel() 은 보조다. 호스트가 이미 막혀 있으면 이것도 안 돌아온다.
    if (impl_->host) {
        impl_->host->Cancel();
    }
}

u32 Host8bfClient::progress() const noexcept {
    if (impl_ == nullptr || impl_->base == nullptr) {
        return 0u;
    }
    return static_cast<ShmHeader*>(impl_->base)->progress;
}

Result<std::vector<std::string>> Host8bfClient::enumerate(const std::string& pluginPath,
                                                          bool want64,
                                                          std::vector<std::string>& notes) {
    ComPtr<IMari8bfHost> host;
    HRESULT hr = createHost(want64, host);
    if (FAILED(hr)) {
        return Err("8bf 호스트 프로세스를 띄우지 못했다(등록됐는지 확인해라)",
                   ErrorCode::NotFound);
    }
    const Bstr path = bstrFromUtf8(pluginPath);
    SAFEARRAY* saNotes = nullptr;
    SAFEARRAY* saNames = nullptr;
    hr = host->Enumerate(path.get(), &saNotes, &saNames);

    if (saNotes != nullptr) {
        for (std::string& n : stringsFromSafeArray(saNotes)) {
            notes.push_back(std::move(n));
        }
        ::SafeArrayDestroy(saNotes);
    }
    if (FAILED(hr)) {
        if (saNames != nullptr) {
            ::SafeArrayDestroy(saNames);
        }
        if (isHostDead(hr)) {
            // 🔴 호스트가 죽었다. **우리는 멀쩡하다.** 오류로 보고하고 끝이다.
            return Err("8bf 호스트가 플러그인을 훑다가 죽었다(플러그인이 손상됐을 수 있다)",
                       ErrorCode::IoError);
        }
        return Err("플러그인을 훑지 못했다", ErrorCode::ParseError);
    }
    std::vector<std::string> names = stringsFromSafeArray(saNames);
    if (saNames != nullptr) {
        ::SafeArrayDestroy(saNames);
    }
    return names;
}

Result<ApplyResult> Host8bfClient::apply(const std::string& pluginPath,
                                         const std::string& filterName, PixelView& view,
                                         bool want64, u32 timeoutMs) {
    if (view.pixels == nullptr || view.width == 0u || view.height == 0u) {
        return Err("필터에 넘길 픽셀이 없다", ErrorCode::InvalidArgument);
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->closeMap();
    impl_->cancelled.store(false, std::memory_order_relaxed);

    // 1) 레이아웃을 계산한다. 🔴 오버플로는 여기서 잡힌다.
    auto layout = computeLayout(view.format, view.width, view.height);
    if (!layout.ok()) {
        return layout.error();
    }
    const ShmLayout& L = layout.value();

    // 2) 파일 매핑을 만든다. 🔴 픽셀은 COM 을 타지 않는다(docs/02 6.2).
    const std::string shmName = makeShmName();
    const std::wstring wname = mari::win::com::utf8ToWide(shmName);
    const ULARGE_INTEGER total{{static_cast<DWORD>(L.totalBytes & 0xFFFFFFFFull),
                                static_cast<DWORD>(L.totalBytes >> 32)}};
    impl_->map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, total.HighPart,
                                      total.LowPart, wname.c_str());
    if (impl_->map == nullptr) {
        return Err("공유 메모리를 만들지 못했다", ErrorCode::OutOfMemory);
    }
    impl_->base = ::MapViewOfFile(impl_->map, FILE_MAP_ALL_ACCESS, 0, 0,
                                  static_cast<SIZE_T>(L.totalBytes));
    if (impl_->base == nullptr) {
        impl_->closeMap();
        return Err("공유 메모리를 매핑하지 못했다", ErrorCode::OutOfMemory);
    }
    impl_->mapped = L.totalBytes;

    // 3) 헤더를 채우고 픽셀을 복사한다.
    ShmHeader* h = impl_->header();
    ::memset(h, 0, sizeof(ShmHeader));
    h->magic = kShmMagic;
    h->version = kShmVersion;
    h->format = static_cast<u32>(view.format);
    h->width = view.width;
    h->height = view.height;
    h->rowBytes = L.rowBytes;
    h->pixelOffset = L.pixelOffset;
    h->pixelBytes = L.pixelBytes;
    const Rect sel = view.selection.isEmpty()
                         ? Rect{0, 0, static_cast<i32>(view.width), static_cast<i32>(view.height)}
                         : view.selection;
    h->selLeft = sel.x;
    h->selTop = sel.y;
    h->selRight = sel.right();
    h->selBottom = sel.bottom();

    u8* dst = static_cast<u8*>(impl_->base) + L.pixelOffset;
    const u32 srcStride = view.rowBytes != 0u ? view.rowBytes : L.rowBytes;
    for (u32 y = 0; y < view.height; ++y) {
        ::memcpy(dst + static_cast<usize>(y) * L.rowBytes,
                 view.pixels + static_cast<usize>(y) * srcStride, L.rowBytes);
    }

    // 4) 호스트를 띄우고 Apply 를 별도 스레드에서 부른다.
    //    🔴 Apply 는 블로킹이다. 감시자가 따로 있어야 타임아웃을 걸 수 있다.
    HRESULT hrCreate = createHost(want64, impl_->host);
    if (FAILED(hrCreate)) {
        impl_->closeMap();
        return Err("8bf 호스트 프로세스를 띄우지 못했다(등록됐는지 확인해라)",
                   ErrorCode::NotFound);
    }

    ApplyResult result;
    HRESULT hrApply = E_FAIL;
    MariHostStatus status = mariHostLoadFailed;
    SAFEARRAY* saNotes = nullptr;

    // 호스트 인터페이스를 작업 스레드로 마샬링해 넘긴다.
    IStream* stm = nullptr;
    if (FAILED(::CoMarshalInterThreadInterfaceInStream(IID_IMari8bfHost, impl_->host.get(),
                                                       &stm))) {
        impl_->closeMap();
        return Err("호스트 인터페이스를 작업 스레드로 넘기지 못했다", ErrorCode::Unknown);
    }

    std::atomic<bool> done{false};
    const Bstr bPath = bstrFromUtf8(pluginPath);
    const Bstr bName = bstrFromUtf8(filterName);
    const Bstr bShm = bstrFromUtf8(shmName);

    std::thread worker([&] {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMari8bfHost> h2;
        if (SUCCEEDED(::CoGetInterfaceAndReleaseStream(stm, IID_IMari8bfHost, h2.putVoid()))) {
            hrApply = h2->Apply(bPath.get(), bName.get(), bShm.get(),
                                static_cast<LONGLONG>(L.totalBytes),
                                static_cast<LONG>(timeoutMs), &saNotes, &status);
        }
        done.store(true, std::memory_order_release);
        ::CoUninitialize();
    });

    // 5) 감시. 🔴 타임아웃 2차선: 우리가 직접 죽인다.
    //    호스트가 abortProc 을 안 부르는 필터에 붙잡혀 있으면 이 길밖에 없다.
    const u32 grace = 2000u; ///< cancel 플래그를 본 뒤 스스로 접을 시간
    const ULONGLONG start = ::GetTickCount64();
    bool killed = false;
    bool askedCancel = false;

    while (!done.load(std::memory_order_acquire)) {
        ::Sleep(20);
        const ULONGLONG elapsed = ::GetTickCount64() - start;

        if (impl_->cancelled.load(std::memory_order_relaxed) && !askedCancel) {
            h->cancel = 1u;
            askedCancel = true;
        }
        if (elapsed > timeoutMs && !askedCancel) {
            h->cancel = 1u; // 먼저 점잖게 부탁한다
            askedCancel = true;
        }
        if (elapsed > static_cast<ULONGLONG>(timeoutMs) + grace) {
            // 안 듣는다. 죽인다.
            const u32 pid = h->hostPid;
            if (pid != 0u) {
                HANDLE proc = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (proc != nullptr) {
                    ::TerminateProcess(proc, 1);
                    ::CloseHandle(proc);
                    killed = true;
                }
            }
            // 죽였으면 Apply 는 RPC 오류로 곧 돌아온다. 그래도 안 돌아오면
            // 아래 join 이 막히므로, 최대 한 번 더 기다리고 포기한다.
            if (elapsed > static_cast<ULONGLONG>(timeoutMs) + grace * 5u) {
                break;
            }
        }
    }

    if (worker.joinable()) {
        worker.join();
    }

    // 6) 결과 판정.
    if (saNotes != nullptr) {
        result.notes = stringsFromSafeArray(saNotes);
        ::SafeArrayDestroy(saNotes);
    }
    result.hostKilled = killed;

    if (killed) {
        result.status = HostStatus::TimedOut;
        result.notes.push_back("플러그인이 " + std::to_string(timeoutMs) +
                               "ms 안에 끝내지 않아 호스트 프로세스를 강제 종료했다. "
                               "픽셀은 원본 그대로 두었다.");
    } else if (FAILED(hrApply)) {
        if (isHostDead(hrApply)) {
            // 🔴 여기가 "호스트가 죽어도 본체는 산다" 가 실제로 일어나는 지점이다.
            result.status = HostStatus::PluginCrashed;
            result.notes.push_back("플러그인이 호스트 프로세스와 함께 죽었다. "
                                   "본체는 멀쩡하고 픽셀은 원본 그대로다.");
        } else {
            result.status = HostStatus::ShmError;
            result.notes.push_back("호스트 호출이 실패했다");
        }
    } else {
        result.status = static_cast<HostStatus>(status);
    }

    // 7) 성공했을 때만 결과를 되가져온다.
    //    🔴 실패하면 원본을 건드리지 않는다. 반쯤 처리된 픽셀을 캔버스에 넣지 않는다.
    if (result.status == HostStatus::Ok) {
        const u8* src = static_cast<const u8*>(impl_->base) + L.pixelOffset;
        for (u32 y = 0; y < view.height; ++y) {
            ::memcpy(view.pixels + static_cast<usize>(y) * srcStride,
                     src + static_cast<usize>(y) * L.rowBytes, L.rowBytes);
        }
    }

    impl_->host.reset(); // 호스트를 놓아 준다. 다음 필터는 새 프로세스에서
    impl_->closeMap();
    return result;
}

Result<bool> pluginIs64Bit(const std::string& path) {
    const std::wstring wpath = mari::win::com::utf8ToWide(path);
    HANDLE f = ::CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return Err("플러그인 파일을 열지 못했다", ErrorCode::NotFound);
    }
    struct Closer {
        HANDLE h;
        ~Closer() { ::CloseHandle(h); }
    } closer{f};

    // 🔴 PE 헤더만 읽는다. 매핑도 실행도 하지 않는다 — 코드를 돌리지 않는다.
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!::ReadFile(f, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return Err("PE 파일이 아니다", ErrorCode::ParseError);
    }
    if (dos.e_lfanew <= 0 || dos.e_lfanew > (16 << 20)) {
        return Err("PE 헤더 오프셋이 말이 안 된다", ErrorCode::ParseError);
    }
    if (::SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        return Err("PE 헤더로 이동하지 못했다", ErrorCode::IoError);
    }
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (!::ReadFile(f, &sig, sizeof(sig), &read, nullptr) || read != sizeof(sig) ||
        sig != IMAGE_NT_SIGNATURE) {
        return Err("PE 서명이 없다", ErrorCode::ParseError);
    }
    if (!::ReadFile(f, &fh, sizeof(fh), &read, nullptr) || read != sizeof(fh)) {
        return Err("PE 파일 헤더를 읽지 못했다", ErrorCode::ParseError);
    }
    switch (fh.Machine) {
    case IMAGE_FILE_MACHINE_AMD64:
    case IMAGE_FILE_MACHINE_ARM64:
        return true;
    case IMAGE_FILE_MACHINE_I386:
    case IMAGE_FILE_MACHINE_ARMNT:
        return false;
    default:
        return Err("모르는 플러그인 아키텍처다", ErrorCode::Unsupported);
    }
}

} // namespace mari::host8bf
