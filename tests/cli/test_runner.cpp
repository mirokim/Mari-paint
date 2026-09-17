// CLI 어댑터 — 인자 해석 · 스크립트 해석 · 이미지 떨구기 · 순차 실행.
//
// 🔴 여기서 검증하는 것은 **어댑터의 책임**뿐이다. 연산 자체의 의미(스트로크가
//    픽셀을 칠하는가 등)는 agent-api 의 테스트가 본다. 같은 것을 두 번 재지 않는다.
#include <mari/cli/runner.hpp>
#include <mari/ora/image.hpp>
#include <mari/ora/ora.hpp>
#include <mari/test/harness.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace mari;
using mari::agent::Json;
using mari::cli::CliOptions;

namespace {

std::vector<std::string> argv(std::initializer_list<const char*> a) {
    return std::vector<std::string>(a.begin(), a.end());
}

/// 테스트마다 고유한 임시 디렉터리. 남은 파일이 다음 테스트를 오염시키지 않게 한다.
std::filesystem::path tempDir(const char* tag) {
    static unsigned counter = 0;
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("mari-cli-" + std::string(tag) + "-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

Json op(const char* name) {
    Json j = Json::object();
    j.set("op", Json::string(name));
    return j;
}

} // namespace

// ── 인자 해석 ────────────────────────────────────────────────────────────

MARI_TEST(args_reject_unknown_option) {
    // 🔴 오타를 조용히 무시하면 스크립트가 "성공했다"고 믿고 넘어간다.
    const auto bad = cli::parseArgs(argv({"--srcipt", "a.json"}));
    CHECK(!bad.ok());
    CHECK(bad.code() == ErrorCode::InvalidArgument);

    const auto ok = cli::parseArgs(argv({"--headless", "--script", "a.json"}));
    CHECK(ok.ok());
    CHECK(ok.value().headless);
    CHECK_EQ(ok.value().scriptPath, std::string("a.json"));
}

MARI_TEST(args_equals_form_and_missing_value) {
    const auto eq = cli::parseArgs(argv({"--script=a.json", "--out-dir=/tmp/x"}));
    CHECK(eq.ok());
    CHECK_EQ(eq.value().scriptPath, std::string("a.json"));
    CHECK_EQ(eq.value().outDir, std::string("/tmp/x"));

    const auto missing = cli::parseArgs(argv({"--script"}));
    CHECK(!missing.ok());
}

MARI_TEST(args_refuse_anonymous_agent_and_multiple_modes) {
    // 🔴 익명 에이전트 획은 만들지 않는다(docs/05 3.1).
    const auto anon = cli::parseArgs(argv({"--agent-id", "", "--exec", "{}"}));
    CHECK(!anon.ok());

    const auto both = cli::parseArgs(argv({"--script", "a.json", "--exec", "{}"}));
    CHECK(!both.ok());
}

MARI_TEST(args_have_no_origin_switch) {
    // 🔴 CLI 표면 어디에도 origin 을 고르는 옵션이 없어야 한다.
    //    있으면 그게 위조 수단이다. 사용법 텍스트와 파서 양쪽을 본다.
    for (const char* forged : {"--origin", "--human", "--as-human", "--stroke-origin",
                               "--origin=humanPen"}) {
        const auto r = cli::parseArgs(argv({forged}));
        CHECK(!r.ok());
    }
    const std::string usage = cli::usageText();
    CHECK(usage.find("--origin") == std::string::npos);
    CHECK(usage.find("--as-human") == std::string::npos);
    // 반대로, 출처가 강제된다는 사실은 사용법에 **적혀 있어야** 한다.
    CHECK(usage.find("origin=agent") != std::string::npos);
}

MARI_TEST(view_default_is_none_until_there_is_somewhere_to_put_it) {
    // stdout 은 파이프다. 아무도 안 물어봤는데 base64 를 흘리지 않는다.
    const auto plain = cli::parseArgs(argv({"--exec", "{}"}));
    CHECK(plain.ok());
    CHECK_EQ(plain.value().view, std::string("none"));

    const auto spill = cli::parseArgs(argv({"--exec", "{}", "--out-dir", "/tmp/x"}));
    CHECK(spill.ok());
    CHECK_EQ(spill.value().view, std::string("dirty"));

    const auto explicitView = cli::parseArgs(argv({"--exec", "{}", "--view", "full"}));
    CHECK(explicitView.ok());
    CHECK_EQ(explicitView.value().view, std::string("full"));
}

// ── 스크립트 해석 ────────────────────────────────────────────────────────

MARI_TEST(script_accepts_array_and_object_forms) {
    const auto arr = Json::parse(R"([{"op":"capabilities"},{"op":"doc.describe"}])");
    CHECK(arr.ok());
    const auto s1 = cli::parseScript(arr.value());
    CHECK(s1.ok());
    CHECK_EQ(s1.value().ops.size(), 2u);
    CHECK(!s1.value().hasOnError);

    const auto obj = Json::parse(
        R"({"agentId":"tester","onError":"continue","ops":[{"op":"capabilities"}]})");
    CHECK(obj.ok());
    const auto s2 = cli::parseScript(obj.value());
    CHECK(s2.ok());
    CHECK_EQ(s2.value().agentId, std::string("tester"));
    CHECK(s2.value().hasOnError);
    CHECK(s2.value().continueOnError);
}

MARI_TEST(script_rejects_garbage) {
    const auto noOps = Json::parse(R"({"agentId":"x"})");
    CHECK(noOps.ok());
    CHECK(!cli::parseScript(noOps.value()).ok());

    const auto badOnError = Json::parse(R"({"onError":"maybe","ops":[]})");
    CHECK(badOnError.ok());
    CHECK(!cli::parseScript(badOnError.value()).ok());

    CHECK(!cli::parseScript(Json::string("nope")).ok());
}

// ── 순차 실행 ────────────────────────────────────────────────────────────

MARI_TEST(run_ops_stops_on_error_unless_told_otherwise) {
    auto session = agent::AgentSession::open("runner-test");
    CHECK(session.ok());

    std::vector<Json> ops;
    ops.push_back(op("capabilities"));
    ops.push_back(op("no.such.op"));
    ops.push_back(op("capabilities"));

    CliOptions o;
    o.view = "none";
    o.quiet = true;
    std::ostringstream err;
    usize failed = 0;
    const Json report = cli::runOps(*session.value(), ops, o, err, failed);
    CHECK_EQ(failed, 1u);
    CHECK(!report["ok"].asBool());
    CHECK(report["stopped"].asBool());
    CHECK_EQ(report["executed"].asInt(), 2); // 세 번째는 실행되지 않았다

    o.continueOnError = true;
    auto session2 = agent::AgentSession::open("runner-test");
    CHECK(session2.ok());
    std::ostringstream err2;
    usize failed2 = 0;
    const Json report2 = cli::runOps(*session2.value(), ops, o, err2, failed2);
    CHECK_EQ(failed2, 1u);
    CHECK_EQ(report2["executed"].asInt(), 3);
    CHECK(!report2["stopped"].asBool());
}

MARI_TEST(progress_goes_to_stderr_only) {
    // 🔴 CLI 의 핵심 규약: 진행 출력은 runOps 가 받은 스트림(=stderr)에만 간다.
    //    결과 JSON 은 반환값이다 — 두 길이 섞이지 않는다.
    auto session = agent::AgentSession::open("runner-test");
    CHECK(session.ok());
    std::vector<Json> ops;
    ops.push_back(op("capabilities"));

    CliOptions o;
    o.view = "none";
    std::ostringstream err;
    usize failed = 0;
    const Json report = cli::runOps(*session.value(), ops, o, err, failed);
    CHECK_EQ(failed, 0u);
    CHECK(err.str().find("capabilities ok") != std::string::npos);
    // 반환 JSON 안에는 진행 문구가 없다.
    const std::string dumped = report.dump();
    CHECK(dumped.find("ok (") == std::string::npos);

    // --quiet 면 stderr 도 조용하다.
    std::ostringstream quietErr;
    o.quiet = true;
    usize f2 = 0;
    (void)cli::runOps(*session.value(), ops, o, quietErr, f2);
    CHECK(quietErr.str().empty());
}

MARI_TEST(run_ops_always_reports_stroke_origins) {
    // 🔴 출처 집계는 물어봐야 나오는 값이 아니다. 항상 붙는다(docs/05 3.2).
    auto session = agent::AgentSession::open("origin-reporter");
    CHECK(session.ok());

    const auto script = Json::parse(R"([
        {"op":"doc.create","width":64,"height":64},
        {"op":"stroke","points":[[8,8],[40,40]],"size":6,"color":"#102030"}
    ])");
    CHECK(script.ok());
    const auto parsedScript = cli::parseScript(script.value());
    CHECK(parsedScript.ok());

    CliOptions o;
    o.view = "none";
    o.quiet = true;
    std::ostringstream err;
    usize failed = 0;
    const Json report = cli::runOps(*session.value(), parsedScript.value().ops, o, err, failed);
    CHECK_EQ(failed, 0u);

    const Json& origins = report["strokeOrigins"];
    CHECK(origins.isObject());
    CHECK_EQ(origins["agentId"].asString(), std::string("origin-reporter"));
    CHECK_EQ(origins["agentStrokes"].asInt(), 1);
    CHECK_EQ(origins["humanStrokes"].asInt(), 0);
    CHECK_EQ(origins["originCounts"]["humanPen"].asInt(), 0);
    CHECK_EQ(origins["originCounts"]["unspecified"].asInt(), 0);
    CHECK(origins["agentStrokeRatio"].asNumber() > 0.99);
    // 🔴 등급 문자열은 어디에도 없다 — 판정은 Sigan 의 몫이다(docs/03 2절).
    const std::string dumped = report.dump();
    CHECK(dumped.find("human-only") == std::string::npos);
    CHECK(dumped.find("ai-assisted") == std::string::npos);
    CHECK(dumped.find("ai-generated") == std::string::npos);
}

MARI_TEST(cli_never_overrides_a_view_the_op_asked_for) {
    auto session = agent::AgentSession::open("view-test");
    CHECK(session.ok());
    const auto script = Json::parse(R"([
        {"op":"doc.create","width":32,"height":32},
        {"op":"fill","region":[0,0,32,32],"color":"#ffffff","view":"none"}
    ])");
    CHECK(script.ok());
    const auto sc = cli::parseScript(script.value());
    CHECK(sc.ok());

    CliOptions o;
    o.view = "full"; // CLI 기본값은 full 이지만…
    o.quiet = true;
    std::ostringstream err;
    usize failed = 0;
    const Json report = cli::runOps(*session.value(), sc.value().ops, o, err, failed);
    CHECK_EQ(failed, 0u);
    // …연산이 직접 "none" 이라고 했으므로 이미지가 없어야 한다.
    CHECK(!report["results"].at(1).has("image"));
}

// ── 이미지 떨구기 ────────────────────────────────────────────────────────

MARI_TEST(out_dir_spills_png_and_strips_base64_from_stdout) {
    const std::filesystem::path dir = tempDir("spill");
    auto session = agent::AgentSession::open("spill-test");
    CHECK(session.ok());

    const auto script = Json::parse(R"([
        {"op":"doc.create","width":40,"height":24},
        {"op":"fill","region":[0,0,40,24],"color":"#3366cc"}
    ])");
    CHECK(script.ok());
    const auto sc = cli::parseScript(script.value());
    CHECK(sc.ok());

    CliOptions o;
    o.view = "full";
    o.outDir = dir.string();
    o.quiet = true;
    o.embedImages = false;
    std::ostringstream err;
    usize failed = 0;
    const Json report = cli::runOps(*session.value(), sc.value().ops, o, err, failed);
    CHECK_EQ(failed, 0u);

    const Json& image = report["results"].at(1)["image"];
    CHECK(image.isObject());
    // base64 는 빠지고 경로가 들어간다 — 같은 픽셀을 두 번 흘리지 않는다.
    CHECK(!image.has("png"));
    CHECK(image.has("path"));

    const std::string path = image["path"].asString();
    CHECK(std::filesystem::exists(path));
    // 실제로 디코드되는 PNG 이고, 칠한 색이 들어 있다.
    const auto bytes = ora::readFileBytes(path);
    CHECK(bytes.ok());
    const auto decoded = ora::decodePng(bytes.value().data(), bytes.value().size());
    CHECK(decoded.ok());
    CHECK_EQ(decoded.value().width, 40);
    CHECK_EQ(decoded.value().height, 24);
    CHECK_EQ(static_cast<int>(decoded.value().pixels[0]), 0x33);
    CHECK_EQ(static_cast<int>(decoded.value().pixels[1]), 0x66);
    CHECK_EQ(static_cast<int>(decoded.value().pixels[2]), 0xcc);

    // stdout 으로 나갈 문자열에 base64 덩어리가 없다.
    // (`"format":"png"` 같은 메타데이터는 남아도 된다 — 문제는 픽셀을 두 번 흘리는 것이다.)
    const std::string dumped = report.dump();
    CHECK(dumped.find("\"png\":\"") == std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(embed_images_keeps_base64_next_to_the_file) {
    const std::filesystem::path dir = tempDir("embed");
    auto session = agent::AgentSession::open("embed-test");
    CHECK(session.ok());
    const auto script = Json::parse(R"([
        {"op":"doc.create","width":16,"height":16},
        {"op":"fill","region":[0,0,16,16],"color":"#ffffff"}
    ])");
    CHECK(script.ok());
    const auto sc = cli::parseScript(script.value());
    CHECK(sc.ok());

    CliOptions o;
    o.view = "full";
    o.outDir = dir.string();
    o.embedImages = true;
    o.quiet = true;
    std::ostringstream err;
    usize failed = 0;
    const Json report = cli::runOps(*session.value(), sc.value().ops, o, err, failed);
    CHECK_EQ(failed, 0u);
    const Json& image = report["results"].at(1)["image"];
    CHECK(image.has("png"));
    CHECK(image.has("path"));
    CHECK(std::filesystem::exists(image["path"].asString()));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(spill_image_is_a_no_op_without_out_dir) {
    Json env = Json::object();
    env.set("op", Json::string("render"));
    Json img = Json::object();
    img.set("png", Json::string("AAAA"));
    env.set("image", std::move(img));

    CHECK(cli::spillImage(env, "", 0, false).ok());
    CHECK(env["image"].has("png")); // 아무 것도 건드리지 않았다
    CHECK(!env["image"].has("path"));
}

MARI_TEST_MAIN()
