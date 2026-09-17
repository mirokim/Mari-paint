// Mari Paint — CLI 본체. agent-api 위에 얹는 얇은 어댑터다.
#include <mari/cli/runner.hpp>

#include <mari/agent/view.hpp>
#include <mari/cli/server.hpp>
#include <mari/mcp/server.hpp>
#include <mari/record/sigan_recorder.hpp>
#include <mari/ora/ora.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>

namespace mari::cli {
namespace {

bool wantsValue(std::string_view opt) {
    return opt == "--script" || opt == "--exec" || opt == "--serve" || opt == "--out-dir" ||
           opt == "--agent-id" || opt == "--view" || opt == "--proof-out" ||
           opt == "--journal-dir";
}

/// 파일 이름으로 쓸 수 없는 문자를 걷어낸다(연산 이름이 경로를 벗어나지 않게).
std::string sanitize(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(okChar ? c : '-');
    }
    if (out.empty()) {
        out = "op";
    }
    return out;
}

/// 이 세션이 만든 획의 출처 집계.
/// 🔴 숫자만 낸다. "human-only"/"ai-assisted" 같은 등급 문자열은 만들지 않는다 —
///    판정은 Sigan 의 몫이다(docs/03 2절 · docs/05 3.2).
/// 🔴 **세 축을 따로** 낸다(docs/06 결정 ②). 붓질 수 하나만 내면 캔버스 전체를 칠한
///    fill 한 번이 "AI 획 0개"로 보인다 — 참인 숫자 하나로 만든 거짓이다(6절 H3).
Json originsJson(const agent::AgentSession& session,
                 const record::SiganRecorderFactory* recorders) {
    const StrokeOriginStats& s = session.originStats();
    Json counts = Json::object();
    counts.set("humanPen", Json::integer(s.count(StrokeOrigin::HumanPen)));
    counts.set("humanMouse", Json::integer(s.count(StrokeOrigin::HumanMouse)));
    counts.set("agent", Json::integer(s.count(StrokeOrigin::Agent)));
    counts.set("imported", Json::integer(s.count(StrokeOrigin::Imported)));
    counts.set("filter", Json::integer(s.count(StrokeOrigin::Filter)));
    counts.set("unspecified", Json::integer(s.unspecified()));

    Json out = Json::object();
    out.set("agentId", Json::string(std::string(session.agentId().view())));
    out.set("originCounts", std::move(counts));
    // 축 A — 붓질.
    out.set("humanStrokes", Json::integer(s.human()));
    out.set("agentStrokes", Json::integer(s.agent()));
    out.set("totalStrokes", Json::integer(s.total()));
    out.set("agentStrokeRatio", Json::number(s.agentRatio()));
    // 축 B — 영역 연산(fill·erase·gradient·transform). 붓질과 같은 칸이 아니다.
    Json regions = Json::object();
    regions.set("human", Json::integer(session.regionOpStats().human()));
    regions.set("agent", Json::integer(session.regionOpStats().agent()));
    regions.set("total", Json::integer(session.regionOpStats().total()));
    out.set("regionOps", std::move(regions));
    // 축 C — 변경된 타일 수(픽셀이 아니다).
    out.set("changedTiles", Json::integer(session.changedTiles()));

    // 기록이 켜져 있으면 **문서 구간**의 집계도 함께 낸다 — 이쪽은 사람 획까지 센다
    // (같은 문서를 사람이 이어 그리면 여기 잡힌다. 세션 숫자는 세션 것만 센다).
    out.set("recorded", Json::boolean(recorders != nullptr));
    if (recorders != nullptr) {
        const agent::RecordingTally t = recorders->totalTally();
        Json seg = Json::object();
        seg.set("humanBrushStrokes", Json::integer(t.strokes.human()));
        seg.set("agentBrushStrokes", Json::integer(t.strokes.agent()));
        seg.set("humanRegionOps", Json::integer(t.regionOps.human()));
        seg.set("agentRegionOps", Json::integer(t.regionOps.agent()));
        seg.set("humanChangedTiles", Json::integer(t.humanTiles()));
        seg.set("agentChangedTiles", Json::integer(t.agentTiles()));
        seg.set("journals", Json::integer(static_cast<i64>(recorders->journals().size())));
        out.set("documentSegments", std::move(seg));
    }
    return out;
}

} // namespace

std::string usageText() {
    return
        "mari-paint — 헤드리스 페인트 엔진 (GUI 없음. UI 는 이 계층의 얇은 껍데기다)\n"
        "\n"
        "사용법:\n"
        "  mari-paint --headless --script <paint.json>   스크립트를 순차 실행\n"
        "  mari-paint --headless --exec '<json>'         연산 하나(또는 배열)를 실행\n"
        "  mari-paint --headless --serve :7777           줄 단위 JSON-RPC 서버\n"
        "  mari-paint --headless --mcp --stdio           MCP 서버(도구 목록은 자동 생성)\n"
        "  mari-paint --version --capabilities           버전 · 능력 목록\n"
        "\n"
        "옵션:\n"
        "  --mcp --stdio         MCP 서버로 기동한다. 한 줄 = JSON-RPC 2.0 메시지 하나.\n"
        "                        🔴 도구 목록은 agent-api 연산 표에서 **자동 생성**된다.\n"
        "                           결과 이미지는 MCP 이미지 콘텐츠로 실린다(docs/05 2.1).\n"
        "                           이 모드에서는 stdout 에 JSON 메시지 말고 아무 것도 안 나간다.\n"
        "  --out-dir <dir>       응답에 실린 PNG 를 이 디렉터리에 떨군다(stdout 에서는 뺀다)\n"
        "  --view <mode>         기본 시각 피드백: none | dirty | full | layer:<id> |\n"
        "                        region:[x,y,w,h]. 연산이 직접 view 를 주면 그쪽이 이긴다.\n"
        "                        기본값은 none 이다 — stdout 이 파이프라서 base64 를\n"
        "                        말없이 흘리지 않는다. --out-dir/--embed-images 를 주면 dirty.\n"
        "  --journal-dir <dir>   기록 저널을 이 디렉터리에 남긴다(문서 하나 = 저널 하나).\n"
        "                        🔴 주지 않으면 기록기가 꽂히지 않는다 — 그리기는 되지만\n"
        "                           기록이 남지 않는다. 응답의 recorded 가 그 사실을 말한다.\n"
        "  --proof-out <path>    이 세션의 기록을 무서명 과정 로그(JSON)로 떨군다.\n"
        "                        🔴 **인증서가 아니다.** 서명도 해시체인도 없고 등급은\n"
        "                           \"unsigned\" 하나뿐이다 — 봉인은 Sigan 의 몫이다(docs/03 2절).\n"
        "                           저널이 여럿이면 <path>.1, <path>.2 … 로 이어 붙는다.\n"
        "  --agent-id <id>       붓을 쥐는 에이전트 이름(기본 mari-cli).\n"
        "                        🔴 이 경로로 들어온 획은 무조건 origin=agent 다.\n"
        "                           origin 을 고르는 옵션은 없다(docs/05 3.1).\n"
        "  --continue-on-error   연산이 실패해도 다음 연산을 계속한다(기본은 중단)\n"
        "  --embed-images        base64 PNG 를 stdout JSON 에 남긴다\n"
        "  --pretty              stdout JSON 을 들여쓴다\n"
        "  --quiet               stderr 진행 출력을 끈다\n"
        "\n"
        "스크립트 형식:\n"
        "  연산 배열 그대로이거나\n"
        "  { \"agentId\": \"...\", \"onError\": \"stop\"|\"continue\", \"ops\": [ ... ] }\n"
        "  연산 이름은 capabilities 가 알려준다(doc.create · stroke · layer.add · ...).\n"
        "\n"
        "출력:\n"
        "  stdout = JSON 값 하나뿐이다(파이프 가능). 진행 상황·경고는 stderr 로 간다.\n"
        "  종료 코드: 0 성공 · 1 실행 실패 · 2 사용법 오류\n";
}

Result<CliOptions> parseArgs(const std::vector<std::string>& args) {
    CliOptions o;
    bool viewGiven = false;
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        std::string value;
        std::string opt = a;
        // `--script=x` 형태도 받는다.
        const usize eq = a.find('=');
        if (a.rfind("--", 0) == 0 && eq != std::string::npos) {
            opt = a.substr(0, eq);
            value = a.substr(eq + 1);
        } else if (wantsValue(a)) {
            if (i + 1 >= args.size()) {
                return Err(a + " 에 값이 없다", ErrorCode::InvalidArgument);
            }
            value = args[++i];
        }

        if (opt == "--headless") {
            o.headless = true;
        } else if (opt == "--version" || opt == "-v") {
            o.showVersion = true;
        } else if (opt == "--capabilities") {
            o.showCapabilities = true;
        } else if (opt == "--help" || opt == "-h") {
            o.help = true;
        } else if (opt == "--script") {
            o.scriptPath = value;
        } else if (opt == "--exec") {
            o.execJson = value;
        } else if (opt == "--serve") {
            o.serveAddr = value;
        } else if (opt == "--mcp") {
            o.mcp = true;
        } else if (opt == "--stdio") {
            o.stdio = true;
        } else if (opt == "--out-dir") {
            o.outDir = value;
        } else if (opt == "--proof-out") {
            o.proofOut = value;
        } else if (opt == "--journal-dir") {
            o.journalDir = value;
        } else if (opt == "--agent-id") {
            o.agentId = value;
        } else if (opt == "--view") {
            o.view = value;
            viewGiven = true;
        } else if (opt == "--continue-on-error") {
            o.continueOnError = true;
        } else if (opt == "--embed-images") {
            o.embedImages = true;
        } else if (opt == "--pretty") {
            o.pretty = true;
        } else if (opt == "--quiet") {
            o.quiet = true;
        } else {
            // 🔴 모르는 옵션을 넘기지 않는다. `--srcipt` 오타가 "아무 것도 안 함"이 되면
            //    스크립트는 성공한 줄 알고 넘어간다.
            return Err("모르는 옵션이다: " + a, ErrorCode::InvalidArgument);
        }
    }
    if (o.agentId.empty()) {
        return Err("--agent-id 가 비었다 — 익명 에이전트 획은 만들지 않는다",
                   ErrorCode::InvalidArgument);
    }
    if (!viewGiven) {
        // 이미지를 받을 곳이 있으면 바뀐 영역을 기본으로 보여 준다. 없으면 만들지 않는다.
        o.view = (!o.outDir.empty() || o.embedImages) ? "dirty" : "none";
    }
    if (o.stdio && !o.mcp) {
        // `--stdio` 는 전송 선택이지 그 자체로 할 일이 아니다. 혼자 오면 오타일 가능성이 크다.
        return Err("--stdio 는 --mcp 와 함께 쓴다", ErrorCode::InvalidArgument);
    }
    if (o.mcp) {
        // 지금 MCP 전송은 stdio 하나뿐이다. 그래서 --mcp 만 줘도 stdio 로 뜬다 —
        // 없는 전송을 고르게 해 놓고 나중에 거절하는 것보다 낫다.
        o.stdio = true;
    }
    int modes = 0;
    modes += o.scriptPath.empty() ? 0 : 1;
    modes += o.execJson.empty() ? 0 : 1;
    modes += o.serveAddr.empty() ? 0 : 1;
    modes += o.mcp ? 1 : 0;
    if (modes > 1) {
        return Err("--script · --exec · --serve · --mcp 는 한 번에 하나만 쓴다",
                   ErrorCode::InvalidArgument);
    }
    return Ok(std::move(o));
}

Result<Script> parseScript(const Json& root) {
    Script s;
    if (root.isArray()) {
        for (usize i = 0; i < root.size(); ++i) {
            s.ops.push_back(root.at(i));
        }
        return Ok(std::move(s));
    }
    if (!root.isObject()) {
        return Err("스크립트는 연산 배열이거나 {\"ops\":[...]} 객체여야 한다",
                   ErrorCode::ParseError);
    }
    const Json& ops = root["ops"];
    if (!ops.isArray()) {
        return Err("스크립트 객체에 \"ops\" 배열이 없다", ErrorCode::ParseError);
    }
    for (usize i = 0; i < ops.size(); ++i) {
        s.ops.push_back(ops.at(i));
    }
    s.agentId = root["agentId"].asString();
    if (root.has("onError")) {
        const std::string mode = root["onError"].asString();
        if (mode != "stop" && mode != "continue") {
            return Err("onError 는 \"stop\" 또는 \"continue\" 여야 한다",
                       ErrorCode::InvalidArgument);
        }
        s.continueOnError = (mode == "continue");
        s.hasOnError = true;
    }
    return Ok(std::move(s));
}

Result<void> spillImage(Json& envelope, const std::string& outDir, usize seq, bool keepBase64) {
    if (outDir.empty()) {
        return Ok();
    }
    const Json& img = envelope["image"];
    if (!img.isObject() || !img.has("png")) {
        return Ok();
    }
    Result<std::vector<u8>> bytes = agent::base64Decode(img["png"].asString());
    if (!bytes.ok()) {
        return bytes.error();
    }
    const std::string name = std::to_string(seq) + "-" + sanitize(envelope["op"].asString()) + ".png";
    const std::string path = (std::filesystem::path(outDir) / name).string();
    const Result<void> w = ora::writeFileBytes(path, bytes.value().data(), bytes.value().size());
    if (!w.ok()) {
        return w;
    }

    // 같은 키 순서를 유지한 채 `png` 만 빼고 `path` 를 붙인다.
    // (Json 에는 키 삭제가 없다. 없어도 되는 API 라서 만들지 않고 여기서 다시 쌓는다.)
    Json rebuilt = Json::object();
    for (const std::string& key : img.keys()) {
        if (key == "png" && !keepBase64) {
            continue;
        }
        rebuilt.set(key, img[key]);
    }
    rebuilt.set("path", Json::string(path));
    rebuilt.set("bytes", Json::integer(static_cast<i64>(bytes.value().size())));
    envelope.set("image", std::move(rebuilt));
    return Ok();
}

Json runOps(agent::AgentSession& session, const std::vector<Json>& ops, const CliOptions& opt,
            std::ostream& err, usize& failed, const record::SiganRecorderFactory* recorders) {
    Json results = Json::array();
    failed = 0;
    bool stopped = false;
    usize executed = 0;
    usize images = 0;

    for (usize i = 0; i < ops.size(); ++i) {
        Json req = ops[i];
        // 🔴 연산이 직접 정한 view 가 언제나 이긴다. CLI 는 **비어 있을 때만** 채운다.
        if (req.isObject() && !req.has("view") && !opt.view.empty()) {
            req.set("view", Json::string(opt.view));
        }
        const std::string opName = req.isObject() && req["op"].isString() ? req["op"].asString()
                                                                         : std::string("<이름 없음>");

        const auto t0 = std::chrono::steady_clock::now();
        Json env = session.execute(req);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        ++executed;

        const bool ok = env["ok"].asBool(false);
        const Result<void> spilled = spillImage(env, opt.outDir, images, opt.embedImages);
        if (env.has("image")) {
            ++images;
        }
        if (!spilled.ok()) {
            // 이미지를 못 떨궜다고 연산 결과를 뒤집지 않는다. 그 사실만 적는다.
            env.set("imageError", Json::string(spilled.message()));
        }
        env.set("index", Json::integer(static_cast<i64>(i)));
        env.set("ms", Json::integer(static_cast<i64>(ms)));

        if (ok) {
            if (!opt.quiet) {
                // 🔴 진행 상황은 stderr 로만 간다. stdout 은 순수 JSON 이어야 파이프가 된다.
                err << "[" << (i + 1) << "/" << ops.size() << "] " << opName << " ok (" << ms
                    << "ms)\n";
            }
        } else {
            ++failed;
            if (!opt.quiet) {
                err << "[" << (i + 1) << "/" << ops.size() << "] " << opName
                    << " 실패: " << env["error"]["message"].asString() << "\n";
            }
        }
        results.push(std::move(env));

        if (!ok && !opt.continueOnError) {
            stopped = true;
            if (!opt.quiet) {
                err << "중단한다 (--continue-on-error 로 계속할 수 있다)\n";
            }
            break;
        }
    }

    Json out = Json::object();
    out.set("ok", Json::boolean(failed == 0));
    out.set("opCount", Json::integer(static_cast<i64>(ops.size())));
    out.set("executed", Json::integer(static_cast<i64>(executed)));
    out.set("failed", Json::integer(static_cast<i64>(failed)));
    out.set("stopped", Json::boolean(stopped));
    out.set("results", std::move(results));
    // 🔴 출처 집계를 **항상** 붙인다. 물어봐야 나오면 안 된다(docs/05 3.2).
    out.set("strokeOrigins", originsJson(session, recorders));
    return out;
}

int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    return runCli(args, out, err, std::cin);
}

int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
           std::istream& in) {
    const auto usage = [&](const std::string& why) {
        err << "오류: " << why << "\n\n" << usageText();
        return kExitUsage;
    };

    Result<CliOptions> parsed = parseArgs(args);
    if (!parsed.ok()) {
        return usage(parsed.message());
    }
    CliOptions opt = std::move(parsed).value();

    if (opt.help) {
        err << usageText();
        return kExitOk;
    }

    const bool hasMode = !opt.scriptPath.empty() || !opt.execJson.empty() ||
                         !opt.serveAddr.empty() || opt.mcp || opt.showVersion ||
                         opt.showCapabilities;
    if (!hasMode) {
        return usage("할 일이 없다 — --script · --exec · --serve · --mcp · --version · "
                     "--capabilities 중 하나가 필요하다");
    }

    // 스크립트가 agentId 를 직접 정할 수 있으므로 세션보다 먼저 읽는다.
    std::optional<Script> script;
    std::string agentId = opt.agentId;
    if (!opt.scriptPath.empty()) {
        Result<std::vector<u8>> bytes = ora::readFileBytes(opt.scriptPath);
        if (!bytes.ok()) {
            err << "오류: 스크립트를 읽을 수 없다: " << opt.scriptPath << " (" << bytes.message()
                << ")\n";
            return kExitFailed;
        }
        const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                               bytes.value().size());
        Result<Json> root = Json::parse(text);
        if (!root.ok()) {
            err << "오류: 스크립트 JSON 이 깨졌다: " << opt.scriptPath << " — " << root.message()
                << "\n";
            return kExitFailed;
        }
        Result<Script> sc = parseScript(root.value());
        if (!sc.ok()) {
            err << "오류: 스크립트 형식이 틀렸다: " << sc.message() << "\n";
            return kExitFailed;
        }
        script = std::move(sc).value();
        if (!script->agentId.empty()) {
            agentId = script->agentId;
        }
        if (script->hasOnError) {
            opt.continueOnError = script->continueOnError;
        }
    }

    if (!opt.outDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt.outDir, ec);
        if (ec) {
            err << "오류: --out-dir 를 만들 수 없다: " << opt.outDir << " (" << ec.message()
                << ")\n";
            return kExitFailed;
        }
    }

    // 🔴 기록 배선(docs/06). **공장을 세션보다 먼저 만든다** — 세션 안의 Application 이
    //    이 공장을 들고 있으므로 공장이 더 오래 살아야 한다.
    //    꽂지 않으면 널 레코더다: agent-api 는 그대로 돌고, 기록만 남지 않는다.
    //    Sigan 미설치가 정상 상태라는 규정(docs/03 5.1)이 여기서도 그대로다.
    std::unique_ptr<record::SiganRecorderFactory> recorders;
    if (!opt.journalDir.empty() || !opt.proofOut.empty()) {
        record::RecordingConfig rc;
        rc.journalDir = opt.journalDir;
        // 프레임에는 32비트 다이제스트만 실린다. 사람이 읽을 이름은 로그가 들고 있는다.
        Result<AgentId> id = AgentId::make(agentId);
        if (id.ok()) {
            rc.agents.push_back(id.value());
        }
        // 싱크는 붙이지 않는다 — Sigan 파이프 연결은 이 CLI 의 일이 아니다.
        // 정본은 저널이고, 저널만으로 기록은 완전하다(docs/03 4.2 · 6절).
        recorders = std::make_unique<record::SiganRecorderFactory>(std::move(rc));
    }

    // 🔴 세션이 열리는 순간 출처가 정해진다. 이름이 없으면 열리지 않는다.
    Result<std::unique_ptr<agent::AgentSession>> opened = agent::AgentSession::open(agentId);
    if (!opened.ok()) {
        err << "오류: 세션을 열 수 없다: " << opened.message() << "\n";
        return kExitFailed;
    }
    agent::AgentSession& session = *opened.value();
    session.application().setRecorderFactory(recorders.get());

    // ── --mcp --stdio (docs/05 2.8 · 4절) ────────────────────────────────
    // 🔴 여기서 하는 일은 스트림을 물려 주는 것뿐이다. 도구 목록도, 이미지 포장도
    //    전부 mari::mcp 가 하고, 연산은 그 아래 같은 AgentSession 이 한다.
    if (opt.mcp) {
        if (!opt.quiet) {
            // 🔴 stdout 은 MCP 메시지 전용이다. 배너 한 줄이 섞이면 클라이언트가 끊는다.
            err << "mari-paint MCP 서버 (stdio) — agentId=" << agentId
                << " (이 경로의 획은 전부 origin=agent 다)\n";
        }
        return mcp::serveStdio(session, in, out, err);
    }

    // ── --version / --capabilities ───────────────────────────────────────
    if (opt.scriptPath.empty() && opt.execJson.empty() && opt.serveAddr.empty()) {
        Json root = Json::object();
        if (opt.showVersion) {
            root.set("name", Json::string("mari-paint"));
            root.set("version", Json::string(session.application().version()));
            root.set("schema", Json::string(agent::kApiSchema));
            root.set("headless", Json::boolean(true));
        }
        if (opt.showCapabilities) {
            Json req = Json::object();
            req.set("op", Json::string("capabilities"));
            req.set("view", Json::string("none"));
            Json env = session.execute(req);
            if (!env["ok"].asBool(false)) {
                err << "오류: capabilities 를 만들 수 없다: "
                    << env["error"]["message"].asString() << "\n";
                return kExitFailed;
            }
            if (opt.showVersion) {
                root.set("capabilities", env["result"]);
            } else {
                root = env["result"];
            }
        }
        out << root.dump(opt.pretty ? 2 : 0) << "\n";
        return kExitOk;
    }

    // ── --serve ──────────────────────────────────────────────────────────
    if (!opt.serveAddr.empty()) {
        Result<ServeAddress> addr = parseServeAddress(opt.serveAddr);
        if (!addr.ok()) {
            return usage(addr.message());
        }
        const Result<void> r = serveTcp(session, addr.value(), err, {});
        Json root = Json::object();
        root.set("ok", Json::boolean(r.ok()));
        root.set("mode", Json::string("serve"));
        root.set("host", Json::string(addr.value().host));
        root.set("port", Json::integer(static_cast<i64>(addr.value().port)));
        root.set("requests", Json::integer(session.requestCount()));
        if (!r.ok()) {
            root.set("error", Json::string(r.message()));
        }
        root.set("strokeOrigins", originsJson(session, recorders.get()));
        out << root.dump(opt.pretty ? 2 : 0) << "\n";
        return r.ok() ? kExitOk : kExitFailed;
    }

    // ── --exec / --script ────────────────────────────────────────────────
    std::vector<Json> ops;
    if (script) {
        ops = script->ops;
    } else {
        Result<Json> one = Json::parse(opt.execJson);
        if (!one.ok()) {
            err << "오류: --exec JSON 이 깨졌다: " << one.message() << "\n";
            return kExitFailed;
        }
        if (one.value().isArray()) {
            for (usize i = 0; i < one.value().size(); ++i) {
                ops.push_back(one.value().at(i));
            }
        } else {
            ops.push_back(std::move(one).value());
        }
    }

    if (!opt.quiet) {
        err << "mari-paint 헤드리스 — 연산 " << ops.size() << "개, agentId=" << agentId
            << " (이 경로의 획은 전부 origin=agent 다)\n";
    }

    usize failed = 0;
    Json root = runOps(session, ops, opt, err, failed, recorders.get());

    // ── --proof-out (docs/03 6절 · docs/06) ──────────────────────────────
    // 🔴 만드는 것은 **무서명 로그**다. 인증서가 아니다.
    if (!opt.proofOut.empty() && recorders) {
        Json written = Json::array();
        const std::vector<std::string>& journals = recorders->journals();
        for (usize i = 0; i < journals.size(); ++i) {
            // 살아 있는 구간이면 축 C(변경 타일 수)까지 아는 쪽에서 뽑는다.
            Result<std::string> log = Err("", ErrorCode::Unknown);
            record::SiganRecorder* live = nullptr;
            for (usize k = 0; k < recorders->liveCount(); ++k) {
                record::SiganRecorder* r = recorders->liveAt(k);
                if (r != nullptr && r->journalPath() == journals[i]) {
                    live = r;
                    break;
                }
            }
            if (live != nullptr) {
                log = live->buildProofLog();
            } else {
                record::RecordingConfig rc;
                Result<AgentId> id = AgentId::make(agentId);
                if (id.ok()) {
                    rc.agents.push_back(id.value());
                }
                log = record::proofLogFromJournal(journals[i], rc);
            }
            const std::string path =
                i == 0 ? opt.proofOut : opt.proofOut + "." + std::to_string(i);
            Json entry = Json::object();
            entry.set("journal", Json::string(journals[i]));
            entry.set("path", Json::string(path));
            if (!log.ok()) {
                // 기록을 못 뽑았다고 그리기 결과를 뒤집지 않는다. 그 사실만 적는다.
                entry.set("error", Json::string(log.message()));
            } else {
                const Result<void> w = ora::writeFileBytes(
                    path, reinterpret_cast<const u8*>(log.value().data()), log.value().size());
                if (!w.ok()) {
                    entry.set("error", Json::string(w.message()));
                } else {
                    entry.set("bytes", Json::integer(static_cast<i64>(log.value().size())));
                }
            }
            written.push(std::move(entry));
        }
        // 🔴 이름부터 "인증서 아님"을 말한다. 등급은 로그 안에 "unsigned" 하나뿐이다.
        root.set("proofLogs", std::move(written));
        root.set("proofLogNotice",
                 Json::string("무서명 과정 로그다. 인증서가 아니다 — 봉인·서명은 Sigan 이 한다"));
    } else if (!opt.proofOut.empty()) {
        root.set("proofLogNotice", Json::string("기록기가 꽂히지 않아 떨굴 로그가 없다"));
    }

    out << root.dump(opt.pretty ? 2 : 0) << "\n";
    if (!opt.quiet) {
        err << "끝: 연산 " << ops.size() << "개 중 " << failed << "개 실패\n";
    }
    return failed == 0 ? kExitOk : kExitFailed;
}

} // namespace mari::cli
