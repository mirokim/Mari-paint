// Mari Paint — 기록 배선: agent-api · 사람 획 → Sigan 발행기 (docs/06)
//
// 🔴 **`SiganPublisher::publish()` 를 부르는 곳은 리포 전체에서 여기 하나다**(docs/06 결정 ③).
//    `tests/record/test_single_publish_path.cpp` 가 리포의 .cpp 를 훑어 그것을 강제한다.
//    GUI 가 붙는 날 누가 지름길을 뚫으면 그날 빌드가 빨개진다.
//
// 🔴 의존 방향(과제 지시): **agent 는 sigan 을 모른다. app 도 모른다.**
//    둘은 `agent::IStrokeRecorder` 라는 중립 인터페이스만 보고, 실제 발행기는
//    이 모듈이 들고 있다. 꽂는 것은 위(CLI·호스트·테스트)의 일이다.
//    그래서 Sigan 이 없어도 agent-api 는 그대로 돈다 — 공장을 안 꽂으면 널 레코더다.
//
// 🔴 경계선(docs/03 2절): 여기에 ES256 서명도, 해시체인 봉인(prevHash)도,
//    신뢰 등급 판정도 **없다.** 세는 것은 개수와 타일 수뿐이고,
//    만드는 로그의 등급은 "unsigned" 하나뿐이다.
#ifndef MARI_RECORD_SIGAN_RECORDER_HPP
#define MARI_RECORD_SIGAN_RECORDER_HPP

#include <mari/agent/recording.hpp>
#include <mari/sigan/publisher.hpp>
#include <mari/sigan/sink.hpp>

#include <string>
#include <vector>

namespace mari::record {

/// 기록 설정. 한 번 정하고 문서마다 구간을 연다.
struct RecordingConfig {
    /// 저널이 놓일 디렉터리. 비면 임시 디렉터리를 쓴다.
    std::string journalDir;
    std::string app = "mari-paint/0.1.0";
    /// 프레임에는 32비트 다이제스트만 실린다. 사람이 읽을 이름은 prooflog 가 들고 있으므로
    /// 여기서 받아 둔다(docs/05 3.2). 비어도 된다.
    std::vector<AgentId> agents;
    /// 스트로크 채널. nullptr 이면 **로컬 저널 모드**다 —
    /// docs/03 5.1 이 Sigan 미설치를 정상 상태로 규정했으므로 이게 기본값이다.
    sigan::ISiganSink* sink = nullptr;
};

class SiganRecorderFactory;

/// 문서 하나의 기록 구간. **`IStrokeRecorder` 의 유일한 구현체다.**
class SiganRecorder final : public agent::IStrokeRecorder {
public:
    /// 저널을 열고 구간을 시작한다. 저널을 못 열면 실패다 —
    /// 기록 없이 그리는 모드를 만들지 않는다(docs/06 결정 ④).
    [[nodiscard]] static Result<std::unique_ptr<SiganRecorder>>
    open(const RecordingConfig& cfg, const std::string& journalPath, u64 sessionId);

    ~SiganRecorder() override;

    void onStrokePoint(const StrokeSource& src, const agent::StrokePointRecord& p) noexcept override;
    void onStrokeEnd(const StrokeSource& src, u32 changedTiles) noexcept override;
    [[nodiscard]] Result<void> onRegionOp(const StrokeSource& src,
                                          const agent::RegionOpRecord& op) override;
    [[nodiscard]] bool recordingBroken() const noexcept override { return broken_; }
    [[nodiscard]] const agent::RecordingTally& tally() const noexcept override { return tally_; }
    [[nodiscard]] Result<std::string> buildProofLog() const override;

    /// 이 구간의 저널 경로. 리포트가 어디를 보라고 알려 줄 때 쓴다.
    [[nodiscard]] const std::string& journalPath() const noexcept { return journalPath_; }
    /// 🔴 "문서 작업 구간 id" 다. agent-api 세션 id 가 **아니다**(docs/06 결정 ⑤).
    [[nodiscard]] u64 segmentId() const noexcept { return sessionId_; }
    /// 유실 없음을 보여 주는 숫자들(published == sent + spooled).
    [[nodiscard]] const sigan::PublisherStats& stats() const noexcept { return pub_->stats(); }
    /// 밀린 프레임을 밀어 넣고 끊겼으면 재연결한다. 획 사이·유휴 시점에 부른다.
    [[nodiscard]] Result<void> pump() { return pub_->pump(); }

    /// 이 구간을 지켜보는 공장(소멸할 때 집계를 넘긴다). 공장이 붙일 때만 쓴다.
    void observeBy(SiganRecorderFactory* f) noexcept { owner_ = f; }

private:
    SiganRecorder() = default;
    /// 저널이 고장 났는지 확인하고 상태를 남긴다. **자동 복구하지 않는다.**
    void checkJournal() noexcept;

    sigan::PublisherPtr pub_;
    SiganRecorderFactory* owner_ = nullptr;
    agent::RecordingTally tally_{};
    std::string journalPath_;
    std::string app_;
    std::vector<AgentId> agents_;
    u64 sessionId_ = 0;
    bool broken_ = false;
};

/// 문서마다 구간을 여는 공장. app 에 꽂는다(`Application::setRecorderFactory`).
///
/// 🔴 구간을 만드는 것은 **문서**다(docs/06 결정 ⑤). 세션이 열리고 닫히는 것도,
///    클라이언트가 재접속하는 것도 구간 경계가 아니다 — 그래서 이 공장은
///    `doc.create` / `doc.open` 에서만 불린다.
class SiganRecorderFactory final : public agent::IRecorderFactory {
public:
    explicit SiganRecorderFactory(RecordingConfig cfg);
    ~SiganRecorderFactory() override;

    [[nodiscard]] Result<std::unique_ptr<agent::IStrokeRecorder>>
    openForDocument(std::string_view hint) override;

    /// 이 공장이 연 저널 경로 전부(닫힌 것 포함). 순서는 연 순서다.
    [[nodiscard]] const std::vector<std::string>& journals() const noexcept { return journals_; }
    /// 살아 있는 구간 + 이미 닫힌 구간의 집계를 합친 값. **세 축을 합치지는 않는다.**
    [[nodiscard]] agent::RecordingTally totalTally() const noexcept;
    [[nodiscard]] usize liveCount() const noexcept { return live_.size(); }
    /// 살아 있는 구간 하나(없으면 nullptr). 테스트와 리포트가 쓴다.
    [[nodiscard]] SiganRecorder* liveAt(usize i) const noexcept;

    /// 구간이 소멸할 때 자기 집계를 넘긴다. `SiganRecorder` 만 부른다.
    void retire(SiganRecorder* r) noexcept;

private:
    RecordingConfig cfg_;
    std::vector<std::string> journals_;
    std::vector<SiganRecorder*> live_;
    agent::RecordingTally closed_{};
    u64 nextSegment_ = 1;
};

/// 이미 닫힌 구간의 저널에서 무서명 로그를 다시 만든다(docs/03 6절).
///
/// 🔴 축 C(변경 타일 수)는 프레임에 자리가 없는 값이라 살아 있는 구간만 안다.
///    닫힌 저널에서는 **"측정 안 함"으로 나간다** — 0 으로 적어 놓고 잰 척하지 않는다.
[[nodiscard]] Result<std::string> proofLogFromJournal(const std::string& journalPath,
                                                      const RecordingConfig& cfg);

} // namespace mari::record

#endif // MARI_RECORD_SIGAN_RECORDER_HPP
