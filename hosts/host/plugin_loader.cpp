// Mari Paint — .8bf 플러그인 로더 구현
#include "plugin_loader.hpp"

#include <cstdlib>
#include <cstring>

namespace mari::host8bf {

/// 지금 처리 중인 공유 메모리 헤더. suite 콜백이 정적으로 봐야 해서 전역이다.
/// 🔴 호스트 프로세스는 한 번에 필터 하나만 돌린다(단일 스레드). 그래서 안전하다.
///    이 전제가 깨지면(스레드 풀 도입 등) 여기가 제일 먼저 터진다.
ShmHeader* g_header = nullptr;

namespace {

u64 qpcNow() noexcept {
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return static_cast<u64>(t.QuadPart);
}

u64 qpcFreq() noexcept {
    static const u64 f = [] {
        LARGE_INTEGER x{};
        return ::QueryPerformanceFrequency(&x) ? static_cast<u64>(x.QuadPart) : 1u;
    }();
    return f;
}

AbortSignal g_abort;

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr,
                                        0);
    if (n <= 0) {
        return {};
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// 모듈에서 PiPL 리소스 한 덩어리를 꺼낸다.
bool readPiplResource(HMODULE mod, std::vector<u8>& out) {
    // PiPL 은 이름 있는 커스텀 리소스다. 이름·타입 둘 다 "PiPL" 인 경우가 표준이고,
    // 번호로 붙은 경우도 있어 열거로 찾는다.
    struct Ctx {
        HMODULE mod;
        std::vector<u8>* out;
        bool found;
    } ctx{mod, &out, false};

    auto enumNames = [](HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR param) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(param);
        HRSRC h = ::FindResourceW(m, name, type);
        if (h == nullptr) {
            return TRUE; // 다음 것
        }
        const DWORD size = ::SizeofResource(m, h);
        HGLOBAL g = ::LoadResource(m, h);
        if (g == nullptr || size == 0u) {
            return TRUE;
        }
        const void* p = ::LockResource(g);
        if (p == nullptr) {
            return TRUE;
        }
        c->out->assign(static_cast<const u8*>(p), static_cast<const u8*>(p) + size);
        c->found = true;
        return FALSE; // 첫 번째 것으로 충분하다
    };

    ::EnumResourceNamesW(mod, L"PiPL", enumNames, reinterpret_cast<LONG_PTR>(&ctx));
    return ctx.found;
}

// ── suite 콜백 ──────────────────────────────────────────────────────────────
//
// 🔴 이게 없으면 대부분의 필터가 즉시 죽는다. 최소 4종을 제공한다.
//    우리가 못 하는 건 **거짓말 대신 실패 코드**로 답한다.

/// 필터가 "취소됐나?" 를 묻는다. 아주 자주 불린다 — 여기가 타임아웃 1차선이다.
ps::Boolean CALLBACK_ABORT() { return g_abort.shouldAbort() ? 1 : 0; }

void CALLBACK_PROGRESS(ps::int32 done, ps::int32 total) {
    // 진행률은 공유 메모리 헤더에 쓴다. 본체가 읽어 UI 에 띄운다.
    if (g_header != nullptr && total > 0) {
        const ps::int32 pct = static_cast<ps::int32>((static_cast<i64>(done) * 100) / total);
        g_header->progress = static_cast<u32>(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
    }
}

ps::OSErr CALLBACK_HOST(ps::int16, ps::int32*) {
    // 호스트 확장. 우리는 아무것도 제공하지 않는다. 정직하게 거절한다.
    return ps::kFilterBadParameters;
}

// ── BufferProcs ─────────────────────────────────────────────────────────────
// 플러그인이 임시 버퍼를 달라고 한다. **크기를 믿지 않고 상한을 건다.**

constexpr ps::int32 kMaxBuffers = 64;
constexpr ps::int32 kMaxBufferBytes = 512 * 1024 * 1024; ///< 512MB. 그 이상은 거절

struct BufferSlot {
    void* p = nullptr;
    ps::int32 size = 0;
};
BufferSlot g_buffers[kMaxBuffers];

ps::OSErr allocateBuffer(ps::int32 size, ps::BufferID* id) {
    if (id == nullptr || size <= 0 || size > kMaxBufferBytes) {
        return ps::kFilterBadParameters;
    }
    for (ps::int32 i = 0; i < kMaxBuffers; ++i) {
        if (g_buffers[i].p == nullptr) {
            void* p = ::VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
            if (p == nullptr) {
                return -108; // memFullErr
            }
            g_buffers[i].p = p;
            g_buffers[i].size = size;
            *id = i + 1; // 0 은 "없음"으로 쓴다
            return ps::kNoErr;
        }
    }
    return -108;
}

BufferSlot* slotOf(ps::BufferID id) {
    if (id <= 0 || id > kMaxBuffers) {
        return nullptr;
    }
    BufferSlot* s = &g_buffers[id - 1];
    return s->p != nullptr ? s : nullptr;
}

ps::Ptr lockBuffer(ps::BufferID id, ps::Boolean) {
    BufferSlot* s = slotOf(id);
    return s != nullptr ? static_cast<ps::Ptr>(s->p) : nullptr;
}
void unlockBuffer(ps::BufferID) {} ///< VirtualAlloc 버퍼는 이미 고정돼 있다
void freeBuffer(ps::BufferID id) {
    BufferSlot* s = slotOf(id);
    if (s != nullptr) {
        ::VirtualFree(s->p, 0, MEM_RELEASE);
        s->p = nullptr;
        s->size = 0;
    }
}
ps::int32 bufferSpace() { return kMaxBufferBytes; }

void freeAllBuffers() {
    for (BufferSlot& s : g_buffers) {
        if (s.p != nullptr) {
            ::VirtualFree(s.p, 0, MEM_RELEASE);
            s.p = nullptr;
            s.size = 0;
        }
    }
}

// ── HandleProcs ─────────────────────────────────────────────────────────────
// Mac 스타일 이중 포인터. 블록 앞에 크기를 숨겨 둔다.

struct HandleBlock {
    ps::int32 size;
    ps::Ptr data; ///< 실제 데이터를 가리킨다. 핸들은 &data 다
};

ps::Handle newHandle(ps::int32 size) {
    if (size < 0 || size > kMaxBufferBytes) {
        return nullptr;
    }
    auto* h = static_cast<HandleBlock*>(::calloc(1, sizeof(HandleBlock)));
    if (h == nullptr) {
        return nullptr;
    }
    h->size = size;
    h->data = size > 0 ? static_cast<ps::Ptr>(::calloc(1, static_cast<size_t>(size))) : nullptr;
    if (size > 0 && h->data == nullptr) {
        ::free(h);
        return nullptr;
    }
    return &h->data;
}

HandleBlock* blockOf(ps::Handle h) {
    if (h == nullptr) {
        return nullptr;
    }
    // data 필드의 주소에서 구조체 시작으로 되돌아간다.
    return reinterpret_cast<HandleBlock*>(reinterpret_cast<char*>(h) -
                                          offsetof(HandleBlock, data));
}

void disposeHandle(ps::Handle h) {
    HandleBlock* b = blockOf(h);
    if (b == nullptr) {
        return;
    }
    ::free(b->data);
    ::free(b);
}
ps::int32 getHandleSize(ps::Handle h) {
    HandleBlock* b = blockOf(h);
    return b != nullptr ? b->size : 0;
}
ps::OSErr setHandleSize(ps::Handle h, ps::int32 newSize) {
    HandleBlock* b = blockOf(h);
    if (b == nullptr || newSize < 0 || newSize > kMaxBufferBytes) {
        return ps::kFilterBadParameters;
    }
    void* p = ::realloc(b->data, static_cast<size_t>(newSize));
    if (newSize > 0 && p == nullptr) {
        return -108;
    }
    b->data = static_cast<ps::Ptr>(p);
    b->size = newSize;
    return ps::kNoErr;
}
ps::Ptr lockHandle(ps::Handle h, ps::Boolean) {
    HandleBlock* b = blockOf(h);
    return b != nullptr ? b->data : nullptr;
}
void unlockHandle(ps::Handle) {}
void recoverSpace(ps::int32) {}

// ── PropertyProcs ───────────────────────────────────────────────────────────
// 우리가 답할 수 있는 것만 답한다. 모르는 건 **거짓말하지 않고** 거절한다.

ps::OSErr getProperty(ps::OSType sig, ps::OSType key, ps::int32, std::intptr_t* simple,
                      ps::Handle* complex) {
    if (sig != ps::kPsSignature) {
        return ps::kErrPlugInPropertyUndefined;
    }
    switch (key) {
    case ps::kPropImageMode:
        if (simple != nullptr) {
            *simple = ps::kPlugInModeRGBColor;
            return ps::kNoErr;
        }
        break;
    case ps::kPropNumberOfChannels:
        if (simple != nullptr && g_header != nullptr) {
            *simple = static_cast<std::intptr_t>(
                channelCountOf(static_cast<ShmPixelFormat>(g_header->format)));
            return ps::kNoErr;
        }
        break;
    case ps::kPropBigNudgeH:
    case ps::kPropBigNudgeV:
        if (simple != nullptr) {
            *simple = 10 << 16; // 16.16 고정소수로 10px
            return ps::kNoErr;
        }
        break;
    case ps::kPropTitle:
        if (complex != nullptr) {
            // 문서 이름을 알려 주지 않는다 — 호스트는 파일 경로를 모른다.
            // 거짓 이름을 만들어 주는 것보다 없다고 하는 게 낫다.
            return ps::kErrPlugInPropertyUndefined;
        }
        break;
    default:
        break;
    }
    return ps::kErrPlugInPropertyUndefined;
}

ps::OSErr setProperty(ps::OSType, ps::OSType, ps::int32, std::intptr_t, ps::Handle) {
    // 플러그인이 우리 문서 속성을 바꾸게 두지 않는다.
    return ps::kErrPlugInPropertyUndefined;
}

// ── ResourceProcs ───────────────────────────────────────────────────────────
// 호스트 리소스(패스·채널)는 없다. **0개라고 정직하게 답한다.**

ps::int32 countResources(ps::OSType) { return 0; }
ps::Handle getResource(ps::OSType, ps::int16) { return nullptr; }
void deleteResource(ps::OSType, ps::int16) {}
ps::OSErr addResource(ps::OSType, ps::Handle) { return ps::kFilterBadParameters; }

} // namespace

bool AbortSignal::shouldAbort() const noexcept {
    if (cancel_.load(std::memory_order_relaxed)) {
        return true;
    }
    const u64 dl = deadline_.load(std::memory_order_relaxed);
    if (dl != 0u && qpcNow() >= dl) {
        timedOut_.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

AbortSignal& abortSignal() noexcept { return g_abort; }

// ── LoadedPlugin ────────────────────────────────────────────────────────────

LoadedPlugin::~LoadedPlugin() { unload(); }

void LoadedPlugin::unload() noexcept {
    if (module_ != nullptr) {
        ::FreeLibrary(module_);
        module_ = nullptr;
    }
    entry_ = nullptr;
}

Result<PluginManifest> LoadedPlugin::openAndScan(const std::string& path) {
    unload();
    const std::wstring wpath = widen(path);
    if (wpath.empty()) {
        return Err("플러그인 경로가 비었거나 잘못됐다", ErrorCode::InvalidArgument);
    }
    // 🔴 DllMain 을 돌리지 않고 리소스만 읽는다.
    HMODULE mod = ::LoadLibraryExW(wpath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (mod == nullptr) {
        return Err("플러그인 파일을 열지 못했다(경로·비트수를 확인해라)", ErrorCode::IoError);
    }

    PluginManifest man;
    man.path = path;

    std::vector<u8> res;
    if (!readPiplResource(mod, res)) {
        ::FreeLibrary(mod);
        return Err("PiPL 리소스가 없다 — .8bf 플러그인이 아니거나 아주 오래된 것이다",
                   ErrorCode::Unsupported);
    }

    auto props = parsePipl(res.data(), res.size(), man.notes);
    ::FreeLibrary(mod);
    if (!props.ok()) {
        return props.error();
    }
    auto entry = piplToEntry(props.value(), man.notes);
    if (!entry.ok()) {
        return entry.error();
    }
    PluginEntry e = entry.value();
    if (e.name.empty()) {
        // 이름이 없으면 파일 이름을 쓴다. 메뉴에 빈칸이 뜨는 것보다 낫다.
        const size_t slash = path.find_last_of("\\/");
        e.name = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    man.entries.push_back(std::move(e));
    return man;
}

Result<void> LoadedPlugin::loadForExecution(const std::string& path,
                                            const std::string& entryName) {
    unload();
    const std::wstring wpath = widen(path);
    if (wpath.empty()) {
        return Err("플러그인 경로가 잘못됐다", ErrorCode::InvalidArgument);
    }
    // 🔴 `LOAD_WITH_ALTERED_SEARCH_PATH`: 동반 DLL 을 **플러그인 폴더**에서 찾게 한다.
    //    이걸 안 주면 플러그인이 우리 폴더의 DLL 을 먼저 집어 이상하게 동작한다.
    module_ = ::LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module_ == nullptr) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_BAD_EXE_FORMAT) {
            return Err("플러그인 비트수가 이 호스트와 다르다", ErrorCode::Unsupported);
        }
        return Err("플러그인을 로드하지 못했다", ErrorCode::IoError);
    }

    entry_ = reinterpret_cast<ps::EntryPointProc>(
        reinterpret_cast<void*>(::GetProcAddress(module_, entryName.c_str())));
    if (entry_ == nullptr) {
        for (const char* n : ps::kFallbackEntryNames) {
            entry_ = reinterpret_cast<ps::EntryPointProc>(
                reinterpret_cast<void*>(::GetProcAddress(module_, n)));
            if (entry_ != nullptr) {
                break;
            }
        }
    }
    if (entry_ == nullptr) {
        unload();
        return Err("플러그인 엔트리포인트를 찾지 못했다", ErrorCode::NotFound);
    }
    return Ok();
}

Result<PluginManifest> scanPlugin(const std::string& path) {
    LoadedPlugin p;
    return p.openAndScan(path);
}

// ── runFilter ───────────────────────────────────────────────────────────────

FilterResult runFilter(const FilterRequest& req) {
    FilterResult out;

    // 1) 공유 메모리를 연다. **본체를 믿지 않는다** — 헤더를 검사한다.
    const std::wstring wname = widen(req.shmName);
    HANDLE map = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
    if (map == nullptr) {
        out.status = HostStatus::ShmError;
        out.notes.push_back("공유 메모리를 열지 못했다: " + req.shmName);
        return out;
    }
    void* base = ::MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(req.shmSize));
    if (base == nullptr) {
        ::CloseHandle(map);
        out.status = HostStatus::ShmError;
        out.notes.push_back("공유 메모리를 매핑하지 못했다(크기가 너무 큰가?)");
        return out;
    }

    auto* header = static_cast<ShmHeader*>(base);
    auto cleanup = [&] {
        g_header = nullptr;
        freeAllBuffers();
        ::UnmapViewOfFile(base);
        ::CloseHandle(map);
    };

    const auto vr = validateHeader(*header, req.shmSize);
    if (!vr.ok()) {
        out.status = HostStatus::ShmError;
        out.notes.push_back(vr.message());
        header->status = static_cast<u32>(out.status);
        cleanup();
        return out;
    }
    g_header = header;

    // 🔴 본체가 우리를 강제 종료할 수 있도록 PID 를 남긴다(docs/02 6.2 타임아웃).
    header->hostPid = static_cast<u32>(::GetCurrentProcessId());
    header->progress = 0;

    // 2) PIPL 을 읽고 아키텍처를 확인한다.
    LoadedPlugin plugin;
    auto man = plugin.openAndScan(req.pluginPath);
    if (!man.ok()) {
        out.status = HostStatus::PluginLoadFailed;
        out.notes.push_back(man.error().message);
        header->status = static_cast<u32>(out.status);
        cleanup();
        return out;
    }
    for (const std::string& n : man.value().notes) {
        out.notes.push_back(n);
    }
    if (man.value().entries.empty()) {
        out.status = HostStatus::PluginLoadFailed;
        out.notes.push_back("플러그인이 필터를 하나도 광고하지 않는다");
        header->status = static_cast<u32>(out.status);
        cleanup();
        return out;
    }

    const PluginEntry* chosen = &man.value().entries.front();
    if (!req.filterName.empty()) {
        for (const PluginEntry& e : man.value().entries) {
            if (e.name == req.filterName) {
                chosen = &e;
                break;
            }
        }
    }
    if (!entryMatchesHost(*chosen, kHost64Bit)) {
        out.status = HostStatus::ArchMismatch;
        out.notes.push_back(std::string("이 플러그인은 ") + (chosen->is64Bit ? "64" : "32") +
                            "비트인데 호스트는 " + (kHost64Bit ? "64" : "32") + "비트다. " +
                            "본체가 맞는 호스트를 띄워야 한다");
        header->status = static_cast<u32>(out.status);
        cleanup();
        return out;
    }

    // 3) FilterRecord 를 세운다.
#if !defined(MARI_8BF_FILTERRECORD_VERIFIED)
    // 🔴 정직하게 실패한다. 추측한 구조체로 남의 네이티브 코드를 부르지 않는다.
    //    자세한 사정은 include/mari/host8bf/photoshop_api.hpp 상단 고지에 있다.
    out.status = HostStatus::PluginLoadFailed;
    out.notes.push_back(
        "FilterRecord 레이아웃이 아직 실물 PIFilter.h 와 대조되지 않았다. "
        "추측한 구조체로 플러그인을 호출하면 메모리가 깨질 수 있어서 막아 두었다. "
        "검증한 뒤 MARI_8BF_FILTERRECORD_VERIFIED 를 정의해서 빌드해라 "
        "(또는 MARI_8BF_USE_ADOBE_SDK 로 진짜 SDK 헤더를 써라).");
    header->status = static_cast<u32>(out.status);
    cleanup();
    return out;
#else
    auto loaded = plugin.loadForExecution(req.pluginPath, chosen->entryName);
    if (!loaded.ok()) {
        out.status = HostStatus::PluginLoadFailed;
        out.notes.push_back(loaded.error().message);
        header->status = static_cast<u32>(out.status);
        cleanup();
        return out;
    }

    ps::BufferProcs bufferProcs{};
    bufferProcs.bufferProcsVersion = ps::kCurrentBufferProcsVersion;
    bufferProcs.numBufferProcs = ps::kCurrentBufferProcsCount;
    bufferProcs.allocateProc = &allocateBuffer;
    bufferProcs.lockProc = &lockBuffer;
    bufferProcs.unlockProc = &unlockBuffer;
    bufferProcs.freeProc = &freeBuffer;
    bufferProcs.spaceProc = &bufferSpace;

    ps::HandleProcs handleProcs{};
    handleProcs.handleProcsVersion = ps::kCurrentHandleProcsVersion;
    handleProcs.numHandleProcs = ps::kCurrentHandleProcsCount;
    handleProcs.newProc = &newHandle;
    handleProcs.disposeProc = &disposeHandle;
    handleProcs.getSizeProc = &getHandleSize;
    handleProcs.setSizeProc = &setHandleSize;
    handleProcs.lockProc = &lockHandle;
    handleProcs.unlockProc = &unlockHandle;
    handleProcs.recoverSpaceProc = &recoverSpace;
    handleProcs.disposeRegularHandleProc = &disposeHandle;

    ps::PropertyProcs propertyProcs{};
    propertyProcs.propertyProcsVersion = ps::kCurrentPropertyProcsVersion;
    propertyProcs.numPropertyProcs = ps::kCurrentPropertyProcsCount;
    propertyProcs.getPropertyProc = &getProperty;
    propertyProcs.setPropertyProc = &setProperty;

    ps::ResourceProcs resourceProcs{};
    resourceProcs.resourceProcsVersion = ps::kCurrentResourceProcsVersion;
    resourceProcs.numResourceProcs = ps::kCurrentResourceProcsCount;
    resourceProcs.countProc = &countResources;
    resourceProcs.getProc = &getResource;
    resourceProcs.deleteProc = &deleteResource;
    resourceProcs.addProc = &addResource;

    ps::FilterRecord fr{};
    fr.serialNumber = 0;
    fr.abortProc = &CALLBACK_ABORT;
    fr.progressProc = &CALLBACK_PROGRESS;
    fr.hostProc = &CALLBACK_HOST;
    fr.hostSig = 0x4D415249u; // 'MARI'
    fr.imageMode = ps::kPlugInModeRGBColor;
    fr.planes = static_cast<ps::int16>(
        channelCountOf(static_cast<ShmPixelFormat>(header->format)));
    fr.imageSize.h = static_cast<ps::int16>(header->width);
    fr.imageSize.v = static_cast<ps::int16>(header->height);
    fr.filterRect.left = static_cast<ps::int16>(header->selLeft);
    fr.filterRect.top = static_cast<ps::int16>(header->selTop);
    fr.filterRect.right = static_cast<ps::int16>(header->selRight);
    fr.filterRect.bottom = static_cast<ps::int16>(header->selBottom);
    fr.maxSpace = kMaxBufferBytes;
    fr.imageHRes = 72 << 16;
    fr.imageVRes = 72 << 16;
    fr.bufferProcs = &bufferProcs;
    fr.handleProcs = &handleProcs;
    fr.propertyProcs = &propertyProcs;
    fr.resourceProcs = &resourceProcs;

    // 🔴 타임아웃 1차선: abortProc 이 마감을 본다.
    const u32 timeout = req.timeoutMs > 0u ? req.timeoutMs : kDefaultTimeoutMs;
    g_abort.setDeadline(qpcNow() + (qpcFreq() * timeout) / 1000u);

    std::intptr_t pluginData = 0;
    ps::int16 result = ps::kNoErr;

    auto call = [&](ps::int16 selector) {
        plugin.entry()(selector, &fr, &pluginData, &result);
    };

    // 4) 규약 순서대로 부른다.
    call(ps::kFilterSelectorParameters);
    if (result == ps::kNoErr) {
        call(ps::kFilterSelectorPrepare);
    }
    if (result == ps::kNoErr) {
        call(ps::kFilterSelectorStart);
    }
    // Continue 는 필터가 inRect 를 비울 때까지 돈다.
    int guard = 0;
    while (result == ps::kNoErr && !g_abort.shouldAbort() &&
           (fr.inRect.right > fr.inRect.left || fr.outRect.right > fr.outRect.left)) {
        // ⚠️ [추정] 타일 왕복(inData/outData 채우기)은 아직 구현하지 않았다.
        //    실물 레이아웃 검증 후에 붙인다. 지금은 무한루프만 막는다.
        if (++guard > 4096) {
            out.notes.push_back("필터가 Continue 를 4096번 넘게 요구했다 — 끊었다");
            break;
        }
        call(ps::kFilterSelectorContinue);
    }
    call(ps::kFilterSelectorFinish);

    // 5) 결과 판정.
    if (g_abort.timedOut()) {
        out.status = HostStatus::TimedOut;
        out.notes.push_back("필터가 " + std::to_string(timeout) + "ms 안에 끝내지 못했다");
    } else if (result == ps::kUserCanceledErr) {
        out.status = HostStatus::Cancelled;
    } else if (result != ps::kNoErr) {
        out.status = HostStatus::PluginRefused;
        out.notes.push_back("필터가 오류 코드 " + std::to_string(result) + " 로 거절했다");
    } else {
        out.status = HostStatus::Ok;
        header->progress = 100;
    }
    header->status = static_cast<u32>(out.status);
    cleanup();
    return out;
#endif // MARI_8BF_FILTERRECORD_VERIFIED
}

} // namespace mari::host8bf
