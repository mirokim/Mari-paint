// Mari Paint — 전송 채널 추상화 (docs/03 4절 ② 스트로크 채널)
//
// 🔴 write() 는 **논블로킹**이다. 그리기 스레드를 절대 막지 않는다(docs/03 5.2).
//    막히면 WouldBlock 을 돌려주고, 발행기가 저널로 스풀한다. **드롭은 없다.**
#ifndef MARI_SIGAN_SINK_HPP
#define MARI_SIGAN_SINK_HPP

#include <mari/core/result.hpp>
#include <mari/sigan/frame.hpp>
#include <mari/sigan/handshake.hpp>

#include <memory>

namespace mari::sigan {

/// write() 결과. 세 가지뿐이고, **"버렸다"는 없다.**
enum class SinkStatus {
    Ok,           ///< 전부 받아갔다
    WouldBlock,   ///< 지금은 못 받는다 — 호출자는 저널로 스풀한다
    Disconnected, ///< 끊겼다 — 호출자는 저널로 스풀하고 나중에 재연결한다
};

/// 안정 문자열(로그·리포트용).
const char* sinkStatusName(SinkStatus s) noexcept;

/// 스트로크 프레임을 내보내는 채널.
/// connect()/handshake() 는 저빈도라 예외·할당을 써도 되지만,
/// write() 는 핫 패스다 — noexcept, 할당 금지.
class ISiganSink {
public:
    virtual ~ISiganSink() = default;

    /// 로그용 이름("pipe", "journal", ...).
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// 지금 프레임을 받을 수 있는 상태인가.
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    /// 연결을 시도한다. 저빈도 — 그리기 스레드에서 부르지 마라.
    virtual Result<void> connect() = 0;

    /// 핸드셰이크. 상대의 자기소개를 돌려준다. 저빈도.
    /// 🔴 재핸드셰이크가 **새 Segment 를 만들면 안 된다**(docs/03 5.3) —
    ///    그래서 local 에 sessionId 와 resumeFromSeq 를 실어 보낸다.
    virtual Result<Handshake> handshake(const Handshake& local) = 0;

    /// 논블로킹 쓰기. 부분 쓰기는 **허용하지 않는다** —
    /// 전부 받거나(Ok) 아무것도 안 받은 것처럼(WouldBlock) 굴어야 한다.
    /// 프레임이 반토막 나면 수신자가 스트림을 잃는다.
    virtual SinkStatus write(const u8* data, usize len) noexcept = 0;

    /// 밀린 내부 버퍼를 밀어낸다. 논블로킹.
    virtual SinkStatus pump() noexcept { return SinkStatus::Ok; }

    virtual void close() noexcept = 0;
};

using SinkPtr = std::unique_ptr<ISiganSink>;

} // namespace mari::sigan

#endif // MARI_SIGAN_SINK_HPP
