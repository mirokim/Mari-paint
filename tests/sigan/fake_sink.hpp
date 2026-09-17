// 테스트용 가짜 싱크. 막기·끊기·구버전 흉내를 낸다(docs/03 10절 검증 계획).
#ifndef MARI_TESTS_SIGAN_FAKE_SINK_HPP
#define MARI_TESTS_SIGAN_FAKE_SINK_HPP

#include <mari/sigan/sink.hpp>

#include <string>
#include <vector>

namespace mari::sigan::test {

/// 받은 프레임을 전부 기억하는 싱크. blocked_ 로 인위적으로 막을 수 있다.
class FakeSink final : public ISiganSink {
public:
    [[nodiscard]] const char* name() const noexcept override { return "fake"; }
    [[nodiscard]] bool connected() const noexcept override { return connected_; }

    Result<void> connect() override {
        if (!allowConnect) {
            return Err("가짜 싱크가 연결을 거부한다", ErrorCode::IoError);
        }
        connected_ = true;
        return Ok();
    }

    Result<Handshake> handshake(const Handshake& local) override {
        lastLocal = local;
        ++handshakes;
        if (!replyToHandshake) {
            return Err("응답 없음", ErrorCode::Unsupported);
        }
        return Ok(reply);
    }

    SinkStatus write(const u8* data, usize len) noexcept override {
        if (!connected_) {
            return SinkStatus::Disconnected;
        }
        if (blocked) {
            return SinkStatus::WouldBlock;
        }
        StrokeFrame f;
        decodeFrame(data, f);
        (void)len;
        received.push_back(f);
        return SinkStatus::Ok;
    }

    void close() noexcept override { connected_ = false; }

    /// 테스트에서 Sigan 이 죽은 상황을 만든다.
    void disconnect() noexcept { connected_ = false; }

    bool blocked = false;          ///< true 면 무조건 WouldBlock
    bool allowConnect = true;      ///< connect() 성공 여부
    bool replyToHandshake = true;  ///< 구버전/무응답 흉내
    Handshake reply;               ///< 상대의 자기소개
    Handshake lastLocal;           ///< 우리가 보낸 자기소개(재연결 검증용)
    int handshakes = 0;
    std::vector<StrokeFrame> received;

private:
    bool connected_ = false;
};

} // namespace mari::sigan::test

#endif
