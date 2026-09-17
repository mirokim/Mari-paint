// CLI·MCP 로 기록을 꺼내 보는 길 (docs/06 5번 작업 — `--proof-out` · origin 집계 조회)
//
// 여기서 지키는 것:
//   · `--journal-dir` 를 주면 기록이 켜지고, `--proof-out` 으로 파일이 떨어진다
//   · 그 파일에 **세 축**이 다 들어 있다(붓질 · 영역 연산 · 변경 타일)
//   · stdout 리포트도 세 축을 말한다 — 획 수 한 줄만 내보내지 않는다(docs/06 6절 H3)
//   · 기록을 안 켜면 그리기는 되고 "기록 안 됨"이라고 **말한다**
//   · MCP 도구 목록에 origin 집계 조회가 **자동으로** 들어 있다(표가 정본이므로)
//
// 🔴 만들어지는 파일은 **인증서가 아니다.** 등급은 "unsigned" 하나뿐이다(docs/03 2절).
#include <mari/agent/session.hpp>
#include <mari/cli/runner.hpp>
#include <mari/mcp/tools.hpp>
#include <mari/ora/ora.hpp>
#include <mari/test/harness.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace mari;
using mari::agent::Json;

namespace {

std::vector<std::string> argv(std::initializer_list<const char*> a) {
    return std::vector<std::string>(a.begin(), a.end());
}

std::filesystem::path tempDir(const char* tag) {
    static unsigned counter = 0;
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("mari-proof-" + std::string(tag) + "-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

std::string readText(const std::filesystem::path& p) {
    Result<std::vector<u8>> b = ora::readFileBytes(p.string());
    if (!b.ok()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(b.value().data()), b.value().size());
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

const char* kScript =
    R"([{"op":"doc.create","width":128,"height":128},)"
    R"({"op":"stroke","points":[[8,8],[40,40]],"size":6,"color":"#102030"},)"
    R"({"op":"fill","region":"canvas","color":"#f2e9dc"}])";

} // namespace

MARI_TEST(proof_out_writes_all_three_axes) {
    const auto dir = tempDir("out");
    const std::string journalDir = (dir / "journals").string();
    const std::string proofPath = (dir / "proof.json").string();

    std::ostringstream out;
    std::ostringstream err;
    const int rc = cli::runCli(argv({"--headless", "--quiet", "--view", "none", "--agent-id",
                                     "cli-proof", "--journal-dir", journalDir.c_str(),
                                     "--proof-out", proofPath.c_str(), "--exec", kScript}),
                               out, err);
    CHECK_EQ(rc, cli::kExitOk);

    Result<Json> report = Json::parse(out.str());
    CHECK(report.ok());
    if (!report.ok()) {
        return;
    }
    const Json& root = report.value();
    CHECK(root["ok"].asBool());

    // 🔴 stdout 리포트가 세 축을 다 말한다. 붓질 1 · 영역 연산 1 — 같은 칸이 아니다.
    const Json& origins = root["strokeOrigins"];
    CHECK_EQ(origins["agentStrokes"].asInt(), 1);
    CHECK_EQ(origins["humanStrokes"].asInt(), 0);
    CHECK_EQ(origins["regionOps"]["agent"].asInt(), 1);
    CHECK(origins["changedTiles"].asInt() > 0);
    CHECK(origins["recorded"].asBool());
    CHECK_EQ(origins["documentSegments"]["agentBrushStrokes"].asInt(), 1);
    CHECK_EQ(origins["documentSegments"]["agentRegionOps"].asInt(), 1);
    CHECK_EQ(origins["documentSegments"]["humanBrushStrokes"].asInt(), 0);

    // 파일이 실제로 떨어졌다.
    CHECK_EQ(root["proofLogs"].size(), static_cast<usize>(1));
    CHECK(std::filesystem::exists(proofPath));
    const std::string log = readText(proofPath);
    CHECK(!log.empty());
    CHECK(contains(log, "\"agentStrokes\": 1"));
    CHECK(contains(log, "\"agentRegionOps\": 1"));
    CHECK(contains(log, "\"humanStrokes\": 0"));
    CHECK(contains(log, "\"synthetic\": true"));  // fill 은 붓질이 아니라고 적혀 있다
    CHECK(contains(log, "\"synthetic\": false")); // 붓질은 붓질이라고 적혀 있다
    CHECK(contains(log, "\"changedTiles\""));
    CHECK(contains(log, "cli-proof"));
    CHECK(contains(log, "\"lostFrames\": 0")); // 유실 0

    // 🔴 인증서가 아니다. 등급은 하나뿐이고 서명은 없다(docs/03 2절 경계선).
    CHECK(contains(log, "\"grade\": \"unsigned\""));
    CHECK(contains(log, "\"signed\": false"));
    CHECK(!contains(log, "ai-assisted"));
    CHECK(!contains(log, "human-only"));
    CHECK(!contains(log, "ES256"));
    CHECK(!contains(log, "prevHash"));

    // 저널도 그 자리에 있다. 정본은 저널이다.
    CHECK_EQ(std::filesystem::path(root["proofLogs"].at(0)["journal"].asString())
                 .parent_path()
                 .string(),
             journalDir);
}

MARI_TEST(without_journal_dir_drawing_works_and_says_it_is_not_recorded) {
    // 🔴 Sigan 미설치·기록 미사용은 정상 상태다(docs/03 5.1). 그리기는 그대로 된다.
    //    다만 기록되지 않았다는 사실을 **숨기지 않는다.**
    std::ostringstream out;
    std::ostringstream err;
    const int rc = cli::runCli(
        argv({"--headless", "--quiet", "--view", "none", "--agent-id", "cli-plain", "--exec",
              kScript}),
        out, err);
    CHECK_EQ(rc, cli::kExitOk);

    Result<Json> report = Json::parse(out.str());
    CHECK(report.ok());
    if (!report.ok()) {
        return;
    }
    CHECK(report.value()["ok"].asBool());
    CHECK(!report.value()["strokeOrigins"]["recorded"].asBool());
    CHECK(!report.value().has("proofLogs"));
    // 그래도 세션 집계는 선다 — 세션은 자기가 한 일을 안다.
    CHECK_EQ(report.value()["strokeOrigins"]["agentStrokes"].asInt(), 1);
    CHECK_EQ(report.value()["strokeOrigins"]["regionOps"]["agent"].asInt(), 1);
}

MARI_TEST(mcp_exposes_the_origin_tally_tool) {
    // 🔴 도구 목록을 손으로 유지하지 않는다(docs/05 4절). 연산 표에 넣었으니
    //    MCP 도구는 **저절로** 생긴다. 그 사실을 여기서 못박는다.
    const Json tools = mcp::toolsArray();
    bool found = false;
    for (usize i = 0; i < tools.size(); ++i) {
        if (tools.at(i)["name"].asString() == "doc_origins") {
            found = true;
            // 설명 어디에도 등급 문자열이 없다.
            const std::string desc = tools.at(i)["description"].asString();
            CHECK(!contains(desc, "ai-assisted"));
            CHECK(!contains(desc, "human-only"));
        }
    }
    CHECK(found);

    // 실제로 불러 보면 세 축이 다 온다.
    auto opened = agent::AgentSession::open("mcp-tally");
    CHECK(opened.ok());
    agent::AgentSession& s = *opened.value();
    Json create = Json::object();
    create.set("op", Json::string("doc.create"));
    create.set("width", Json::integer(64));
    create.set("height", Json::integer(64));
    create.set("view", Json::string("none"));
    CHECK(s.execute(create)["ok"].asBool());
    Json fill = Json::object();
    fill.set("op", Json::string("fill"));
    fill.set("region", Json::string("canvas"));
    fill.set("view", Json::string("none"));
    CHECK(s.execute(fill)["ok"].asBool());

    Json ask = Json::object();
    ask.set("op", Json::string("doc.origins"));
    ask.set("view", Json::string("none"));
    const Json got = s.execute(ask);
    CHECK(got["ok"].asBool());
    const Json& r = got["result"];
    CHECK_EQ(r["brushStrokes"]["agent"].asInt(), 0);  // 축 A — 붓질 0
    CHECK_EQ(r["regionOps"]["agent"].asInt(), 1);     // 축 B — 영역 연산 1
    CHECK_EQ(r["changedTiles"]["agent"].asInt(), 1);  // 축 C — 64x64 = 타일 1개
    CHECK_EQ(r["brushStrokes"]["byOrigin"]["unspecified"].asInt(), 0);
    CHECK(!r["recorded"].asBool()); // 이 세션은 기록을 안 켰다. 그 사실도 말한다

    // 🔴 세 축을 하나로 합친 "기여도" 같은 값은 없다. 등급 문자열도 없다.
    const std::string dumped = got.dump();
    CHECK(!contains(dumped, "contribution"));
    CHECK(!contains(dumped, "ai-assisted"));
    CHECK(!contains(dumped, "human-only"));
    CHECK(!contains(dumped, "ai-generated"));
}

MARI_TEST_MAIN()
