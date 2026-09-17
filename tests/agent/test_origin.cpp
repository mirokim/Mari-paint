// A0 — StrokeOrigin: 획의 출처가 위조 불가능하게 따라다니는가 (docs/05 3절 · 6절)
//
// 여기서 증명하려는 것 넷:
//   1. `agent_origin_forced`  — 에이전트 진입점으로 만든 획은 **무조건** Agent 다.
//   2. `no_origin_override`   — origin 을 임의로 설정하는 공개 API 가 **없다.**
//   3. 출처별 집계가 정확하다 (docs/05 3.2 의 "사람 획 N / AI 획 M" 원자료).
//   4. 출처가 저널 → prooflog 까지 **손실 없이** 흘러간다.
//
// (프레임 바이트가 origin 에 반응하는가 = `origin_in_signature` 는
//  tests/sigan/test_frame.cpp 에 있다. 골든 바이트 옆에 두는 게 맞다.)
#include <mari/agent/stroke_gate.hpp>
#include <mari/core/origin.hpp>
#include <mari/sigan/prooflog.hpp>
#include <mari/sigan/publisher.hpp>
#include <mari/stroke/native_engine.hpp>
#include <mari/stroke/pipeline.hpp>
#include <mari/test/harness.hpp>

#include "../stroke/fake_tilemap.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

using namespace mari;
using mari::agent::AgentStrokeGate;
using mari::test::FakeTileMap;

namespace {

constexpr u64 kMs = 1'000'000ull;

stroke::RawInputEvent pen(f64 x, f64 y, u64 ms) {
    stroke::RawInputEvent e{};
    e.x = x;
    e.y = y;
    e.timestampNs = ms * kMs;
    e.pressure = 1.0f;
    return e;
}

brush::MariBrushPreset roundPreset() {
    brush::MariBrushPreset p;
    p.name = "origin-round";
    p.tip.diameter = 8.0f;
    p.tip.hardness = 0.9f;
    p.spacing = 0.2f;
    return p;
}

std::string tmpPath(const char* tag) {
    return (std::filesystem::temp_directory_path() / (std::string("mari_origin_") + tag + ".jrnl"))
        .string();
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

/// 저장소 루트를 __FILE__ 에서 되짚는다(.../tests/agent/test_origin.cpp → 3단계 위).
std::filesystem::path repoRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string readFile(const std::filesystem::path& p) {
    std::FILE* fp = std::fopen(p.string().c_str(), "rb");
    if (fp == nullptr) {
        return {};
    }
    std::string out;
    char buf[4096];
    usize n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(fp);
    return out;
}

// ── no_origin_override 의 컴파일 타임 절반 ───────────────────────────────
// origin 을 설정하는 멤버가 **존재하지 않음**을 검출 관용구로 못박는다.
// 누가 나중에 setOrigin() 을 달면 여기서 컴파일이 깨진다.
template <typename T, typename = void> struct HasSetOrigin : std::false_type {};
template <typename T>
struct HasSetOrigin<T, std::void_t<decltype(std::declval<T&>().setOrigin(
                           std::declval<StrokeOrigin>()))>> : std::true_type {};

static_assert(!HasSetOrigin<StrokeSource>::value, "StrokeSource 에 setOrigin 이 생기면 안 된다");
static_assert(!HasSetOrigin<brush::StrokeContext>::value,
              "StrokeContext 에 setOrigin 이 생기면 안 된다");
static_assert(!HasSetOrigin<sigan::StrokeSample>::value,
              "StrokeSample 에 setOrigin 이 생기면 안 된다");

// 🔴 출처는 **고를 수 없다.** origin 값을 받아 StrokeSource 를 만드는 길이 없어야 한다.
static_assert(!std::is_constructible_v<StrokeSource, StrokeOrigin>,
              "origin 을 인자로 받는 StrokeSource 생성자가 있으면 안 된다");
static_assert(!std::is_constructible_v<StrokeSource, StrokeOrigin, AgentId>,
              "origin + agentId 를 받는 공개 생성자가 있으면 안 된다");
// 🔴 기본값 금지. 빠뜨리면 컴파일이 깨져야 한다 — 조용히 사람 획이 되면 안 된다.
static_assert(!std::is_default_constructible_v<StrokeSource>,
              "StrokeSource 에 기본 생성자가 생기면 안 된다");
static_assert(!std::is_default_constructible_v<brush::StrokeContext>,
              "StrokeContext 에 기본 생성자가 생기면 안 된다 — 출처를 빠뜨릴 수 있게 된다");
static_assert(!std::is_default_constructible_v<sigan::StrokeSample>,
              "StrokeSample 에 기본 생성자가 생기면 안 된다");
// 🔴 에이전트 진입점은 origin 을 **인자로 받지 않는다.**
static_assert(std::is_invocable_v<decltype(&AgentStrokeGate::source), const AgentStrokeGate&>,
              "source() 는 인자 없이 불릴 수 있어야 한다");
static_assert(!std::is_invocable_v<decltype(&AgentStrokeGate::source), const AgentStrokeGate&,
                                   StrokeOrigin>,
              "🔴 에이전트 게이트가 origin 을 인자로 받으면 그게 위조 수단이다");
// 핫 패스 안전: 출처를 들고 다녀도 복사가 할당을 하지 않는다.
static_assert(std::is_trivially_copyable_v<StrokeSource>,
              "StrokeSource 복사가 할당을 하면 핫 패스 규칙이 깨진다");

} // namespace

// ── 1. agent_origin_forced ───────────────────────────────────────────────

MARI_TEST(agent_origin_forced) {
    auto gate = AgentStrokeGate::open("claude/opus-5");
    CHECK(gate.ok());
    // 🔴 게이트는 origin 을 고를 인자를 주지 않는다. 나오는 건 언제나 Agent 다.
    const StrokeSource src = gate.value().source();
    CHECK(src.origin() == StrokeOrigin::Agent);
    CHECK(src.isAgent());
    CHECK(!isHumanOrigin(src.origin()));
    CHECK(src.agentDigest() != 0u);
    CHECK_EQ(std::string(src.agent().c_str()), std::string("claude/opus-5"));

    // 같은 게이트를 몇 번을 물어도 사람 획이 되는 길은 없다.
    for (int i = 0; i < 8; ++i) {
        CHECK(gate.value().source().origin() == StrokeOrigin::Agent);
    }
}

MARI_TEST(agent_gate_refuses_anonymous_agent) {
    // 익명 에이전트 획은 만들지 않는다 — "누가 그렸는지 모르는 AI 획"은 증거가 아니다.
    CHECK(!AgentStrokeGate::open("").ok());
    CHECK(!AgentStrokeGate::open(std::string(40, 'a')).ok());
    CHECK(!AgentStrokeGate::open("나쁜 아이디").ok()); // 허용 문자 밖
    CHECK(!AgentStrokeGate::open(AgentId{}).ok());
    CHECK(AgentStrokeGate::open("mari.agent_1:v2+x/y-z").ok());
}

MARI_TEST(agent_stroke_reaches_the_frame_as_agent) {
    // 에이전트 진입점 → 스트로크 파이프라인 → Sigan 프레임 → 저널.
    // 중간 어디에서도 출처를 갈아끼울 수 있는 자리가 없다.
    auto gate = AgentStrokeGate::open("claude/opus-5");
    CHECK(gate.ok());

    FakeTileMap map;
    auto engine = stroke::makeNativeEngine();
    CHECK(engine.ok());
    CHECK(engine.value()->setPreset(roundPreset(), nullptr).ok());

    stroke::StrokePipeline pipe(engine.value().get());
    // 🔴 여기가 핵심이다. 사람 팩토리를 쓸 수도 있지만, 에이전트 경로는 게이트를 거치고
    //    게이트는 Agent 밖에 못 만든다.
    brush::StrokeContext ctx(gate.value().source());
    ctx.target = &map;
    ctx.color = Color8::rgba(0, 0, 0, 255);
    ctx.layerId = 3;
    CHECK(pipe.begin(ctx, pen(10.0, 10.0, 0)).ok());
    CHECK(pipe.source().has_value());
    CHECK(pipe.source().value().origin() == StrokeOrigin::Agent);

    const std::string path = tmpPath("agent");
    auto pub = sigan::SiganPublisher::open(sigan::PublisherConfig{path, "mari-paint/0.1.0", 99});
    CHECK(pub.ok());
    auto& p = *pub.value();

    const std::vector<u32> flags = {
        sigan::frameFlags(sigan::FrameFlag::Down), sigan::frameFlags(sigan::FrameFlag::Move),
        sigan::frameFlags(sigan::FrameFlag::Move), sigan::frameFlags(sigan::FrameFlag::Up)};
    for (usize i = 0; i < flags.size(); ++i) {
        if (i + 1 == flags.size()) {
            pipe.end(pen(10.0 + static_cast<f64>(i) * 4.0, 10.0, i * 8));
        } else if (i > 0) {
            pipe.extend(pen(10.0 + static_cast<f64>(i) * 4.0, 10.0, i * 8));
        }
        // 출처는 파이프라인이 들고 있는 것을 그대로 싣는다. 여기서 고르지 않는다.
        sigan::StrokeSample s(pipe.source().value());
        s.pos = pipe.lastSample().pos;
        s.pressure = pipe.lastSample().pressure;
        s.layerId = 3;
        s.flags = flags[i];
        p.publish(s);
    }
    p.endStroke();
    CHECK_EQ(p.stats().origins.agent(), 1ull);   // 획 1개
    CHECK_EQ(p.stats().origins.human(), 0ull);
    pub.value().reset();

    auto scan = sigan::recoverJournal(path);
    CHECK(scan.ok());
    CHECK_EQ(scan.value().frames.size(), static_cast<usize>(4));
    const u32 digest = gate.value().agentId().digest();
    for (const auto& f : scan.value().frames) {
        // 🔴 프레임 **전부**가 Agent 다. 하나라도 사람으로 새면 인증서 위조기가 된다.
        CHECK(f.origin == StrokeOrigin::Agent);
        CHECK_EQ(f.agentId, digest);
    }
    std::filesystem::remove(path);
}

// ── 2. no_origin_override ────────────────────────────────────────────────

MARI_TEST(no_origin_override) {
    // 절반은 위 namespace 의 static_assert 가 이미 컴파일 타임에 증명했다.
    // 나머지 절반: **공개 헤더 어디에도** origin 을 설정하는 이름이 없다.
    const std::filesystem::path inc = repoRoot() / "include" / "mari";
    CHECK(std::filesystem::exists(inc));

    static const char* kForbidden[] = {"setOrigin",     "set_origin",    "originOverride",
                                       "overrideOrigin", "forceOrigin",  "withOrigin",
                                       "markAsHuman",   "setStrokeOrigin"};
    usize scanned = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(inc)) {
        if (!e.is_regular_file() || e.path().extension() != ".hpp") {
            continue;
        }
        ++scanned;
        const std::string text = readFile(e.path());
        for (const char* bad : kForbidden) {
            if (text.find(bad) != std::string::npos) {
                CHECK_FAIL(std::string("공개 헤더에 출처 설정 API 가 생겼다: ") +
                           e.path().filename().string() + " → " + bad);
            }
        }
    }
    CHECK(scanned > 10); // 스캔이 실제로 돌았는지 확인(빈 통과 방지)

    // 에이전트 게이트의 source() 는 인자를 받지 않는다 — 시그니처를 눈으로도 못박는다.
    const std::string gateSrc = readFile(repoRoot() / "include" / "mari" / "agent" /
                                         "stroke_gate.hpp");
    CHECK(contains(gateSrc, "StrokeSource source() const noexcept"));
    // StrokeSource 의 생성자는 private 이고 기본값은 지워져 있다.
    const std::string originSrc = readFile(repoRoot() / "include" / "mari" / "core" /
                                           "origin.hpp");
    CHECK(contains(originSrc, "StrokeSource() = delete;"));
    CHECK(contains(originSrc, "friend class agent::AgentStrokeGate;"));
    // 🔴 경계선(docs/03 2절): 출처 계층이 등급을 자칭하지 않는다.
    CHECK(!contains(originSrc, "human-only"));
    CHECK(!contains(originSrc, "ai-assisted"));
    CHECK(!contains(originSrc, "ai-generated"));
}

// ── 3. 집계 ──────────────────────────────────────────────────────────────

MARI_TEST(origin_stats_count_exactly) {
    StrokeOriginStats st;
    st.add(StrokeOrigin::HumanPen, 1847);
    st.add(StrokeOrigin::Agent, 213);
    st.add(StrokeOrigin::HumanMouse, 0);
    st.add(StrokeOrigin::Imported, 4);
    st.add(StrokeOrigin::Filter, 2);

    CHECK_EQ(st.count(StrokeOrigin::HumanPen), 1847ull);
    CHECK_EQ(st.count(StrokeOrigin::Agent), 213ull);
    CHECK_EQ(st.human(), 1847ull);
    CHECK_EQ(st.agent(), 213ull);
    CHECK_EQ(st.total(), 2066ull);
    CHECK_EQ(st.unspecified(), 0ull);
    // docs/05 3.2 의 "사람 획 1,847 / AI 획 213 (10.3%)" 중 비율 부분.
    // 🔴 비율까지가 Mari 의 몫이다. 등급 문자열은 만들지 않는다.
    CHECK_NEAR(st.agentRatio(), 213.0 / 2066.0, 1e-12);

    StrokeOriginStats empty;
    CHECK_EQ(empty.total(), 0ull);
    CHECK_NEAR(empty.agentRatio(), 0.0, 1e-12); // 0 나누기 0 을 하지 않는다

    // 출처 미상은 사람으로도 AI 로도 세지 않는다. 따로 센다.
    StrokeOriginStats odd;
    odd.add(static_cast<StrokeOrigin>(kOriginUnspecified), 3);
    CHECK_EQ(odd.unspecified(), 3ull);
    CHECK_EQ(odd.human(), 0ull);
    CHECK_EQ(odd.agent(), 0ull);
    CHECK_EQ(odd.total(), 3ull);
}

// ── 4. 저널 → prooflog 까지 출처가 살아 있는가 ────────────────────────────

MARI_TEST(prooflog_reports_origin_counts_without_grading) {
    const std::string path = tmpPath("mix");
    auto pub = sigan::SiganPublisher::open(sigan::PublisherConfig{path, "mari-paint/0.1.0", 7});
    CHECK(pub.ok());
    auto& p = *pub.value();

    auto gate = AgentStrokeGate::open("claude/opus-5");
    CHECK(gate.ok());

    // 사람 획 3개 + AI 획 1개.
    const StrokeSource sources[] = {StrokeSource::humanPen(), StrokeSource::humanPen(),
                                    gate.value().source(), StrokeSource::humanMouse()};
    for (const StrokeSource& src : sources) {
        sigan::StrokeSample s(src);
        s.layerId = 1;
        s.flags = sigan::frameFlags(sigan::FrameFlag::Down);
        p.publish(s);
        s.flags = sigan::frameFlags(sigan::FrameFlag::Move);
        s.pos.x += 1.0f;
        p.publish(s);
        s.flags = sigan::frameFlags(sigan::FrameFlag::Up);
        p.publish(s);
        p.endStroke();
    }
    CHECK_EQ(p.stats().origins.human(), 3ull);
    CHECK_EQ(p.stats().origins.agent(), 1ull);
    const u64 sessionId = p.sessionId();
    pub.value().reset();

    auto scan = sigan::recoverJournal(path);
    CHECK(scan.ok());
    const auto strokes = sigan::summarizeStrokes(scan.value());
    CHECK_EQ(strokes.size(), static_cast<usize>(4));
    const auto st = sigan::originStats(strokes);
    CHECK_EQ(st.count(StrokeOrigin::HumanPen), 2ull);
    CHECK_EQ(st.count(StrokeOrigin::HumanMouse), 1ull);
    CHECK_EQ(st.agent(), 1ull);
    CHECK_EQ(st.unspecified(), 0ull); // 출처 없는 획은 하나도 없다

    sigan::ProofLogInput in;
    in.sessionId = sessionId;
    in.scan = &scan.value();
    in.agents.push_back(gate.value().agentId());
    auto log = sigan::buildProofLog(in);
    CHECK(log.ok());
    const std::string& json = log.value();

    CHECK(contains(json, "\"humanStrokes\": 3"));
    CHECK(contains(json, "\"agentStrokes\": 1"));
    CHECK(contains(json, "\"agentStrokeRatio\": 0.250"));
    CHECK(contains(json, "\"humanPen\": 2"));
    CHECK(contains(json, "\"humanMouse\": 1"));
    CHECK(contains(json, "\"unspecified\": 0"));
    CHECK(contains(json, "\"origin\": \"agent\""));
    CHECK(contains(json, "claude/opus-5"));

    // 🔴 판정은 Sigan 의 몫이다. Mari 는 숫자만 싣는다(docs/03 2절 · docs/05 3.2).
    CHECK(!contains(json, "human-only"));
    CHECK(!contains(json, "ai-assisted"));
    CHECK(!contains(json, "ai-generated"));
    CHECK(!contains(json, "signature"));
    CHECK(!contains(json, "ES256"));
    CHECK(contains(json, "\"grade\": \"unsigned\"")); // 등급은 여전히 이것 하나뿐이다
    std::filesystem::remove(path);
}

MARI_TEST_MAIN()
