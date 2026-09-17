// 기록 배선의 종단 증명 (docs/06-recording-contract.md)
//
// 여기서 증명하려는 것 — **제품 목표 그 자체다.**
//   1. agent-api 로 그은 획이 발행기까지 **전부** 도착하고, 전부 origin=Agent 다
//   2. 사람 경로 인터페이스로 그은 획은 origin=HumanPen 으로, **같은 발행기**에 도착한다
//   3. 섞어 그리면 집계가 정확히 M/N 이고, prooflog JSON 에 그 숫자가 실제로 찍힌다
//   4. fill 은 규약대로 **합성 프레임 쌍**으로 남고, "AI 획 1개"로 축소되지 않는다
//   5. 파이프가 끊겨도 그리기는 성공하고 **드롭이 0** 이다(저널로 스풀된다)
//   6. 저널이 고장 나면 연산이 실패하고 캔버스가 **원상복구**된다
//   7. 레코더가 없어도 agent-api 는 그대로 돈다(널 레코더)
//   8. 세션을 닫고 다시 열어도 같은 문서면 저널이 하나다(구간을 쪼개지 않는다)
//
// 🔴 경계선(docs/03 2절): 이 파일 어디에도 서명·해시체인·등급 판정이 없다.
#include <mari/agent/session.hpp>
#include <mari/app/stroke_entry.hpp>
#include <mari/record/sigan_recorder.hpp>
#include <mari/ora/ora.hpp>
#include <mari/sigan/prooflog.hpp>
#include <mari/test/harness.hpp>

#include "../sigan/fake_sink.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace mari;
using mari::agent::AgentSession;
using mari::agent::Json;

namespace {

Json req(const char* op) {
    Json j = Json::object();
    j.set("op", Json::string(op));
    j.set("view", Json::string("none"));
    return j;
}

Json strokeAt(i32 x, i32 y) {
    Json j = req("stroke");
    Json pts = Json::array();
    for (int i = 0; i < 4; ++i) {
        Json p = Json::object();
        p.set("x", Json::integer(x + i * 6));
        p.set("y", Json::integer(y));
        pts.push(std::move(p));
    }
    j.set("points", std::move(pts));
    j.set("color", Json::string("#101010"));
    return j;
}

/// 테스트마다 자기 디렉터리를 쓴다(저널이 섞이지 않게).
std::filesystem::path tmpDir(const char* tag) {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / (std::string("mari_rec_") + tag);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

/// 저널을 읽어 프레임을 돌려준다. **파일에서** 읽는다 — 메모리 집계가 아니라
/// 기록된 것을 본다.
std::vector<sigan::StrokeFrame> framesOf(const std::string& path) {
    Result<sigan::JournalScan> scan = sigan::recoverJournal(path);
    if (!scan.ok()) {
        return {};
    }
    return scan.value().frames;
}

} // namespace

// ── 1. 종단 증명 — agent-api 획이 전부 Agent 로 도착한다 ──────────────────

MARI_TEST(agent_strokes_reach_the_publisher_as_agent) {
    const auto dir = tmpDir("agent");
    record::RecordingConfig rc;
    rc.journalDir = dir.string();
    record::SiganRecorderFactory factory(rc);

    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    s.application().setRecorderFactory(&factory);

    Json create = req("doc.create");
    create.set("width", Json::integer(256));
    create.set("height", Json::integer(256));
    CHECK(s.execute(create)["ok"].asBool());
    // 🔴 문서 하나 = 저널 하나(docs/06 결정 ⑤).
    CHECK_EQ(factory.journals().size(), static_cast<usize>(1));

    constexpr int kStrokes = 5;
    for (int i = 0; i < kStrokes; ++i) {
        const Json r = s.execute(strokeAt(20 + i * 10, 40));
        CHECK(r["ok"].asBool());
        // 응답이 기록되었다고 말한다. 말만 하고 안 하면 그게 가장 나쁜 거짓말이다.
        CHECK(r["result"]["recorded"].asBool());
    }

    const auto frames = framesOf(factory.journals()[0]);
    CHECK(frames.size() >= static_cast<usize>(kStrokes * 2)); // 획마다 최소 down+up
    for (const auto& f : frames) {
        // 🔴 하나라도 사람으로 새면 Mari 는 인증서 위조기가 된다.
        CHECK(f.origin == StrokeOrigin::Agent);
        CHECK(f.agentId != 0u);
        CHECK(!sigan::hasFlag(f.flags, sigan::FrameFlag::Synthetic)); // 전부 진짜 붓질이다
    }

    // 획 단위로 세면 정확히 kStrokes 개다.
    Result<sigan::JournalScan> scan = sigan::recoverJournal(factory.journals()[0]);
    CHECK(scan.ok());
    const auto strokes = sigan::summarizeStrokes(scan.value());
    CHECK_EQ(strokes.size(), static_cast<usize>(kStrokes));
    const auto st = sigan::brushStrokeStats(strokes);
    CHECK_EQ(st.agent(), static_cast<u64>(kStrokes));
    CHECK_EQ(st.human(), 0ull);
    CHECK_EQ(st.unspecified(), 0ull); // 출처 없는 획은 하나도 없다
}

// ── 2. 사람 경로 — 같은 발행기, 다른 출처 ─────────────────────────────────

MARI_TEST(human_pen_path_reaches_the_same_publisher) {
    // 🔴 GUI 는 아직 없다. 그러나 **입구는 지금 있다** — WM_POINTER 가 붙는 날
    //    새로 쓸 발행 코드는 0줄이고, 이 테스트가 그 약속을 미리 걸어 둔다.
    const auto dir = tmpDir("human");
    record::RecordingConfig rc;
    rc.journalDir = dir.string();
    record::SiganRecorderFactory factory(rc);

    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    s.application().setRecorderFactory(&factory);
    Json create = req("doc.create");
    create.set("width", Json::integer(128));
    create.set("height", Json::integer(128));
    CHECK(s.execute(create)["ok"].asBool());

    app::Document* doc = s.document();
    CHECK(doc != nullptr);
    const LayerId layer = doc->layers().activeLayer();

    constexpr int kStrokes = 3;
    for (int k = 0; k < kStrokes; ++k) {
        // 🔴 출처는 팩토리에서 나온다. 여기서 값을 고르는 길이 없다.
        app::StrokeEntry entry(*doc, StrokeSource::humanPen(), layer, kInvalidBrushId, false);
        CHECK(entry.recording());
        app::PenSample p;
        p.pos = PointF{static_cast<f32>(10 + k), 20.0f};
        p.pressure = 0.4f;
        entry.down(p);
        p.pos.x += 5.0f;
        p.pressure = 0.8f;
        entry.move(p);
        p.pos.x += 5.0f;
        p.pressure = 0.2f;
        entry.up(p);
        entry.finish(2);
        CHECK(!entry.broken());
    }

    Result<sigan::JournalScan> scan = sigan::recoverJournal(factory.journals()[0]);
    CHECK(scan.ok());
    const auto strokes = sigan::summarizeStrokes(scan.value());
    CHECK_EQ(strokes.size(), static_cast<usize>(kStrokes));
    for (const auto& f : scan.value().frames) {
        CHECK(f.origin == StrokeOrigin::HumanPen);
        CHECK_EQ(f.agentId, 0u); // 사람 획에는 에이전트 식별자가 없다
    }
    const agent::RecordingTally& t = doc->recorder()->tally();
    CHECK_EQ(t.strokes.count(StrokeOrigin::HumanPen), static_cast<u64>(kStrokes));
    CHECK_EQ(t.strokes.agent(), 0ull);
    CHECK_EQ(t.humanTiles(), static_cast<u64>(kStrokes * 2));
}

// ── 3. 섞어 그리기 — 집계가 정확하고 prooflog 에 찍힌다 ───────────────────

MARI_TEST(mixed_human_and_agent_counts_are_exact_in_the_prooflog) {
    const auto dir = tmpDir("mixed");
    record::RecordingConfig rc;
    rc.journalDir = dir.string();
    Result<AgentId> id = AgentId::make("claude/opus-5");
    CHECK(id.ok());
    rc.agents.push_back(id.value());
    record::SiganRecorderFactory factory(rc);

    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    s.application().setRecorderFactory(&factory);
    Json create = req("doc.create");
    create.set("width", Json::integer(256));
    create.set("height", Json::integer(256));
    CHECK(s.execute(create)["ok"].asBool());

    app::Document* doc = s.document();
    CHECK(doc != nullptr);
    const LayerId layer = doc->layers().activeLayer();

    constexpr int kHuman = 7; // 사람 M
    constexpr int kAgent = 3; // AI N
    for (int k = 0; k < kHuman; ++k) {
        app::StrokeEntry entry(*doc, StrokeSource::humanPen(), layer, kInvalidBrushId, false);
        app::PenSample p;
        p.pos = PointF{static_cast<f32>(k * 3), 5.0f};
        p.pressure = 0.5f;
        entry.down(p);
        p.pos.x += 2.0f;
        entry.up(p);
        entry.finish(1);
    }
    for (int k = 0; k < kAgent; ++k) {
        CHECK(s.execute(strokeAt(60 + k * 10, 100))["ok"].asBool());
    }

    // 세션 집계(AI 만 센다) vs 문서 구간 집계(사람까지 센다). 둘은 다른 것을 센다.
    CHECK_EQ(s.originStats().agent(), static_cast<u64>(kAgent));
    const agent::RecordingTally& t = doc->recorder()->tally();
    CHECK_EQ(t.strokes.human(), static_cast<u64>(kHuman));
    CHECK_EQ(t.strokes.agent(), static_cast<u64>(kAgent));

    // 🔴 prooflog 에 **실제로** 찍히는가. 숫자가 파일까지 못 가면 아무 소용이 없다.
    Result<std::string> log = doc->recorder()->buildProofLog();
    CHECK(log.ok());
    const std::string& json = log.value();
    CHECK(contains(json, "\"humanStrokes\": 7"));
    CHECK(contains(json, "\"agentStrokes\": 3"));
    CHECK(contains(json, "\"humanPen\": 7"));
    CHECK(contains(json, "\"unspecified\": 0"));
    CHECK(contains(json, "claude/opus-5"));
    CHECK(contains(json, "\"changedTiles\""));
    CHECK(contains(json, "\"measured\": true"));

    // 🔴 판정은 Sigan 의 몫이다. Mari 는 숫자만 싣는다.
    CHECK(!contains(json, "human-only"));
    CHECK(!contains(json, "ai-assisted"));
    CHECK(!contains(json, "ai-generated"));
    CHECK(!contains(json, "ES256"));
    CHECK(contains(json, "\"grade\": \"unsigned\""));

    // 저장한 .ora 안에도 같은 로그가 들어간다(docs/03 6절 통로).
    const std::string oraPath = (dir / "mixed.ora").string();
    Json save = req("doc.save");
    save.set("path", Json::string(oraPath));
    CHECK(s.execute(save)["ok"].asBool());
    Result<std::optional<std::string>> inOra = ora::readProofLog(oraPath);
    CHECK(inOra.ok());
    CHECK(inOra.value().has_value());
    if (inOra.value().has_value()) {
        CHECK(contains(*inOra.value(), "\"humanStrokes\": 7"));
        CHECK(contains(*inOra.value(), "\"agentStrokes\": 3"));
        CHECK(!contains(*inOra.value(), "ai-assisted"));
    }
}

// ── 4. fill 은 "AI 획 1개"로 축소되지 않는다 ──────────────────────────────

MARI_TEST(fill_is_a_synthetic_frame_pair_not_a_brush_stroke) {
    const auto dir = tmpDir("fill");
    record::RecordingConfig rc;
    rc.journalDir = dir.string();
    record::SiganRecorderFactory factory(rc);

    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    s.application().setRecorderFactory(&factory);
    Json create = req("doc.create");
    create.set("width", Json::integer(256));
    create.set("height", Json::integer(256));
    CHECK(s.execute(create)["ok"].asBool());

    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#f2e9dc"));
    const Json f = s.execute(fill);
    CHECK(f["ok"].asBool());
    CHECK_EQ(f["result"]["origin"].asString(), std::string("agent"));
    CHECK_EQ(f["result"]["regionOp"].asString(), std::string("fill"));
    CHECK(f["result"]["changedTiles"].asInt() > 0);

    // 🔴 정확히 2프레임이다. 1개면 영역이 안 담기고, 500개면 없던 붓질을 지어낸 것이다.
    const auto frames = framesOf(factory.journals()[0]);
    CHECK_EQ(frames.size(), static_cast<usize>(2));
    if (frames.size() == 2) {
        CHECK(sigan::hasFlag(frames[0].flags, sigan::FrameFlag::Down));
        CHECK(sigan::hasFlag(frames[0].flags, sigan::FrameFlag::Synthetic));
        CHECK(sigan::hasFlag(frames[1].flags, sigan::FrameFlag::Up));
        CHECK(sigan::hasFlag(frames[1].flags, sigan::FrameFlag::Synthetic));
        // 두 점이 영향 영역을 정한다 — 좌상단과 우하단.
        CHECK_NEAR(static_cast<f64>(frames[0].cx), 0.0, 1e-6);
        CHECK_NEAR(static_cast<f64>(frames[0].cy), 0.0, 1e-6);
        CHECK_NEAR(static_cast<f64>(frames[1].cx), 255.0, 1e-6);
        CHECK_NEAR(static_cast<f64>(frames[1].cy), 255.0, 1e-6);
        // 🔴 없던 필압을 지어내지 않는다. 전부 0 이다.
        for (const auto& fr : frames) {
            CHECK_NEAR(static_cast<f64>(fr.pressure), 0.0, 1e-9);
            CHECK_NEAR(static_cast<f64>(fr.tiltX), 0.0, 1e-9);
            CHECK_NEAR(static_cast<f64>(fr.velocity), 0.0, 1e-9);
            CHECK(fr.origin == StrokeOrigin::Agent);
        }
    }

    // 🔴 붓질 칸은 **0 이다.** 여기가 1 이 되는 순간 "AI 10.3%" 류의 거짓이 시작된다.
    CHECK_EQ(s.originStats().agent(), 0ull);
    CHECK_EQ(s.regionOpStats().agent(), 1ull);
    app::Document* doc = s.document();
    const agent::RecordingTally& t = doc->recorder()->tally();
    CHECK_EQ(t.strokes.total(), 0ull);
    CHECK_EQ(t.regionOps.agent(), 1ull);
    // 256x256 캔버스는 타일 4x4 = 16개다. 면적이 실제로 세어진다.
    CHECK_EQ(t.agentTiles(), 16ull);
    CHECK_EQ(s.changedTiles(), 16ull);

    // prooflog 도 두 칸을 나눠 말한다.
    Result<std::string> log = doc->recorder()->buildProofLog();
    CHECK(log.ok());
    CHECK(contains(log.value(), "\"agentStrokes\": 0"));   // 붓질 0
    CHECK(contains(log.value(), "\"agentRegionOps\": 1")); // 영역 연산 1
    CHECK(contains(log.value(), "\"synthetic\": true"));
    CHECK(contains(log.value(), "\"agent\": 16"));         // 변경 타일 16
}

// ── 5. 파이프 실패는 실패가 아니다 — 드롭 0 ───────────────────────────────

MARI_TEST(pipe_failure_still_draws_and_drops_nothing) {
    const auto dir = tmpDir("pipe");
    sigan::test::FakeSink sink;
    sink.allowConnect = false; // Sigan 이 죽어 있다 — docs/03 5.1 이 말하는 정상 상태다
    record::RecordingConfig rc;
    rc.journalDir = dir.string();
    rc.sink = &sink;
    record::SiganRecorderFactory factory(rc);

    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    s.application().setRecorderFactory(&factory);
    Json create = req("doc.create");
    create.set("width", Json::integer(128));
    create.set("height", Json::integer(128));
    CHECK(s.execute(create)["ok"].asBool());

    // 🔴 파이프가 끊겨 있어도 fill 은 **성공한다.** 여기서 거절하면 제품이 망가진다.
    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    CHECK(s.execute(fill)["ok"].asBool());
    CHECK(s.execute(strokeAt(10, 10))["ok"].asBool());

    record::SiganRecorder* rec = factory.liveAt(0);
    CHECK(rec != nullptr);
    if (rec != nullptr) {
        const sigan::PublisherStats& st = rec->stats();
        CHECK(st.published > 0);
        CHECK_EQ(st.sent, 0ull);                     // 아무것도 못 보냈다
        CHECK_EQ(st.published, st.sent + st.spooled); // 🔴 그래도 유실은 0 이다
        CHECK(!rec->recordingBroken());               // 파이프 실패는 고장이 아니다
    }
    // 저널에는 전부 남아 있다. 정본은 저널이다.
    CHECK(framesOf(factory.journals()[0]).size() >= 3u);
}

// ── 6. 저널 실패는 연산 실패다 — 롤백까지 ─────────────────────────────────

namespace {

/// 저널이 고장 난 상태를 흉내 내는 레코더. **두 번째 발행 경로가 아니다** —
/// publish() 를 부르지 않는다. ops 쪽의 롤백 규약만 검사한다(docs/06 결정 ④).
class BrokenRecorder final : public agent::IStrokeRecorder {
public:
    void onStrokePoint(const StrokeSource&, const agent::StrokePointRecord&) noexcept override {}
    void onStrokeEnd(const StrokeSource&, u32) noexcept override {}
    Result<void> onRegionOp(const StrokeSource&, const agent::RegionOpRecord&) override {
        return Err("저널이 고장 났다(테스트)", ErrorCode::IoError);
    }
    [[nodiscard]] bool recordingBroken() const noexcept override { return broken; }
    [[nodiscard]] const agent::RecordingTally& tally() const noexcept override { return tally_; }
    [[nodiscard]] Result<std::string> buildProofLog() const override {
        return Err("고장 난 기록에서 로그를 만들지 않는다", ErrorCode::IoError);
    }
    bool broken = false;

private:
    agent::RecordingTally tally_{};
};

class BrokenFactory final : public agent::IRecorderFactory {
public:
    Result<std::unique_ptr<agent::IStrokeRecorder>> openForDocument(std::string_view) override {
        auto r = std::make_unique<BrokenRecorder>();
        last = r.get();
        return Ok(std::unique_ptr<agent::IStrokeRecorder>(std::move(r)));
    }
    BrokenRecorder* last = nullptr;
};

} // namespace

MARI_TEST(journal_failure_rejects_the_draw_and_rolls_back) {
    BrokenFactory factory;
    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    s.application().setRecorderFactory(&factory);
    Json create = req("doc.create");
    create.set("width", Json::integer(128));
    create.set("height", Json::integer(128));
    CHECK(s.execute(create)["ok"].asBool());

    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#ff0000"));
    const Json f = s.execute(fill);
    // 🔴 기록이 안 되면 연산도 실패다. 픽셀만 바뀌고 정본에 흔적이 없는 상태를 남기지 않는다.
    CHECK(!f["ok"].asBool());

    // 🔴 그리고 캔버스는 **원상복구**되어 있다.
    std::vector<u8> px;
    CHECK(s.document()->exportComposite(px).ok());
    CHECK_EQ(static_cast<int>(px[3]), 0); // 좌상단 알파 = 0, 칠해지지 않았다
    CHECK_EQ(s.regionOpStats().agent(), 0ull);

    // 이후 그리기는 전부 거절된다. 자동 복구는 없다 — 그 사이가 증거의 구멍이 된다.
    factory.last->broken = true;
    CHECK(!s.execute(strokeAt(10, 10))["ok"].asBool());
    CHECK(!s.execute(fill)["ok"].asBool());
}

MARI_TEST(a_real_unwritable_journal_is_detected) {
    // 진짜 저널 고장. /dev/full 은 열리지만 쓰기가 항상 실패한다.
    if (!std::filesystem::exists("/dev/full")) {
        return; // 이 플랫폼에는 없다. 없는 것을 있는 척하지 않는다
    }
    record::RecordingConfig rc;
    Result<std::unique_ptr<record::SiganRecorder>> rec =
        record::SiganRecorder::open(rc, "/dev/full", 1);
    CHECK(rec.ok());
    if (!rec.ok()) {
        return;
    }
    agent::RegionOpRecord op;
    op.kind = agent::RegionOpKind::Fill;
    op.area = Rect{0, 0, 64, 64};
    op.layerId = 1;
    op.changedTiles = 1;
    const Result<void> r = rec.value()->onRegionOp(StrokeSource::humanPen(), op);
    CHECK(!r.ok()); // 🔴 못 썼으면 못 썼다고 말한다. 조용히 성공한 척하지 않는다
    CHECK(rec.value()->recordingBroken());
    // 고장 난 뒤에는 집계도 늘지 않는다 — 일어나지 않은 기록을 세지 않는다.
    CHECK_EQ(rec.value()->tally().regionOps.total(), 0ull);
}

// ── 7. 레코더 없이도 agent-api 는 돈다 (널 레코더) ────────────────────────

MARI_TEST(agent_api_works_without_any_recorder) {
    // 🔴 Sigan 미설치는 정상 상태다(docs/03 5.1). 공장을 안 꽂아도 전부 돌아야 한다.
    auto opened = AgentSession::open("claude/opus-5");
    CHECK(opened.ok());
    AgentSession& s = *opened.value();
    CHECK(s.application().recorderFactory() == nullptr);

    Json create = req("doc.create");
    create.set("width", Json::integer(128));
    create.set("height", Json::integer(128));
    CHECK(s.execute(create)["ok"].asBool());
    const Json st = s.execute(strokeAt(10, 10));
    CHECK(st["ok"].asBool());
    // 🔴 기록되지 않았다는 사실을 **숨기지 않는다.** 기록된 척하는 것이 가장 나쁘다.
    CHECK(!st["result"]["recorded"].asBool());

    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    const Json f = s.execute(fill);
    CHECK(f["ok"].asBool());
    CHECK(!f["result"]["recorded"].asBool());
    CHECK(s.document()->recorder() == nullptr);
    CHECK(!s.document()->recordingBroken()); // 없는 것과 고장 난 것은 다르다

    // 집계는 그대로 선다(기록 여부와 무관하게 세션은 자기가 한 일을 안다).
    CHECK_EQ(s.originStats().agent(), 1ull);
    CHECK_EQ(s.regionOpStats().agent(), 1ull);
}

// ── 8. 구간을 만드는 것은 문서다 (결정 ⑤) ─────────────────────────────────

MARI_TEST(a_new_session_on_the_same_document_does_not_split_the_segment) {
    const auto dir = tmpDir("segment");
    record::RecordingConfig rc;
    rc.journalDir = dir.string();
    record::SiganRecorderFactory factory(rc);

    const std::string oraPath = (dir / "work.ora").string();
    {
        auto opened = AgentSession::open("claude/opus-5");
        CHECK(opened.ok());
        AgentSession& s = *opened.value();
        s.application().setRecorderFactory(&factory);
        Json create = req("doc.create");
        create.set("width", Json::integer(128));
        create.set("height", Json::integer(128));
        CHECK(s.execute(create)["ok"].asBool());
        CHECK(s.execute(strokeAt(10, 10))["ok"].asBool());
        Json save = req("doc.save");
        save.set("path", Json::string(oraPath));
        CHECK(s.execute(save)["ok"].asBool());
    }
    // 세션이 사라졌다. 🔴 그건 작가 입장에서 아무 일도 아니다.
    CHECK_EQ(factory.journals().size(), static_cast<usize>(1));

    // 같은 공장으로 새 세션을 열어 **다른 문서**를 열면 그때 구간이 하나 더 생긴다.
    {
        auto opened = AgentSession::open("claude/opus-5");
        CHECK(opened.ok());
        AgentSession& s = *opened.value();
        s.application().setRecorderFactory(&factory);
        Json open = req("doc.open");
        open.set("path", Json::string(oraPath));
        CHECK(s.execute(open)["ok"].asBool());
        CHECK(s.execute(strokeAt(30, 30))["ok"].asBool());
    }
    // 문서를 두 번 열었으니 구간도 둘이다 — 세션 때문이 아니라 **문서 때문이다.**
    CHECK_EQ(factory.journals().size(), static_cast<usize>(2));

    // 닫힌 구간의 숫자도 사라지지 않는다.
    const agent::RecordingTally total = factory.totalTally();
    CHECK_EQ(total.strokes.agent(), 2ull);

    // 한 세션이 같은 문서에 계속 그리면 저널은 **하나뿐**이다.
    {
        auto opened = AgentSession::open("claude/opus-5");
        CHECK(opened.ok());
        AgentSession& s = *opened.value();
        s.application().setRecorderFactory(&factory);
        Json open = req("doc.open");
        open.set("path", Json::string(oraPath));
        CHECK(s.execute(open)["ok"].asBool());
        const usize before = factory.journals().size();
        for (int i = 0; i < 4; ++i) {
            CHECK(s.execute(strokeAt(40 + i * 5, 50))["ok"].asBool());
        }
        CHECK_EQ(factory.journals().size(), before); // 획이 늘어도 구간은 그대로다
    }
}

MARI_TEST_MAIN()
