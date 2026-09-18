// Mari Paint — 🔴 headless_parity (docs/05 6절 표) + docs/02 8절 실측
//
// docs/05 6절이 요구하는 통과 조건은 한 줄이다:
//   "헤드리스에서 GUI 와 동일한 연산 집합이 노출된다"
//
// 🔴 정직성 먼저 — 이 리포에 **GUI 는 없다.**
//    Qt 도 없고, 창을 여는 코드도 없고, Windows COM 래퍼는 Linux 에서 컴파일조차 안 된다
//    (docs/04 2절). 그러므로 이 테스트는 GUI 를 띄워 목록 두 개를 맞대 보지 **않는다.**
//    그건 지금 할 수 없는 일이고, 할 수 없는 일을 했다고 쓰면 그게 거짓말이다.
//
//    대신 **갈라질 수 없다는 구조**를 검사한다. docs/05 1절의 그림이 말하는 그대로다:
//
//              mari-agent-api   ← 능력은 **여기 하나뿐이다**
//         ┌───────┬───────┼────────┬─────────┐
//      MCP 서버  COM    CLI      JSON-RPC    UI
//
//    연산 표(`agent::opTable()`)가 하나뿐이고, 모든 표면이 그 표를 **읽기만** 하면
//    "사람은 되는데 AI 는 안 되는 것"이 생길 자리가 없다. 나중에 GUI 가 붙어도
//    같은 표를 읽는 한 parity 는 유지된다 — 이 테스트가 못 박는 것이 그 "하나뿐"이다.
//
//    그래서 검사는 넷이다:
//      ① 진짜 헤드리스 **프로세스**가 내보내는 연산 집합 == 인프로세스 `opTable()`
//      ② 화면이 아예 없는 환경(DISPLAY·WAYLAND_DISPLAY 제거)에서도 같은 집합
//      ③ 같은 바이너리의 MCP 표면(`--mcp --stdio`)도 같은 표에서 나온다
//      ④ 표에 **GUI 전용 연산이 없다** — 지원 연산은 전부 헤드리스에서 실행 가능하다
//      ⑤ CLI·MCP 소스에 **두 번째 연산 표가 없다** (있으면 그게 갈라짐의 씨앗이다)
//
// 뒷부분은 docs/02 8절 성능 목표 중 **화면 없이 잴 수 있는 것**을 실제로 잰다.
// 못 재는 것(펜 입력 → 화면 표시)은 못 잰다고 출력한다.
//
// 이 파일 이름이 `test_*.cpp` 가 아닌 이유는 cli_binary_main.cpp 와 같다 —
// 바이너리 경로를 컴파일 타임에 박아야 해서 cli/CMakeLists.txt 가 직접 등록한다.
#include <mari/agent/capabilities.hpp>
#include <mari/agent/json.hpp>
#include <mari/agent/session.hpp>
#include <mari/mcp/tools.hpp>
#include <mari/test/harness.hpp>
#include <mari/test/sys.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef MARI_PAINT_BIN
#  error "MARI_PAINT_BIN 이 정의되지 않았다 (cli/CMakeLists.txt 가 박아 준다)"
#endif

using namespace mari;
using mari::agent::AgentSession;
using mari::agent::Json;

namespace {

using Run = mari::test::ProcessResult;

std::string slurp(const std::filesystem::path& p) {
    return mari::test::slurpFile(p);
}

std::filesystem::path workDir(const char* tag) {
    static unsigned counter = 0;
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("mari-headless-" + std::string(tag) + "-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

/// 바이너리를 돌린다. `envPrefix` 에 `env -u DISPLAY` 같은 것을 끼워 넣을 수 있다.
/// (POSIX 전용. Windows 에는 DISPLAY 가 없어 무시된다 — mari/test/sys.hpp 참조.)
Run run(const std::filesystem::path& dir, const std::string& args,
        const std::string& envPrefix = std::string{}, const std::string& stdinFile = std::string{}) {
    return mari::test::runProcess(MARI_PAINT_BIN, dir, args, envPrefix, stdinFile);
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary);
    o << text;
}

std::string readWholeFile(const std::filesystem::path& p) {
    return slurp(p);
}

std::filesystem::path repoRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

/// 인프로세스 연산 표의 이름들. **정본이다.**
std::vector<std::string> tableOpNames() {
    std::vector<std::string> names;
    for (const agent::OpSpec& op : agent::opTable()) {
        names.emplace_back(op.name);
    }
    return names;
}

/// `--capabilities` 응답에서 연산 이름을 뽑는다.
std::vector<std::string> capsOpNames(const Json& caps) {
    std::vector<std::string> names;
    const Json& ops = caps["ops"];
    for (usize i = 0; i < ops.size(); ++i) {
        names.push_back(ops.at(i)["name"].asString());
    }
    return names;
}

/// 두 목록을 **순서까지** 비교하고, 어긋나면 어느 쪽에만 있는지 이름을 찍는다.
void expectSameOps(mari::test::Context& mari_ctx, const std::vector<std::string>& expected,
                   const std::vector<std::string>& actual, const char* what) {
    if (expected == actual) {
        return;
    }
    for (const std::string& e : expected) {
        if (std::find(actual.begin(), actual.end(), e) == actual.end()) {
            mari_ctx.fail(__FILE__, __LINE__,
                          std::string(what) + ": 표에는 있는데 안 보인다 — " + e);
        }
    }
    for (const std::string& a : actual) {
        if (std::find(expected.begin(), expected.end(), a) == expected.end()) {
            mari_ctx.fail(__FILE__, __LINE__,
                          std::string(what) + ": 표에 없는데 노출됐다 — " + a);
        }
    }
    if (expected.size() == actual.size()) {
        mari_ctx.fail(__FILE__, __LINE__, std::string(what) + ": 연산 순서가 표와 다르다");
    }
}

/// 현재 RSS(바이트). 못 읽으면 0.
usize rssBytes() {
    return static_cast<usize>(mari::test::rssBytes());
}

f64 nowMs() {
    return std::chrono::duration<f64, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Json req(const char* op) {
    Json j = Json::object();
    j.set("op", Json::string(op));
    j.set("view", Json::string("none")); // 측정에 PNG 인코딩을 섞지 않는다
    return j;
}

std::unique_ptr<AgentSession> openSession(mari::test::Context& mari_ctx) {
    auto s = AgentSession::open("mari-bench");
    CHECK(s.ok());
    return s.ok() ? std::move(s).value() : nullptr;
}

std::string mib(usize bytes) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return std::string(buf);
}

} // namespace

// ── 🔴 docs/05 6절: headless_parity ──────────────────────────────────────

MARI_TEST(headless_parity) {
    const std::vector<std::string> table = tableOpNames();
    CHECK(!table.empty());

    // ① 진짜 프로세스. 라이브러리를 직접 부르는 게 아니라 **빌드된 바이너리**를 띄운다.
    const auto dir = workDir("parity");
    const Run plain = run(dir, "--headless --capabilities");
    CHECK_EQ(plain.exitCode, 0);
    const Result<Json> parsed = Json::parse(plain.out);
    CHECK(parsed.ok());
    if (!parsed.ok()) {
        return;
    }
    const Json caps = parsed.value();
    CHECK(caps["headless"].asBool());
    expectSameOps(mari_ctx, table, capsOpNames(caps), "헤드리스 프로세스 --capabilities");

    // ② 화면이 **아예 없는** 환경. GUI 세션 변수를 지우고 같은 질문을 한다.
    //    하나라도 연산이 사라지면 그 연산은 화면에 의존하고 있었다는 뜻이다.
    const Run blind = run(dir, "--headless --capabilities",
                          "env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE ");
    CHECK_EQ(blind.exitCode, 0);
    const Result<Json> blindParsed = Json::parse(blind.out);
    CHECK(blindParsed.ok());
    if (blindParsed.ok()) {
        expectSameOps(mari_ctx, table, capsOpNames(blindParsed.value()),
                      "DISPLAY 없는 환경의 --capabilities");
    }

    // ③ 같은 바이너리의 **다른 표면**(MCP)도 같은 표에서 나온다.
    //    도구 이름을 연산 이름으로 되돌려서 비교한다 — 번역은 mcp 한 곳에서만 한다.
    const std::filesystem::path rpcPath = dir / "rpc.jsonl";
    writeText(rpcPath,
              "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
              "\"params\":{\"protocolVersion\":\"2025-06-18\"}}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
    const Run mcpRun = run(dir, "--headless --mcp --stdio --agent-id parity", std::string{},
                           rpcPath.string());
    CHECK_EQ(mcpRun.exitCode, 0);

    std::vector<std::string> mcpOps;
    {
        std::istringstream lines(mcpRun.out);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) {
                continue;
            }
            const Result<Json> msg = Json::parse(line);
            if (!msg.ok()) {
                mari_ctx.fail(__FILE__, __LINE__, "MCP 응답이 JSON 이 아니다: " + line);
                continue;
            }
            const Json& tools = msg.value()["result"]["tools"];
            for (usize i = 0; i < tools.size(); ++i) {
                mcpOps.push_back(mcp::opNameOf(tools.at(i)["name"].asString()));
            }
        }
    }
    // MCP 는 미지원 연산을 도구로 내보내지 않는다(되는 척하지 않는다) — 그 차이만큼만 빠진다.
    std::vector<std::string> exported;
    for (const agent::OpSpec& op : agent::opTable()) {
        if (mcp::isExported(op)) {
            exported.emplace_back(op.name);
        }
    }
    CHECK(!mcpOps.empty());
    expectSameOps(mari_ctx, exported, mcpOps, "헤드리스 MCP tools/list");

    // 빠진 것은 **미지원 연산뿐**이어야 한다. 지원하는데 MCP 에서만 사라지면 그게 갈라짐이다.
    for (const agent::OpSpec& op : agent::opTable()) {
        if (!op.supported) {
            continue;
        }
        if (std::find(mcpOps.begin(), mcpOps.end(), std::string(op.name)) == mcpOps.end()) {
            mari_ctx.fail(__FILE__, __LINE__,
                          std::string("지원 연산인데 MCP 표면에서 사라졌다: ") + op.name);
        }
    }

    // ④ GUI 전용 연산이 없다. 지원 연산은 전부 구현이 달려 있고, 헤드리스에서 부를 수 있다.
    //    "창이 있어야 한다" 같은 조건을 단 연산이 하나라도 있으면 여기서 걸린다.
    for (const agent::OpSpec& op : agent::opTable()) {
        if (op.supported) {
            CHECK(op.fn != nullptr);
            if (op.fn == nullptr) {
                mari_ctx.fail(__FILE__, __LINE__,
                              std::string("지원한다면서 구현이 없다: ") + op.name);
            }
        } else {
            // 미지원이면 **이유를 말한다.** 조용히 빠지지 않는다.
            CHECK(op.unsupportedReason[0] != '\0');
        }
        const std::string reason = op.unsupportedReason;
        for (const char* guiWord : {"GUI", "창", "window", "display", "X11"}) {
            if (reason.find(guiWord) != std::string::npos) {
                mari_ctx.fail(__FILE__, __LINE__,
                              std::string(op.name) + " 가 화면을 이유로 빠져 있다: " + reason);
            }
        }
    }

    // ⑤ 두 번째 연산 표가 없다. 어댑터가 자기 목록을 들고 있으면 언젠가 반드시 어긋난다.
    //    (docs/05 1절 "MCP 는 그 위에 씌우는 얇은 어댑터여야 한다")
    const char* adapters[] = {"cli/src/runner.cpp", "cli/src/server.cpp", "mcp/src/server.cpp",
                              "mcp/src/tools.cpp"};
    for (const char* rel : adapters) {
        const std::string text = readWholeFile(repoRoot() / rel);
        CHECK(!text.empty());
        // 연산 표를 새로 짜는 흔적: OpSpec 를 직접 만들거나 opTable 을 다시 정의하는 것.
        if (text.find("OpSpec{") != std::string::npos ||
            text.find("opTable()") != std::string::npos) {
            const bool onlyReads = text.find("const std::vector<OpSpec>& opTable") ==
                                   std::string::npos;
            if (!onlyReads) {
                mari_ctx.fail(__FILE__, __LINE__,
                              std::string(rel) + " 가 연산 표를 다시 정의한다");
            }
        }
        // 어댑터가 연산 이름을 **하드코딩한 목록**으로 들고 있지 않은지.
        // (`doc.create` 같은 이름이 어댑터 소스에 박혀 있으면 표가 늘 때 따라오지 않는다)
        // 점이 들어간 이름만 본다 — `snapshot` 은 연산 이름이면서 그룹 이름이기도 해서
        // 어댑터가 **그룹**으로 쓰는 것까지 잡으면 오탐이 된다(mcp 의 groupUsage 가 그렇다).
        for (const char* opName : {"\"doc.create\"", "\"layer.add\"", "\"snapshot.restore\"",
                                   "\"brush.import\""}) {
            if (text.find(opName) != std::string::npos) {
                mari_ctx.fail(__FILE__, __LINE__, std::string(rel) +
                                                      " 에 연산 이름이 하드코딩돼 있다: " + opName);
            }
        }
    }

    std::printf("      [headless_parity] 연산 %zu개 — 프로세스·DISPLAY 없는 환경·MCP 세 표면이"
                " 같은 표에서 나온다\n"
                "        MCP 로 나가는 도구 %zu개 (미지원 %zu개는 도구를 주지 않는다)\n"
                "        ⚠️ 이 리포에 GUI 는 없다. 'GUI 와 동일'은 GUI 를 띄워 비교한 것이"
                " 아니라\n"
                "           능력 계층이 하나뿐이라 갈라질 수 없음으로 증명한 것이다.\n",
                table.size(), exported.size(), table.size() - exported.size());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ── docs/02 8절 — 화면 없이 잴 수 있는 것을 **실제로 잰다** ──────────────

MARI_TEST(perf_cold_start) {
    // 목표: 콜드 스타트 < 1.5초 (docs/02 8절). Krita 는 보통 5~10초.
    // 헤드리스 바이너리가 떠서 자기 능력을 말하고 죽기까지를 잰다.
    const auto dir = workDir("cold");
    f64 best = 1e9;
    f64 worst = 0.0;
    constexpr int kRuns = 5;
    for (int i = 0; i < kRuns; ++i) {
        const f64 t0 = nowMs();
        const Run r = run(dir, "--headless --capabilities");
        const f64 dt = nowMs() - t0;
        CHECK_EQ(r.exitCode, 0);
        best = dt < best ? dt : best;
        worst = dt > worst ? dt : worst;
    }
    std::printf("      [perf] 콜드 스타트(--capabilities, %d회): 최소 %.1f ms / 최대 %.1f ms"
                " (목표 <1500ms)\n"
                "        ⚠️ 셸 경유(std::system) 시간이 포함돼 있다 — 실제 기동은 이보다 짧다.\n",
                kRuns, best, worst);
    CHECK(best < 1500.0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(perf_empty_canvas_memory) {
    // 목표: 빈 캔버스(1920×1080) 메모리 < 150MB (docs/02 8절).
    // 프로세스 전체 RSS 로 잰다 — "캔버스만"이 아니라 앱이 실제로 쓰는 양이 목표였다.
    const usize rss0 = rssBytes();
    CHECK(rss0 > 0);

    std::unique_ptr<AgentSession> s = openSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json create = req("doc.create");
    create.set("width", Json::integer(1920));
    create.set("height", Json::integer(1080));
    CHECK(s->execute(create)["ok"].asBool());

    const usize rss1 = rssBytes();
    const usize grew = rss1 > rss0 ? rss1 - rss0 : 0;
    std::printf("      [perf] 빈 캔버스 1920x1080: 세션+문서 RSS 증가 %s, 프로세스 전체 %s"
                " (목표 <150MB)\n"
                "        빈 캔버스는 타일을 잡지 않는다 — 칠한 만큼만 는다(COW 타일).\n",
                mib(grew).c_str(), mib(rss1).c_str());
    CHECK(rss1 < 150u * 1024u * 1024u);
}

MARI_TEST(perf_open_8k_canvas) {
    // 목표: 8K 캔버스 열기 < 3초 (docs/02 8절).
    // 빈 8K 를 여는 건 아무것도 증명하지 못한다 — 실제로 픽셀을 채워 .ora 로 저장한 뒤 연다.
    const auto dir = workDir("8k");
    std::unique_ptr<AgentSession> s = openSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json create = req("doc.create");
    create.set("width", Json::integer(8192));
    create.set("height", Json::integer(8192));
    CHECK(s->execute(create)["ok"].asBool());

    Json fill = req("fill");
    fill.set("region", Json::string("canvas"));
    fill.set("color", Json::string("#2E5A8C"));
    CHECK(s->execute(fill)["ok"].asBool());

    const std::filesystem::path path = dir / "8k.ora";
    Json save = req("doc.save");
    save.set("path", Json::string(path.string()));
    const f64 tSave0 = nowMs();
    const Json saved = s->execute(save);
    const f64 saveMs = nowMs() - tSave0;
    CHECK(saved["ok"].asBool());
    CHECK(s->execute(req("doc.close"))["ok"].asBool());

    std::error_code ec;
    const auto fileSize = static_cast<usize>(std::filesystem::file_size(path, ec));

    Json open = req("doc.open");
    open.set("path", Json::string(path.string()));
    const f64 t0 = nowMs();
    const Json opened = s->execute(open);
    const f64 openMs = nowMs() - t0;
    CHECK(opened["ok"].asBool());

    // 열린 게 **진짜 8K** 인지 본다. 조용히 작은 캔버스가 열렸으면 측정이 무의미하다.
    CHECK(s->document() != nullptr);
    if (s->document() != nullptr) {
        CHECK_EQ(s->document()->canvasSize().width, i32{8192});
        CHECK_EQ(s->document()->canvasSize().height, i32{8192});
    }
    const Json listed = s->execute(req("layer.list"));
    CHECK(listed["ok"].asBool());
    CHECK(listed["result"]["layers"].size() >= 1u);
    // 픽셀이 실제로 돌아왔다 — 타일이 하나도 없으면 빈 캔버스를 연 것이다.
    CHECK(listed["result"]["layers"].at(0)["tiles"].asInt() >= 16384);

    std::printf("      [perf] 8K(8192x8192) 전면 채색: 저장 %.0f ms, 파일 %s, **열기 %.0f ms**"
                " (목표 <3000ms)\n"
                "        ⚠️ 저장은 목표가 없던 항목인데 **%.1f초**다. 8192² 를 PNG 로 디플레이트"
                "하는 값이고,\n"
                "           지금은 재지 않고 적어만 둔다 — 목표가 없으니 통과/실패를 말할 수 없다.\n",
                saveMs, mib(fileSize).c_str(), openMs, saveMs / 1000.0);
    CHECK(openMs < 3000.0);

    CHECK(s->execute(req("doc.close"))["ok"].asBool());
    std::filesystem::remove_all(dir, ec);
}

MARI_TEST(perf_install_size) {
    // 목표: 설치 용량(호환 모듈 제외) < 80MB (docs/02 8절).
    // 여기서 잴 수 있는 것은 **이 빌드의 실행파일 하나**다. 정직하게 그것만 말한다.
    std::error_code ec;
    const std::filesystem::path bin = MARI_PAINT_BIN;
    const auto size = static_cast<usize>(std::filesystem::file_size(bin, ec));
    CHECK(!ec);
    std::printf("      [perf] mari-paint 실행파일: %s (목표 <80MB)\n"
                "        ⚠️ RelWithDebInfo 미스트립 바이너리다. 정적 라이브러리는 안에 들어가 "
                "있고,\n"
                "           zlib·sqlite3·libpng 는 시스템 공유 라이브러리라 여기 안 잡힌다.\n"
                "           GUI·COM·8bf 호스트는 이 빌드에 **없다** — 붙으면 늘어난다.\n",
                mib(size).c_str());
    CHECK(size < 80u * 1024u * 1024u);
}

MARI_TEST(perf_agent_stroke_throughput) {
    // docs/02 8절 "펜 입력 → 화면 표시 < 16ms" 중 **화면 표시는 잴 수 없다**(화면이 없다).
    // 잴 수 있는 것은 입력 → 픽셀까지다. 그 절반을 재서 그대로 적는다.
    std::unique_ptr<AgentSession> s = openSession(mari_ctx);
    CHECK(s != nullptr);
    if (s == nullptr) {
        return;
    }
    Json create = req("doc.create");
    create.set("width", Json::integer(2048));
    create.set("height", Json::integer(2048));
    CHECK(s->execute(create)["ok"].asBool());

    constexpr int kStrokes = 200;
    constexpr int kPoints = 64;
    f64 worst = 0.0;
    f64 total = 0.0;
    for (int i = 0; i < kStrokes; ++i) {
        Json stroke = req("stroke");
        Json points = Json::array();
        for (int p = 0; p < kPoints; ++p) {
            Json pt = Json::object();
            pt.set("x", Json::number(100.0 + static_cast<f64>(p) * 24.0));
            pt.set("y", Json::number(100.0 + static_cast<f64>(i) * 9.0 +
                                     static_cast<f64>(p % 7) * 3.0));
            pt.set("p", Json::number(0.2 + 0.7 * static_cast<f64>(p % 10) / 9.0));
            points.push(std::move(pt));
        }
        stroke.set("points", std::move(points));
        stroke.set("size", Json::number(18.0));
        stroke.set("color", Json::string("#101820"));
        const f64 t0 = nowMs();
        const Json r = s->execute(stroke);
        const f64 dt = nowMs() - t0;
        CHECK(r["ok"].asBool());
        total += dt;
        worst = dt > worst ? dt : worst;
    }

    const f64 avg = total / static_cast<f64>(kStrokes);
    const f64 perPoint = total / static_cast<f64>(kStrokes * kPoints);
    std::printf("      [perf] agent-api stroke: 획 %d개 × 점 %d개, 총 %.1f ms\n"
                "        획 1개 평균 %.3f ms / 최악 %.3f ms, 점 1개당 %.1f us\n"
                "        ⚠️ docs/02 8절의 '펜 입력 → 화면 표시 <16ms' 는 **미측정**이다 —\n"
                "           화면이 없으므로 표시 쪽 절반을 잴 방법이 없다. 위 숫자는 입력→픽셀뿐.\n",
                kStrokes, kPoints, total, avg, worst, perPoint * 1000.0);

    // 🔴 16ms 와 맞대 볼 단위는 **점 하나**다 — 펜 이벤트 하나가 그것이다.
    //    획 하나(점 64개)를 16ms 와 비교하면 단위가 달라 아무 뜻이 없다.
    //    입력→픽셀만으로 한 프레임을 다 써 버리면 표시 쪽을 붙일 여지가 없다.
    CHECK(perPoint < 16.0);
    // 획 단위는 회귀 감시용으로만 느슨하게 건다(기계 부하에 흔들린다).
    CHECK(worst < 200.0);

    // 이 세션이 만든 획은 전부 Agent 칸에 있다(다른 칸이 늘면 경계선이 새는 것이다).
    CHECK_EQ(s->originStats().agent(), static_cast<u64>(kStrokes));
    CHECK_EQ(s->originStats().human(), u64{0});
    CHECK_EQ(s->originStats().unspecified(), u64{0});
}

MARI_TEST_MAIN()
