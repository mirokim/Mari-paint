// Mari Paint — 명명 파이프 싱크 (docs/03 4절 ②)
//
// Windows: 명명 파이프 \\.\pipe\sigan-native (Sigan 의 WinTabBridge 규약).
// POSIX  : 같은 이름의 유닉스 도메인 소켓으로 대체한다 — Linux 에서도 컴파일·동작한다.
//
// 🔴 쓰기는 논블로킹이다. 커널 버퍼가 차면 WouldBlock 을 돌려줄 뿐
//    그리기 스레드를 막지 않는다(docs/03 5.2). 스풀은 발행기가 저널로 한다.
#ifndef MARI_SIGAN_PIPE_SINK_HPP
#define MARI_SIGAN_PIPE_SINK_HPP

#include <mari/sigan/sink.hpp>

#include <string>
#include <vector>

namespace mari::sigan {

/// 기본 채널 이름. 플랫폼별 실제 경로는 defaultPipePath() 가 만든다.
inline constexpr const char* kPipeName = "sigan-native";

/// Windows 면 `\\.\pipe\sigan-native`, POSIX 면 유닉스 소켓 경로.
std::string defaultPipePath();

/// 명명 파이프(POSIX 에서는 유닉스 도메인 소켓) 클라이언트.
///
/// 부분 쓰기 정책: 커널이 프레임을 반만 받아가면 나머지는 내부 잔여 버퍼에 보관하고
/// 다음 write()/pump() 에서 마저 민다. **와이어에 반토막 프레임을 남기지 않는다.**
/// 잔여 버퍼가 한도를 넘으면 WouldBlock 을 돌려 스풀로 넘긴다.
class PipeSink final : public ISiganSink {
public:
    explicit PipeSink(std::string path = defaultPipePath(), usize residualLimit = 1u << 20);
    ~PipeSink() override;

    [[nodiscard]] const char* name() const noexcept override { return "pipe"; }
    [[nodiscard]] bool connected() const noexcept override { return fd_ >= 0; }

    Result<void> connect() override;
    Result<Handshake> handshake(const Handshake& local) override;
    SinkStatus write(const u8* data, usize len) noexcept override;
    SinkStatus pump() noexcept override;
    void close() noexcept override;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    /// 아직 커널에 못 넘긴 잔여 바이트.
    [[nodiscard]] usize residual() const noexcept { return residual_.size() - residualHead_; }

private:
    /// 논블로킹 저수준 쓰기. 보낸 바이트 수, 끊기면 -1.
    long rawWrite(const u8* data, usize len) noexcept;
    bool drainResidual() noexcept;

    std::string path_;
    int fd_ = -1; ///< POSIX fd. Windows 에서는 HANDLE 을 담는 인덱스로 쓴다(-1 = 끊김)
    [[maybe_unused]] void* handle_ = nullptr; ///< Windows HANDLE. POSIX 에서는 쓰지 않는다
    std::vector<u8> residual_;
    usize residualHead_ = 0;
    usize residualLimit_;
};

} // namespace mari::sigan

#endif // MARI_SIGAN_PIPE_SINK_HPP
