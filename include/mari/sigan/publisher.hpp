// Mari Paint — Sigan 증거 발행기 (docs/03 4.2 · 5.2 · 5.3)
//
// 🔴 이 파일의 핵심 약속:
//    1. **드롭 없음.** 모든 프레임은 먼저 저널에 들어간다. 파이프는 그 다음이다.
//    2. **seq 연속.** 끊겨도 재연결해도 seq 는 하나도 건너뛰지 않는다.
//       → Sigan 이 seq 연속성만 보고 "같은 Segment"임을 안다(docs/03 5.3).
//    3. **그리기 스레드를 막지 않는다.** publish() 는 noexcept 이고 논블로킹이다.
//
// 🔴 경계선(docs/03 2절): 여기에 해시체인 봉인·ES256 서명·등급 판정은 **없다.**
//    Mari 는 증명하지 않는다. 증명 가능한 형태로 기록할 뿐이다.
#ifndef MARI_SIGAN_PUBLISHER_HPP
#define MARI_SIGAN_PUBLISHER_HPP

#include <mari/core/origin.hpp>
#include <mari/sigan/clock.hpp>
#include <mari/sigan/journal.hpp>
#include <mari/sigan/sink.hpp>

#include <string>

namespace mari::sigan {

/// 그리기 쪽이 넘기는 한 점. seq 와 t 는 **발행기가** 채운다 — 호출자가 못 만든다.
///
/// 🔴 `source` 는 **기본값이 없다.** 기본 생성자를 지운 이유가 그것이다 —
///    `StrokeSample s;` 가 컴파일되면 출처를 빠뜨린 코드가 조용히 사람 획을 만든다.
///    출처를 고르는 세터는 없다. 만들 때 한 번 박고 끝이다(docs/05 3.1).
struct StrokeSample {
    /// 유일한 생성자. 출처 없이는 샘플 자체가 만들어지지 않는다.
    explicit StrokeSample(StrokeSource src) noexcept : source(src) {}

    /// 획의 출처. StrokeSource 는 origin 을 고를 수 있는 공개 API 를 주지 않는다.
    StrokeSource source;
    PointF pos{};          ///< 캔버스 좌표
    f32 pressure = 0.0f;
    f32 tiltX = 0.0f;
    f32 tiltY = 0.0f;
    f32 rotation = 0.0f;
    f32 velocity = 0.0f;
    LayerId layerId = kInvalidLayerId;
    BrushId brushId = kInvalidBrushId;
    u32 flags = 0; ///< FrameFlag 비트합
};

/// 발행기 설정.
struct PublisherConfig {
    std::string journalPath;                 ///< 로컬 저널 경로(필수)
    std::string app = "mari-paint/0.1.0";    ///< 핸드셰이크 app 문자열
    u64 sessionId = 0;                       ///< 0 이면 시계 앵커에서 만든다
};

/// 발행 통계. **Dropped 는 일부러 두지 않는다**(docs/03 4.2).
struct PublisherStats {
    u64 published = 0;   ///< publish() 호출로 발행된 프레임 수
    u64 sent = 0;        ///< 싱크가 받아간 프레임 수
    u64 spooled = 0;     ///< 싱크가 못 받아 저널에만 있는 프레임 수(나중에 밀어넣는다)
    u64 resent = 0;      ///< 재연결 후 밀어넣은 프레임 수
    u64 reconnects = 0;  ///< 재핸드셰이크 횟수. **새 Segment 가 아니다**
    u64 idTruncations = 0; ///< u64 식별자를 u32 와이어로 좁히며 잘린 횟수(정직하게 센다)
    /// 출처별 **획**(Down 프레임) 수. docs/05 3.2 의 비율 표시용 원자료다.
    /// 🔴 숫자만 싣는다. 등급 판정은 Sigan 의 몫이다(docs/03 2절).
    StrokeOriginStats origins{};
};

/// 스트로크 프레임 발행기.
///
/// 스레드 규약: 한 인스턴스는 그리기 스레드 하나가 독점한다.
///   publish()/endStroke() 는 그리기 스레드에서, pump() 는 획 사이나 유휴 시점에 부른다.
class SiganPublisher {
public:
    /// 저널을 만들고 발행기를 연다. 싱크는 나중에 붙여도 된다(Sigan 미설치 = 로컬 모드).
    static Result<std::unique_ptr<SiganPublisher>> open(PublisherConfig cfg,
                                                        SessionClock clock = SessionClock());

    ~SiganPublisher();

    /// 싱크를 붙인다(소유하지 않는다). nullptr 이면 로컬 저널 모드.
    void setSink(ISiganSink* sink) noexcept;

    /// 한 점을 발행한다. **핫 패스** — noexcept, 할당 없음, 절대 블로킹 없음.
    /// 돌아오는 값은 이 점에 배정된 seq 다. 0 은 없다.
    u64 publish(const StrokeSample& s) noexcept;

    /// 획이 끝났다(up). 저널을 flush 하고 완성 지점을 찍는다(docs/03 5.4).
    void endStroke() noexcept;

    /// 저빈도 유지보수: 끊겼으면 재연결·재핸드셰이크하고 **밀린 seq 구간을 재전송**한다.
    /// 🔴 재연결해도 seq 는 이어진다 — 새 Segment 를 만들지 않는다(docs/03 5.3).
    Result<void> pump();

    /// 협상 결과. 싱크가 없으면 로컬 기본값이다.
    [[nodiscard]] const Negotiated& negotiated() const noexcept { return negotiated_; }
    [[nodiscard]] const PublisherStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const SessionClock& clock() const noexcept { return clock_; }
    [[nodiscard]] u64 sessionId() const noexcept { return sessionId_; }
    /// 마지막으로 배정된 seq. 다음 프레임은 이 값 + 1 이다.
    [[nodiscard]] u64 lastSeq() const noexcept { return seq_; }
    /// 싱크가 받아간 마지막 seq. 이 뒤는 저널에만 있다.
    [[nodiscard]] u64 sentSeq() const noexcept { return sentSeq_; }
    [[nodiscard]] Journal& journal() noexcept { return *journal_; }

    /// 우리가 상대에게 보내는 자기소개를 만든다(재연결 시 resumeFromSeq 가 채워진다).
    [[nodiscard]] Handshake localHandshake() const;

private:
    SiganPublisher() = default;
    void applyCaps(StrokeFrame& f) const noexcept;
    Result<void> resendBacklog();

    JournalPtr journal_;
    ISiganSink* sink_ = nullptr;
    SessionClock clock_{};
    PublisherConfig cfg_{};
    Negotiated negotiated_{};
    PublisherStats stats_{};
    u64 sessionId_ = 0;
    u64 seq_ = 0;
    u64 sentSeq_ = 0;
    bool capLayer_ = true;
    bool capBrush_ = true;
    u8 wire_[kFrameSize]{}; ///< 핫 패스용 고정 버퍼. 할당하지 않는다
};

using PublisherPtr = std::unique_ptr<SiganPublisher>;

/// 미제출 저널을 발견해 복구한다(docs/03 5.4). Mari 가 죽었다 살아났을 때 쓴다.
/// 잘린 꼬리는 버리고 **마지막 완성된 획까지** 돌려준다.
Result<JournalScan> recoverJournal(const std::string& path);

} // namespace mari::sigan

#endif // MARI_SIGAN_PUBLISHER_HPP
