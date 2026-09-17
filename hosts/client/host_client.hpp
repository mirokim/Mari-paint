// Mari Paint — .8bf 격리 호스트 클라이언트 (본체 쪽)
//
// ════════════════════════════════════════════════════════════════════════════
// 🔴 "호스트가 죽어도 본체는 산다" 를 **코드 구조로** 보이는 파일이다.
//
//    1. 이 헤더 어디에도 `LoadLibrary("*.8bf")` 가 없다. 있을 수도 없다 —
//       플러그인을 여는 코드는 hosts/host/ 안에만 있고, 그건 별도 실행파일이다.
//    2. 호스트와의 통신은 COM 뿐이다. 호스트가 죽으면 `RPC_S_SERVER_UNAVAILABLE`
//       이 돌아온다. 그건 **정상적으로 처리하는 오류 코드**지 크래시가 아니다.
//    3. 픽셀은 파일 매핑에 있다. 호스트가 죽어도 매핑은 우리 것이라 멀쩡하다.
//       원본을 따로 들고 있다가 실패하면 되돌린다.
//    4. 타임아웃이 지나면 `TerminateProcess` 로 **우리가 죽인다.**
// ════════════════════════════════════════════════════════════════════════════
//
// ⚠️ Windows 전용.
#ifndef MARI_HOST8BF_HOST_CLIENT_HPP
#define MARI_HOST8BF_HOST_CLIENT_HPP

#if !defined(_WIN32)
#error "host_client.hpp 는 Windows 전용이다"
#endif

#include <mari/core/result.hpp>
#include <mari/host8bf/protocol.hpp>

#include <string>
#include <vector>

namespace mari::host8bf {

/// 호스트에 넘길 픽셀 한 판.
struct PixelView {
    ShmPixelFormat format = ShmPixelFormat::RGBA8;
    u32 width = 0;
    u32 height = 0;
    /// 원본 픽셀. 호스트가 이걸 읽고 같은 자리에 결과를 쓴다.
    u8* pixels = nullptr;
    u32 rowBytes = 0;
    /// 필터를 걸 영역(이 버퍼 기준, 반열림). 비우면 전체.
    Rect selection{};
};

/// 필터 한 번 실행 결과.
struct ApplyResult {
    HostStatus status = HostStatus::PluginLoadFailed;
    /// 🔴 못 한 것을 전부 담는다. 조용히 넘어가지 않는다(docs/02 5절).
    std::vector<std::string> notes;
    /// 호스트가 죽어서 우리가 강제 종료했나.
    bool hostKilled = false;
};

/// 격리 호스트와 이야기하는 쪽. 본체가 소유한다.
///
/// 수명: 필터 한 번마다 호스트를 새로 띄우고 끝나면 놓는다.
///       호스트를 계속 살려 두면 이전 필터가 남긴 쓰레기가 다음 필터에 영향을 준다.
class Host8bfClient {
public:
    Host8bfClient() noexcept = default;
    ~Host8bfClient();

    Host8bfClient(const Host8bfClient&) = delete;
    Host8bfClient& operator=(const Host8bfClient&) = delete;

    /// 플러그인이 광고하는 필터 목록을 읽는다(메뉴 구성).
    /// `want64` 는 호스트 비트수 — 플러그인 아키텍처에 맞춰 본체가 정한다.
    [[nodiscard]] Result<std::vector<std::string>> enumerate(const std::string& pluginPath,
                                                             bool want64,
                                                             std::vector<std::string>& notes);

    /// 필터를 건다.
    ///
    /// 🔴 실패해도 `view.pixels` 는 **원본 그대로**다. 부분적으로 망가진 결과를
    ///    캔버스에 밀어 넣지 않는다. 그게 실행취소보다 먼저 지켜야 할 것이다.
    [[nodiscard]] Result<ApplyResult> apply(const std::string& pluginPath,
                                            const std::string& filterName, PixelView& view,
                                            bool want64, u32 timeoutMs = kDefaultTimeoutMs);

    /// 진행 중인 필터를 취소한다. 다른 스레드에서 불러도 된다.
    void cancel() noexcept;

    /// 0..100.
    [[nodiscard]] u32 progress() const noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/// 플러그인 파일이 32비트인지 64비트인지 본다(PE 헤더의 Machine 필드).
///
/// 🔴 이걸 먼저 봐야 어느 호스트를 띄울지 안다. 틀리면 LoadLibrary 가 실패하거나,
///    더 나쁘게는 이상하게 성공한다.
/// 파일을 **매핑해서 헤더만** 읽는다 — 코드를 실행하지 않는다.
[[nodiscard]] Result<bool> pluginIs64Bit(const std::string& path);

} // namespace mari::host8bf

#endif // MARI_HOST8BF_HOST_CLIENT_HPP
