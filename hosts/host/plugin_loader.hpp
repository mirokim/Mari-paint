// Mari Paint — .8bf 플러그인 로더 (호스트 프로세스 안에서만 돈다)
//
// 🔴 이 헤더는 **호스트 실행파일 전용**이다. 본체(mari-paint.exe)에 링크되면 안 된다.
//    본체가 LoadLibrary("plugin.8bf") 를 부르는 순간 설계 원칙 3이 깨진다
//    ("남의 코드는 내 프로세스에 들이지 않는다").
//
// 여기서 죽는 건 괜찮다. 이 프로세스가 죽어도 본체는 산다 — 그게 이 프로세스의 존재 이유다.
//
// ⚠️ Windows 전용.
#ifndef MARI_HOST8BF_PLUGIN_LOADER_HPP
#define MARI_HOST8BF_PLUGIN_LOADER_HPP

#if !defined(_WIN32)
#error "plugin_loader.hpp 는 Windows 전용이다"
#endif

#include <mari/core/result.hpp>
#include <mari/host8bf/photoshop_api.hpp>
#include <mari/host8bf/pipl.hpp>
#include <mari/host8bf/protocol.hpp>

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace mari::host8bf {

/// 이 호스트가 64비트인가. 컴파일 타임에 정해진다.
inline constexpr bool kHost64Bit = sizeof(void*) == 8;

/// 로드한 플러그인 하나.
class LoadedPlugin {
public:
    LoadedPlugin() noexcept = default;
    ~LoadedPlugin();

    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    /// .8bf 를 연다. **아직 코드를 실행하지 않는다** — PIPL 만 읽는다.
    ///
    /// 🔴 `LOAD_LIBRARY_AS_DATAFILE` 로 먼저 열어 리소스만 읽는다. 그래야 DllMain 이
    ///    안 돈다. 악의적·버그 있는 플러그인이 로드만으로 뭔가 하는 걸 막는다.
    ///    실제로 실행할 때만 `LOAD_WITH_ALTERED_SEARCH_PATH` 로 다시 연다
    ///    (플러그인 폴더의 동반 DLL 을 찾게 해 주되, 우리 폴더를 먼저 뒤지지 않게).
    [[nodiscard]] Result<PluginManifest> openAndScan(const std::string& path);

    /// 실행 준비. 여기서 처음으로 남의 코드가 우리 프로세스에 들어온다.
    [[nodiscard]] Result<void> loadForExecution(const std::string& path,
                                                const std::string& entryName);

    [[nodiscard]] ps::EntryPointProc entry() const noexcept { return entry_; }
    void unload() noexcept;

private:
    HMODULE module_ = nullptr;
    ps::EntryPointProc entry_ = nullptr;
};

/// 필터 한 번 실행에 필요한 것 전부.
struct FilterRequest {
    std::string pluginPath;
    std::string filterName;  ///< 빈 문자열이면 첫 번째 항목
    std::string shmName;     ///< 파일 매핑 이름
    u64 shmSize = 0;
    u32 timeoutMs = kDefaultTimeoutMs;
};

/// 실행 결과.
struct FilterResult {
    HostStatus status = HostStatus::PluginLoadFailed;
    /// 🔴 못 한 것·모르는 것을 전부 여기 남긴다. 조용히 넘어가지 않는다.
    std::vector<std::string> notes;
};

/// 취소·타임아웃 신호. 필터 콜백(abortProc)이 매번 본다.
///
/// 🔴 이게 무한루프 방어의 **1차선**이다. 잘 만든 필터는 abortProc 을 자주 부르고,
///    우리가 true 를 주면 스스로 멈춘다. 안 부르는 필터를 위해 본체가
///    TerminateProcess 로 2차선을 쥐고 있다(docs/02 6.2).
class AbortSignal {
public:
    void requestCancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }
    /// 마감 시각을 세운다(단조 틱).
    void setDeadline(u64 qpcDeadline) noexcept {
        deadline_.store(qpcDeadline, std::memory_order_relaxed);
    }
    /// 취소됐거나 마감을 넘겼나. abortProc 이 부른다 — **아주 자주 불린다.**
    [[nodiscard]] bool shouldAbort() const noexcept;
    [[nodiscard]] bool timedOut() const noexcept { return timedOut_.load(std::memory_order_relaxed); }

private:
    mutable std::atomic<bool> timedOut_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<u64> deadline_{0};
};

/// 이 호스트 프로세스에 하나뿐인 취소 신호(콜백이 정적으로 접근해야 한다).
[[nodiscard]] AbortSignal& abortSignal() noexcept;

/// 공유 메모리에 있는 픽셀에 필터 하나를 건다.
///
/// 하는 일 순서:
///   1. 매핑을 연다 → `validateHeader()` 로 **본체를 믿지 않고** 검사한다
///   2. PIPL 을 읽어 필터를 고른다(아키텍처가 안 맞으면 ArchMismatch)
///   3. `FilterRecord` 와 suite 콜백을 세운다
///   4. About/Parameters → Prepare → Start → Continue* → Finish 순서로 부른다
///   5. 결과를 공유 메모리에 되쓰고 상태를 적는다
///
/// ⚠️ `MARI_8BF_FILTERRECORD_VERIFIED` 가 정의되지 않았으면 3번에서 멈추고
///    `PluginLoadFailed` + "FilterRecord 레이아웃 미검증" note 를 돌려준다.
///    추측한 구조체로 남의 코드를 부르느니 **정직하게 실패한다**
///    (photoshop_api.hpp 상단 고지 참조).
[[nodiscard]] FilterResult runFilter(const FilterRequest& req);

/// 플러그인을 훑어 필터 목록만 낸다(메뉴 구성용). 코드를 실행하지 않는다.
[[nodiscard]] Result<PluginManifest> scanPlugin(const std::string& path);

} // namespace mari::host8bf

#endif // MARI_HOST8BF_PLUGIN_LOADER_HPP
