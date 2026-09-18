// 🔴 **진짜 프로세스**를 띄워서 검증한다.
//
// 인프로세스 테스트(test_runner.cpp)가 이미 어댑터 로직을 덮지만, 그것만으로는
// "빌드된 바이너리가 실제로 돌아간다"를 증명하지 못한다. docs/05 2.8 이 요구하는 것은
// 라이브러리가 아니라 **실행 가능한 헤드리스 바이너리**다. 그래서 여기서는
// exec 해서 stdout/stderr/종료 코드를 그대로 본다.
//
// 이 파일 이름이 `test_*.cpp` 가 아닌 이유: tests/ 의 자동 발견에 걸리면 바이너리 경로를
// 컴파일 타임에 넣을 수 없다. cli/CMakeLists.txt 가 직접 등록하고 MARI_PAINT_BIN 을 박는다.
#include <mari/agent/json.hpp>
#include <mari/ora/ora.hpp>
#include <mari/test/harness.hpp>
#include <mari/test/sys.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef MARI_PAINT_BIN
#  error "MARI_PAINT_BIN 이 정의되지 않았다 (cli/CMakeLists.txt 가 박아 준다)"
#endif

using namespace mari;
using mari::agent::Json;

namespace {

using Run = mari::test::ProcessResult;

std::string slurp(const std::filesystem::path& p) {
    return mari::test::slurpFile(p);
}

/// 작업 디렉터리 하나를 잡고 그 안에서 바이너리를 돌린다.
/// stdout/stderr 를 따로 파일로 받아 **섞이지 않은 상태**로 본다 — 그게 이 테스트의 핵심이다.
/// (OS 별 차이는 mari/test/sys.hpp 가 흡수한다.)
Run run(const std::filesystem::path& dir, const std::string& args) {
    return mari::test::runProcess(MARI_PAINT_BIN, dir, args);
}

std::filesystem::path workDir(const char* tag) {
    static unsigned counter = 0;
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("mari-paint-bin-" + std::string(tag) + "-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary);
    o << text;
}

/// stdout 이 **JSON 값 하나뿐**인지 본다. 파싱은 뒤에 쓰레기가 붙으면 실패하므로
/// 이 한 번의 파싱이 곧 "로그가 섞이지 않았다"의 증명이다.
Json requirePureJson(const std::string& stdoutText, mari::test::Context& ctx, const char* what) {
    const auto parsed = Json::parse(stdoutText);
    if (!parsed.ok()) {
        ctx.fail(__FILE__, __LINE__,
                 std::string(what) + ": stdout 이 순수 JSON 이 아니다 — " + parsed.message() +
                     " / 받은 것: " + stdoutText.substr(0, 200));
        return Json::null();
    }
    return parsed.value();
}

} // namespace

// ── --version / --capabilities ───────────────────────────────────────────

MARI_TEST(capabilities_is_valid_json_on_stdout) {
    const auto dir = workDir("caps");
    const Run r = run(dir, "--capabilities");
    CHECK_EQ(r.exitCode, 0);

    const Json caps = requirePureJson(r.out, mari_ctx, "capabilities");
    CHECK(caps.isObject());
    CHECK(caps["ops"].isArray());
    CHECK(caps["ops"].size() > 10u);

    // 🔴 자기 설명 안에 출처 규약이 있고, 그것이 "고를 수 없음"이라고 적혀 있어야 한다.
    CHECK(caps.has("origin"));
    CHECK_EQ(caps["origin"]["forced"].asString(), std::string("agent"));
    CHECK(!caps["origin"]["settable"].asBool(true));

    // 🔴 경계선(docs/03 2절): 능력 목록에 서명·봉인·등급이 있으면 안 된다.
    const std::string text = caps.dump();
    for (const char* forbidden : {"ES256", "ecdsa", "signature", "sealChain", "chainHash",
                                  "human-only", "ai-assisted", "ai-generated"}) {
        CHECK(text.find(forbidden) == std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(version_and_capabilities_combine_into_one_json_value) {
    const auto dir = workDir("ver");
    const Run r = run(dir, "--version --capabilities");
    CHECK_EQ(r.exitCode, 0);
    const Json j = requirePureJson(r.out, mari_ctx, "version+capabilities");
    CHECK_EQ(j["name"].asString(), std::string("mari-paint"));
    CHECK(!j["version"].asString().empty());
    CHECK(j["capabilities"]["ops"].isArray());
    CHECK(j["headless"].asBool());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ── 스크립트 한 벌 ───────────────────────────────────────────────────────

MARI_TEST(script_creates_paints_and_saves_a_real_ora_that_opens_again) {
    const auto dir = workDir("script");
    writeText(dir / "paint.json", R"({
      "agentId": "cli-e2e",
      "onError": "stop",
      "ops": [
        { "op": "doc.create", "width": 128, "height": 128 },
        { "op": "fill", "region": [0,0,128,128], "color": "#ffffff" },
        { "op": "layer.add", "name": "line" },
        { "op": "stroke", "points": [[16,16],[64,90],[112,24]], "size": 9,
          "color": "#203040", "pressureProfile": "taper-in-out", "view": "dirty" },
        { "op": "doc.describe", "view": "none" },
        { "op": "doc.save", "path": "art.ora" }
      ]
    })");

    const Run r = run(dir, "--headless --script paint.json --out-dir shots");
    CHECK_EQ(r.exitCode, 0);

    const Json report = requirePureJson(r.out, mari_ctx, "script");
    CHECK(report["ok"].asBool());
    CHECK_EQ(report["failed"].asInt(), 0);
    CHECK_EQ(report["opCount"].asInt(), 6);

    // 🔴 파일이 **실제로** 생겼고, 우리 리더로 다시 열린다.
    const std::filesystem::path ora = dir / "art.ora";
    CHECK(std::filesystem::exists(ora));
    CHECK(std::filesystem::file_size(ora) > 0u);
    const auto loaded = ora::load(ora.string());
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.value().tree->canvasSize().width, 128);
        CHECK_EQ(loaded.value().tree->canvasSize().height, 128);
        CHECK_EQ(loaded.value().tree->roots().size(), 2u);
    }

    // 저장 응답에 해시가 실린다(64자 16진). 🔴 계산일 뿐 봉인이 아니다(docs/03 2절).
    const Json& saved = report["results"].at(5)["result"];
    CHECK_EQ(saved["path"].asString(), std::string("art.ora"));
    CHECK(saved["canvasHash"].asString().size() == 64u);

    // --out-dir 에 PNG 가 떨어졌고 stdout 에는 base64 가 없다.
    CHECK(std::filesystem::exists(dir / "shots"));
    usize pngs = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir / "shots")) {
        if (e.path().extension() == ".png") {
            ++pngs;
        }
    }
    CHECK(pngs >= 1u);
    // base64 픽셀은 stdout 에 없다(`"format":"png"` 같은 메타데이터는 남아도 된다).
    CHECK(r.out.find("\"png\": \"") == std::string::npos);
    CHECK(r.out.find("\"png\":\"") == std::string::npos);

    // 🔴 출처가 보고된다. 사람 획 0, AI 획은 붓질 수만큼(agent-api 는 fill 도 센다).
    //    여기서 고정 숫자를 박지 않는 이유: 무엇을 "획"으로 셀지는 능력 계층의 결정이고,
    //    CLI 가 그걸 다시 판정하면 두 벌이 된다. CLI 가 보장할 것은 **사람 0** 이다.
    const Json& origins = report["strokeOrigins"];
    CHECK_EQ(origins["agentId"].asString(), std::string("cli-e2e"));
    CHECK(origins["agentStrokes"].asInt() >= 1);
    CHECK_EQ(origins["humanStrokes"].asInt(), 0);
    CHECK_EQ(origins["originCounts"]["humanPen"].asInt(), 0);
    CHECK_EQ(origins["originCounts"]["humanMouse"].asInt(), 0);
    CHECK_EQ(origins["originCounts"]["unspecified"].asInt(), 0);
    CHECK(origins["agentStrokeRatio"].asNumber() > 0.99);

    // 진행 상황은 stderr 에만 있다.
    CHECK(r.err.find("[1/6]") != std::string::npos);
    CHECK(r.err.find("doc.save") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(saved_file_reopens_through_the_cli_itself) {
    const auto dir = workDir("reopen");
    writeText(dir / "make.json", R"([
      { "op": "doc.create", "width": 64, "height": 64 },
      { "op": "fill", "region": [0,0,64,64], "color": "#123456" },
      { "op": "doc.save", "path": "a.ora" }
    ])");
    const Run made = run(dir, "--headless --script make.json --quiet");
    CHECK_EQ(made.exitCode, 0);
    CHECK(std::filesystem::exists(dir / "a.ora"));

    const Run reopened =
        run(dir, "--headless --quiet --exec "
                 "'[{\"op\":\"doc.open\",\"path\":\"a.ora\"},"
                 "{\"op\":\"doc.describe\",\"view\":\"none\"}]'");
    CHECK_EQ(reopened.exitCode, 0);
    const Json j = requirePureJson(reopened.out, mari_ctx, "reopen");
    CHECK(j["ok"].asBool());
    CHECK_EQ(j["results"].at(0)["ok"].asBool(false), true);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ── 실패 경로 ────────────────────────────────────────────────────────────

MARI_TEST(failing_script_exits_nonzero_with_the_reason_on_stderr) {
    const auto dir = workDir("fail");
    writeText(dir / "bad.json", R"([
      { "op": "doc.create", "width": 32, "height": 32 },
      { "op": "definitely.not.an.op" },
      { "op": "doc.create", "width": 32, "height": 32 }
    ])");
    const Run r = run(dir, "--headless --script bad.json");

    // 🔴 실패는 종료 코드로 드러난다. "실패했는데 0" 이 제일 나쁘다.
    CHECK_EQ(r.exitCode, 1);
    // 🔴 이유는 stderr 에 있다.
    CHECK(r.err.find("definitely.not.an.op") != std::string::npos);
    CHECK(r.err.find("실패") != std::string::npos);

    // stdout 은 실패해도 여전히 순수 JSON 이다(파이프가 살아 있어야 한다).
    const Json report = requirePureJson(r.out, mari_ctx, "failing script");
    CHECK(!report["ok"].asBool(true));
    CHECK_EQ(report["failed"].asInt(), 1);
    CHECK(report["stopped"].asBool());
    CHECK_EQ(report["executed"].asInt(), 2); // 세 번째는 돌지 않았다

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(continue_on_error_runs_everything_and_still_exits_nonzero) {
    const auto dir = workDir("cont");
    writeText(dir / "bad.json", R"([
      { "op": "doc.create", "width": 32, "height": 32 },
      { "op": "definitely.not.an.op" },
      { "op": "doc.describe", "view": "none" }
    ])");
    const Run r = run(dir, "--headless --script bad.json --continue-on-error --quiet");
    CHECK_EQ(r.exitCode, 1);
    const Json report = requirePureJson(r.out, mari_ctx, "continue");
    CHECK_EQ(report["executed"].asInt(), 3);
    CHECK_EQ(report["failed"].asInt(), 1);
    CHECK(r.err.empty()); // --quiet

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(broken_json_and_bad_usage_never_print_to_stdout) {
    const auto dir = workDir("usage");

    // 깨진 스크립트 JSON → 실행 실패(1). stdout 은 비어 있다(반쪽 JSON 을 뱉지 않는다).
    writeText(dir / "broken.json", "{ this is not json");
    const Run broken = run(dir, "--script broken.json");
    CHECK_EQ(broken.exitCode, 1);
    CHECK(broken.out.empty());
    CHECK(!broken.err.empty());

    // 없는 파일 → 실행 실패(1).
    const Run missing = run(dir, "--script nope.json");
    CHECK_EQ(missing.exitCode, 1);
    CHECK(missing.out.empty());

    // 모르는 옵션 → 사용법 오류(2).
    const Run typo = run(dir, "--srcipt x.json");
    CHECK_EQ(typo.exitCode, 2);
    CHECK(typo.out.empty());
    CHECK(typo.err.find("모르는 옵션") != std::string::npos);

    // 할 일이 없으면 사용법 오류(2). 조용히 0 으로 끝나지 않는다.
    const Run nothing = run(dir, "--headless");
    CHECK_EQ(nothing.exitCode, 2);
    CHECK(nothing.out.empty());

    // 🔴 익명 에이전트로는 열리지 않는다.
    const Run anon = run(dir, "--headless --agent-id '' --exec '{\"op\":\"capabilities\"}'");
    CHECK(anon.exitCode != 0);
    CHECK(anon.out.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(stdout_stays_pure_json_even_with_images_and_progress) {
    // 🔴 이 테스트가 "파이프가 된다"의 정본이다.
    //    이미지를 base64 로 싣고 진행 출력을 켠 채로도 stdout 은 값 하나뿐이어야 한다.
    const auto dir = workDir("pure");
    writeText(dir / "s.json", R"([
      { "op": "doc.create", "width": 32, "height": 32 },
      { "op": "fill", "region": [0,0,32,32], "color": "#ff8800", "view": "full" }
    ])");
    const Run r = run(dir, "--headless --script s.json --embed-images --pretty");
    CHECK_EQ(r.exitCode, 0);
    CHECK(!r.err.empty()); // 진행 출력은 살아 있다
    const Json j = requirePureJson(r.out, mari_ctx, "pure stdout");
    CHECK(j["results"].at(1)["image"]["png"].asString().size() > 16u);
    // 진행 문구가 stdout 으로 새지 않았다.
    CHECK(r.out.find("[1/2]") == std::string::npos);
    CHECK(r.out.find("헤드리스 —") == std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST_MAIN()
