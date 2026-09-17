// MCP 서버 어댑터 (docs/05 4절 · A3)
//
// 🔴 여기서 증명하려는 것 넷:
//    ① `mcp_tools_generated` — 도구 목록이 agent-api 연산 표와 **자동으로** 일치한다.
//       손으로 유지하는 두 번째 목록이 없다(docs/05 4절 · 6절 검증표).
//    ② initialize / tools/list / tools/call 왕복이 **실제 JSON-RPC 바이트**로 돈다.
//    ③ 🔴 tools/call 결과에 **이미지 콘텐츠**가 실린다 —
//       AI 가 자기가 그린 걸 봐야 한다(docs/05 2.1). 이게 제일 중요하다.
//    ④ 잘못된 요청에 규약대로 오류 응답이 나온다. **크래시가 아니다.**
#include <mari/agent/capabilities.hpp>
#include <mari/agent/json.hpp>
#include <mari/agent/session.hpp>
#include <mari/agent/view.hpp>
#include <mari/cli/runner.hpp>
#include <mari/mcp/server.hpp>
#include <mari/mcp/tools.hpp>
#include <mari/test/harness.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mari;
using mari::agent::Json;

namespace {

/// 세션 하나. 이름이 없으면 열리지 않으므로(익명 AI 획 금지) 여기서도 이름을 댄다.
std::unique_ptr<agent::AgentSession> openSession(mari::test::Context& ctx) {
    Result<std::unique_ptr<agent::AgentSession>> s = agent::AgentSession::open("mcp-test");
    if (!s.ok()) {
        ctx.fail(__FILE__, __LINE__, "세션을 열 수 없다: " + s.message());
        return nullptr;
    }
    return std::move(s).value();
}

Json parseOrFail(const std::string& text, mari::test::Context& ctx, int line) {
    Result<Json> r = Json::parse(text);
    if (!r.ok()) {
        ctx.fail(__FILE__, line, "응답이 JSON 이 아니다: " + text);
        return Json::null();
    }
    return std::move(r).value();
}

/// JSON-RPC 요청 한 줄을 만든다. **문자열로 만든다** — 바이트 수준에서 검증하려는 것이라
/// Json 객체를 그대로 넘기면 프레이밍·파싱을 건너뛰게 된다.
std::string rpc(const std::string& id, const std::string& method, const std::string& params = "") {
    std::string s = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"method\":\"" + method + "\"";
    if (!params.empty()) {
        s += ",\"params\":" + params;
    }
    return s + "}";
}

/// 배열에서 `type` 이 주어진 값인 첫 콘텐츠.
const Json& contentOfType(const Json& content, const char* type) {
    static const Json kNull = Json::null();
    for (usize i = 0; i < content.size(); ++i) {
        if (content.at(i)["type"].asString() == type) {
            return content.at(i);
        }
    }
    return kNull;
}

/// 문자열에서 **전부 소문자인** ASCII 낱말만 뽑는다(설명문 검사용).
/// 대문자가 섞인 낱말(고유명사 "Mari Paint")은 도구 이름 후보가 아니므로 버린다.
/// 낱말 경계를 대문자까지 포함해 끊어야 "Mari" 가 "ari" 로 새지 않는다.
std::vector<std::string> lowercaseWords(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool hasUpper = false;
    const auto flush = [&] {
        if (cur.size() >= 3 && !hasUpper) {
            out.push_back(cur);
        }
        cur.clear();
        hasUpper = false;
    };
    for (const char c : text) {
        const bool upper = (c >= 'A' && c <= 'Z');
        const bool wordChar =
            upper || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (wordChar) {
            cur.push_back(c);
            hasUpper = hasUpper || upper;
        } else {
            flush();
        }
    }
    flush();
    return out;
}

} // namespace

// ── ① 도구 목록은 생성된다 ───────────────────────────────────────────────

MARI_TEST(mcp_tools_generated) {
    const Json tools = mcp::toolsArray();

    // 기대 목록을 **여기서 손으로 적지 않는다.** 연산 표에서 그때그때 만든다 —
    // 그래야 "표가 늘면 도구도 는다"를 실제로 검사하게 된다.
    std::vector<std::string> expected;
    usize unsupported = 0;
    for (const agent::OpSpec& op : agent::opTable()) {
        if (mcp::isExported(op)) {
            expected.push_back(mcp::toolNameOf(op.name));
        } else {
            ++unsupported;
        }
    }
    CHECK(!expected.empty());
    CHECK_EQ(tools.size(), expected.size());

    for (usize i = 0; i < expected.size() && i < tools.size(); ++i) {
        CHECK_EQ(tools.at(i)["name"].asString(), expected[i]);
    }

    // 미지원 연산은 도구로 나가지 않는다 — "되는 척하지 않는다"(docs/04 6절).
    // 대신 capabilities 도구가 그 목록과 이유를 전부 돌려주므로 정보는 남는다.
    CHECK(unsupported > 0); // 지금 표에 미지원 연산이 실제로 있다(select.invert 등)
    for (const agent::OpSpec& op : agent::opTable()) {
        if (op.supported) {
            continue;
        }
        CHECK(mcp::findTool(mcp::toolNameOf(op.name)) == nullptr);
    }
    CHECK(mcp::findTool("capabilities") != nullptr);

    // 이름 규약: MCP 클라이언트가 흔히 거는 `[A-Za-z0-9_-]{1,64}` 를 지킨다.
    for (usize i = 0; i < tools.size(); ++i) {
        const std::string name = tools.at(i)["name"].asString();
        CHECK(!name.empty());
        CHECK(name.size() <= 64);
        for (const char c : name) {
            const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!okChar) {
                mari_ctx.fail(__FILE__, __LINE__, "도구 이름에 쓸 수 없는 문자: " + name);
            }
        }
        // 이름이 겹치면 하나가 조용히 가려진다.
        for (usize j = i + 1; j < tools.size(); ++j) {
            CHECK_NE(name, tools.at(j)["name"].asString());
        }
        // 왕복: 도구 이름 → 연산 이름 → 도구 이름.
        const std::string opName = mcp::opNameOf(name);
        CHECK(!opName.empty());
        CHECK_EQ(mcp::toolNameOf(opName), name);
    }
}

MARI_TEST(mcp_tool_schema_comes_from_the_op_table) {
    // 🔴 하드코딩된 목록으로는 통과할 수 없는 검사다 —
    //    설명·파라미터·필수 여부를 **런타임 표의 내용과 글자 단위로** 맞춰 본다.
    const Json tools = mcp::toolsArray();
    usize checked = 0;

    for (const agent::OpSpec& op : agent::opTable()) {
        if (!mcp::isExported(op)) {
            continue;
        }
        const Json* tool = nullptr;
        for (usize i = 0; i < tools.size(); ++i) {
            if (tools.at(i)["name"].asString() == mcp::toolNameOf(op.name)) {
                tool = &tools.at(i);
                break;
            }
        }
        if (tool == nullptr) {
            mari_ctx.fail(__FILE__, __LINE__, std::string("도구가 없다: ") + op.name);
            continue;
        }
        ++checked;

        const std::string desc = (*tool)["description"].asString();
        CHECK(desc.find(op.summary) != std::string::npos);
        // 각 도구에 **언제 쓰는지** 한 줄이 있다.
        const char* usage = mcp::groupUsage(op.group);
        if (usage == nullptr) {
            mari_ctx.fail(__FILE__, __LINE__,
                          std::string("그룹에 안내문이 없다: ") + op.group + " (" + op.name + ")");
        } else {
            CHECK(desc.find("언제: ") != std::string::npos);
            CHECK(desc.find(usage) != std::string::npos);
        }

        const Json& schema = (*tool)["inputSchema"];
        CHECK_EQ(schema["type"].asString(), std::string("object"));
        // 🔴 모르는 키는 거절이다 — agent-api 디스패치와 같은 말을 해야 한다.
        CHECK_EQ(schema["additionalProperties"].asBool(true), false);

        const Json& props = schema["properties"];
        for (const agent::ParamSpec& p : op.params) {
            CHECK(props.has(p.name));
            CHECK_EQ(props[p.name]["description"].asString(), std::string(p.desc));
            bool listed = false;
            for (usize i = 0; i < schema["required"].size(); ++i) {
                listed = listed || schema["required"].at(i).asString() == p.name;
            }
            CHECK_EQ(listed, p.required);
        }
        // 🔴 전 연산이 시각 피드백을 받는다(docs/05 2.1). 예외가 있으면 안 된다.
        CHECK(props.has(agent::kUniversalViewParam));

        // 🔴 출처를 고르는 파라미터가 생성될 수 없다 — 표에 없기 때문이다(docs/05 3.1).
        for (const std::string& key : props.keys()) {
            CHECK_NE(key, std::string("origin"));
            CHECK_NE(key, std::string("agentId"));
        }
    }
    CHECK(checked > 10);
}

MARI_TEST(mcp_schema_types_cover_the_table_notation) {
    // 표가 쓰는 표기를 스키마가 전부 번역할 수 있어야 한다.
    // 못 하면 그 파라미터는 AI 에게 "아무 타입이나"로 보인다.
    for (const agent::OpSpec& op : agent::opTable()) {
        for (const agent::ParamSpec& p : op.params) {
            const Json t = mcp::schemaTypeOf(p.type);
            if (t.isNull()) {
                mari_ctx.fail(__FILE__, __LINE__,
                              std::string("스키마 타입을 모른다: ") + p.type + " (" + op.name +
                                  "." + p.name + ")");
            }
        }
    }
    const Json intType = mcp::schemaTypeOf("int");
    const Json boolType = mcp::schemaTypeOf("bool");
    CHECK_EQ(intType.asString(), std::string("integer"));
    CHECK_EQ(boolType.asString(), std::string("boolean"));
    const Json either = mcp::schemaTypeOf("string|int");
    CHECK(either.isArray());
    CHECK_EQ(either.size(), usize{2});
    CHECK_EQ(either.at(0).asString(), std::string("string"));
    CHECK_EQ(either.at(1).asString(), std::string("integer"));
    CHECK(mcp::schemaTypeOf("color").isArray()); // "#RRGGBB" · [r,g,b] · {r,g,b}
}

// ── ② 실제 JSON-RPC 바이트 왕복 ──────────────────────────────────────────

MARI_TEST(mcp_initialize_and_tools_list_roundtrip) {
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    mcp::McpServer server(*session);
    CHECK_EQ(server.initialized(), false);

    const std::string initLine =
        rpc("1", "initialize",
            "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
            "\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}}");
    const Json init = parseOrFail(server.handleLine(initLine), mari_ctx, __LINE__);
    CHECK_EQ(init["jsonrpc"].asString(), std::string("2.0"));
    CHECK_EQ(init["id"].asInt(), i64{1});
    CHECK(!init.has("error"));
    CHECK_EQ(init["result"]["protocolVersion"].asString(), std::string("2025-06-18"));
    CHECK_EQ(init["result"]["serverInfo"]["name"].asString(), std::string("mari-paint"));
    CHECK(init["result"]["capabilities"]["tools"].isObject());
    CHECK(!init["result"]["instructions"].asString().empty());
    CHECK_EQ(server.initialized(), true);
    CHECK_EQ(server.negotiatedVersion(), std::string("2025-06-18"));

    // 🔴 알림에는 응답하지 않는다(JSON-RPC 2.0). 한 줄이라도 더 내보내면 규약 위반이다.
    CHECK_EQ(server.handleLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"),
             std::string());

    const Json listed = parseOrFail(server.handleLine(rpc("2", "tools/list")), mari_ctx, __LINE__);
    CHECK_EQ(listed["id"].asInt(), i64{2});
    CHECK_EQ(listed["result"]["tools"].size(), mcp::toolsArray().size());

    // ping 은 빈 객체다.
    const Json pong = parseOrFail(server.handleLine(rpc("3", "ping")), mari_ctx, __LINE__);
    CHECK(pong["result"].isObject());
    CHECK_EQ(pong["result"].size(), usize{0});
}

MARI_TEST(mcp_protocol_version_negotiation) {
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    // 우리가 아는 판을 대면 그 판으로 맞춘다.
    mcp::McpServer older(*session);
    const Json a = parseOrFail(
        older.handleLine(rpc("1", "initialize", "{\"protocolVersion\":\"2024-11-05\"}")), mari_ctx,
        __LINE__);
    CHECK_EQ(a["result"]["protocolVersion"].asString(), std::string("2024-11-05"));

    // 모르는 판이면 우리 판을 제시한다(규약이 시키는 대로. 끊지 않는다).
    mcp::McpServer future(*session);
    const Json b = parseOrFail(
        future.handleLine(rpc("1", "initialize", "{\"protocolVersion\":\"1999-01-01\"}")), mari_ctx,
        __LINE__);
    CHECK_EQ(b["result"]["protocolVersion"].asString(), std::string(mcp::kProtocolVersion));
    CHECK(mcp::isKnownProtocolVersion(mcp::kProtocolVersion));
    CHECK(!mcp::isKnownProtocolVersion("1999-01-01"));
}

// ── ③ 🔴 AI 가 자기가 그린 걸 본다 ───────────────────────────────────────

MARI_TEST(mcp_tools_call_carries_image_content) {
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    mcp::McpServer server(*session);

    const Json made = parseOrFail(
        server.handleLine(rpc("1", "tools/call",
                              "{\"name\":\"doc_create\",\"arguments\":{\"width\":96,"
                              "\"height\":96}}")),
        mari_ctx, __LINE__);
    CHECK_EQ(made["result"]["isError"].asBool(true), false);

    // 획 하나. 기본 시각 피드백이 dirty 라 바뀐 곳이 그대로 돌아온다.
    const Json drawn = parseOrFail(
        server.handleLine(rpc("2", "tools/call",
                              "{\"name\":\"stroke\",\"arguments\":{\"points\":"
                              "[[10,10],[80,80]],\"color\":\"#ff0000\",\"size\":8}}")),
        mari_ctx, __LINE__);
    CHECK(!drawn.has("error"));
    const Json& result = drawn["result"];
    CHECK_EQ(result["isError"].asBool(true), false);

    const Json& content = result["content"];
    CHECK(content.isArray());
    CHECK(content.size() >= 2);

    // 🔴 이미지 콘텐츠가 실제로 실려 있다.
    const Json& image = contentOfType(content, "image");
    if (!image.isObject()) {
        CHECK_FAIL("tools/call 결과에 이미지 콘텐츠가 없다 — AI 가 자기 그림을 못 본다");
        return;
    }
    CHECK_EQ(image["mimeType"].asString(), std::string("image/png"));
    const std::string b64 = image["data"].asString();
    CHECK(!b64.empty());

    Result<std::vector<u8>> png = agent::base64Decode(b64);
    CHECK(png.ok());
    if (png.ok()) {
        const std::vector<u8>& bytes = png.value();
        CHECK(bytes.size() > 8);
        // 진짜 PNG 인가(시그니처 8바이트).
        const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        for (usize i = 0; i < 8 && i < bytes.size(); ++i) {
            CHECK_EQ(static_cast<int>(bytes[i]), static_cast<int>(sig[i]));
        }
    }

    // 텍스트 콘텐츠는 무슨 일이 있었는지 말한다.
    const Json& text = contentOfType(content, "text");
    CHECK(text.isObject());
    const std::string body = text["text"].asString();
    CHECK(body.find("\"op\":\"stroke\"") != std::string::npos);
    CHECK(body.find("\"origin\":\"agent\"") != std::string::npos); // 출처는 언제나 Agent
    CHECK(body.find("dirtyRect") != std::string::npos);
    // 🔴 같은 base64 를 두 번 싣지 않는다. 컨텍스트만 두 배로 먹고 새 정보는 0 이다.
    CHECK(body.find(b64) == std::string::npos);
    CHECK(body.find("mcp-image-content") != std::string::npos);

    // 기계가 읽을 몫에도 픽셀은 없다. 좌표·크기는 남는다.
    const Json& structured = result["structuredContent"];
    CHECK_EQ(structured["ok"].asBool(false), true);
    CHECK(!structured["image"].has("png"));
    CHECK_EQ(structured["image"]["w"].asInt(), i64{96});

    // view:none 이면 이미지가 없다(빠른 경로). 요청한 대로 돌아온다.
    const Json quiet = parseOrFail(
        server.handleLine(rpc("3", "tools/call",
                              "{\"name\":\"stroke\",\"arguments\":{\"points\":[[5,5],[20,20]],"
                              "\"view\":\"none\"}}")),
        mari_ctx, __LINE__);
    CHECK(!contentOfType(quiet["result"]["content"], "image").isObject());
}

MARI_TEST(mcp_render_tool_returns_the_whole_canvas) {
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    mcp::McpServer server(*session);
    (void)server.handleLine(
        rpc("1", "tools/call", "{\"name\":\"doc_create\",\"arguments\":{\"width\":40,\"height\":24}}"));
    const Json shown =
        parseOrFail(server.handleLine(rpc("2", "tools/call", "{\"name\":\"render\"}")), mari_ctx,
                    __LINE__);
    const Json& image = contentOfType(shown["result"]["content"], "image");
    CHECK(image.isObject());
    CHECK_EQ(shown["result"]["structuredContent"]["image"]["w"].asInt(), i64{40});
    CHECK_EQ(shown["result"]["structuredContent"]["image"]["h"].asInt(), i64{24});
}

// ── ④ 잘못된 요청은 규약대로 거절된다(크래시가 아니다) ────────────────────

MARI_TEST(mcp_bad_requests_get_protocol_errors) {
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    mcp::McpServer server(*session);

    const auto errorOf = [&](const std::string& line, int at) {
        const Json r = parseOrFail(server.handleLine(line), mari_ctx, at);
        return r["error"]["code"].asInt();
    };

    // 깨진 JSON — 읽지 못했으므로 id 는 null 이다(JSON-RPC 2.0 §5).
    const Json broken = parseOrFail(server.handleLine("{\"jsonrpc\": "), mari_ctx, __LINE__);
    CHECK_EQ(broken["error"]["code"].asInt(), mcp::kRpcParseError);
    CHECK(broken["id"].isNull());
    CHECK_EQ(broken["jsonrpc"].asString(), std::string("2.0"));

    CHECK_EQ(errorOf("[1,2,3]", __LINE__), mcp::kRpcInvalidRequest);
    CHECK_EQ(errorOf("\"안녕\"", __LINE__), mcp::kRpcInvalidRequest);
    CHECK_EQ(errorOf("{\"jsonrpc\":\"2.0\",\"id\":5}", __LINE__), mcp::kRpcInvalidRequest);
    CHECK_EQ(errorOf(rpc("6", "there/is/no/such/method"), __LINE__), mcp::kRpcMethodNotFound);
    CHECK_EQ(errorOf(rpc("7", "tools/call", "{\"name\":\"no_such_tool\"}"), __LINE__),
             mcp::kRpcInvalidParams);
    CHECK_EQ(errorOf(rpc("8", "tools/call", "{}"), __LINE__), mcp::kRpcInvalidParams);
    CHECK_EQ(errorOf(rpc("9", "tools/call", "{\"name\":\"render\",\"arguments\":5}"), __LINE__),
             mcp::kRpcInvalidParams);
    // 미지원 연산은 도구가 아니다 → 도구 이름으로도 부를 수 없다.
    CHECK_EQ(errorOf(rpc("10", "tools/call", "{\"name\":\"select_invert\"}"), __LINE__),
             mcp::kRpcInvalidParams);

    // 빈 줄·공백 줄은 메시지가 아니다. 응답하지 않는다.
    CHECK_EQ(server.handleLine(""), std::string());
    CHECK_EQ(server.handleLine("   \r\n"), std::string());
    // 우리가 요청한 적 없는 **응답**에는 응답하지 않는다(무한 왕복 방지).
    CHECK_EQ(server.handleLine("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}"), std::string());
    // id 없는 요청은 알림이다 — 모르는 메서드여도 응답하지 않는다.
    CHECK_EQ(server.handleLine("{\"jsonrpc\":\"2.0\",\"method\":\"who/knows\"}"), std::string());

    // 🔴 여기까지 오면 던지지도, 죽지도 않았다는 뜻이다. 세션은 멀쩡하다.
    const Json alive = parseOrFail(server.handleLine(rpc("11", "tools/list")), mari_ctx, __LINE__);
    CHECK(alive["result"]["tools"].size() > 0);
}

MARI_TEST(mcp_op_failure_is_isError_not_rpc_error) {
    // 🔴 연산 실패는 **AI 에게 보여야 한다.** JSON-RPC error 로 감추면 AI 는 이유를 못 보고
    //    같은 실수를 반복한다. MCP 가 isError 를 따로 둔 이유다.
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    mcp::McpServer server(*session);

    // 문서가 없는데 그리려 했다.
    const Json noDoc = parseOrFail(
        server.handleLine(rpc("1", "tools/call",
                              "{\"name\":\"stroke\",\"arguments\":{\"points\":[[1,1],[2,2]]}}")),
        mari_ctx, __LINE__);
    CHECK(!noDoc.has("error")); // 규약 실패가 아니다
    CHECK_EQ(noDoc["result"]["isError"].asBool(false), true);
    const std::string why = contentOfType(noDoc["result"]["content"], "text")["text"].asString();
    CHECK(why.find("\"ok\":false") != std::string::npos);
    CHECK(why.find("\"message\"") != std::string::npos);

    // 모르는 인자 키도 마찬가지다 — 오타가 조용히 무시되면 AI 는 이유를 못 찾는다.
    const Json typo = parseOrFail(
        server.handleLine(rpc("2", "tools/call",
                              "{\"name\":\"doc_create\",\"arguments\":{\"width\":8,\"height\":8,"
                              "\"widht\":8}}")),
        mari_ctx, __LINE__);
    CHECK(!typo.has("error"));
    CHECK_EQ(typo["result"]["isError"].asBool(false), true);
}

// ── stdio 전송과 CLI 연결 ────────────────────────────────────────────────

MARI_TEST(mcp_stdio_is_line_delimited) {
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    std::istringstream in(rpc("1", "initialize", "{\"protocolVersion\":\"2025-06-18\"}") + "\n" +
                          "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n" +
                          "\n" + // 빈 줄은 흘려보낸다
                          rpc("2", "tools/list") + "\r\n" + // CRLF 도 받는다
                          rpc("3", "tools/call", "{\"name\":\"doc_create\",\"arguments\":{"
                                                 "\"width\":16,\"height\":16}}") +
                          "\n");
    std::ostringstream out;
    std::ostringstream err;
    CHECK_EQ(mcp::serveStdio(*session, in, out, err), 0);

    // 응답은 요청 셋에 대해 정확히 셋이다(알림·빈 줄에는 응답이 없다).
    std::istringstream lines(out.str());
    std::string line;
    std::vector<Json> got;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        got.push_back(parseOrFail(line, mari_ctx, __LINE__));
    }
    CHECK_EQ(got.size(), usize{3});
    if (got.size() == 3) {
        CHECK_EQ(got[0]["id"].asInt(), i64{1});
        CHECK_EQ(got[1]["id"].asInt(), i64{2});
        CHECK_EQ(got[2]["id"].asInt(), i64{3});
        CHECK_EQ(got[2]["result"]["isError"].asBool(true), false);
    }
    // 🔴 메시지 안에 개행이 없다. 있으면 프레이밍이 깨진다.
    for (const Json& j : got) {
        CHECK(j.dump().find('\n') == std::string::npos);
    }
}

MARI_TEST(mcp_cli_starts_the_server) {
    // `mari-paint --headless --mcp --stdio` (docs/05 2.8)
    Result<cli::CliOptions> opt =
        cli::parseArgs({"--headless", "--mcp", "--stdio", "--agent-id", "mcp-cli"});
    CHECK(opt.ok());
    if (opt.ok()) {
        CHECK_EQ(opt.value().mcp, true);
        CHECK_EQ(opt.value().stdio, true);
    }
    // `--mcp` 만 줘도 stdio 로 뜬다(지금 전송은 그것뿐이다).
    Result<cli::CliOptions> onlyMcp = cli::parseArgs({"--headless", "--mcp"});
    CHECK(onlyMcp.ok());
    if (onlyMcp.ok()) {
        CHECK_EQ(onlyMcp.value().stdio, true);
    }
    // 전송만 고르는 것은 할 일이 아니다. 모드를 겹쳐 주는 것도 거절한다.
    CHECK(!cli::parseArgs({"--stdio"}).ok());
    CHECK(!cli::parseArgs({"--mcp", "--serve", ":0"}).ok());
    CHECK(!cli::parseArgs({"--mcp", "--exec", "{}"}).ok());

    std::istringstream in(rpc("1", "initialize", "{\"protocolVersion\":\"2025-06-18\"}") + "\n" +
                          rpc("2", "tools/list") + "\n");
    std::ostringstream out;
    std::ostringstream err;
    const int rc = cli::runCli({"--headless", "--mcp", "--stdio", "--agent-id", "mcp-cli"}, out,
                               err, in);
    CHECK_EQ(rc, cli::kExitOk);

    // 🔴 stdout 에는 MCP 메시지 말고 **아무 것도** 없다. 배너 한 줄이면 클라이언트가 끊는다.
    std::istringstream lines(out.str());
    std::string line;
    usize count = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        const Json j = parseOrFail(line, mari_ctx, __LINE__);
        CHECK_EQ(j["jsonrpc"].asString(), std::string("2.0"));
        ++count;
    }
    CHECK_EQ(count, usize{2});
    // 진단은 stderr 로만 간다.
    CHECK(!err.str().empty());
}

// ── 얇은 어댑터라는 것 ───────────────────────────────────────────────────

MARI_TEST(mcp_instructions_only_name_real_tools) {
    // initialize 의 안내문이 실제로 없는 도구를 가리키면 AI 가 헛발질한다.
    // 안내문에 나오는 ASCII 낱말은 **전부 진짜 도구 이름**이어야 한다
    // (도구가 아닌 낱말은 아래 목록에 있는 것만 허용한다).
    std::unique_ptr<agent::AgentSession> session = openSession(mari_ctx);
    if (!session) {
        return;
    }
    mcp::McpServer server(*session);
    const Json init = parseOrFail(
        server.handleLine(rpc("1", "initialize", "{\"protocolVersion\":\"2025-06-18\"}")), mari_ctx,
        __LINE__);
    const std::string text = init["result"]["instructions"].asString();
    CHECK(!text.empty());

    const std::vector<std::string> notTools = {"atomic", "origin", "agent"};
    usize named = 0;
    for (const std::string& word : lowercaseWords(text)) {
        bool allowed = false;
        for (const std::string& w : notTools) {
            allowed = allowed || w == word;
        }
        if (allowed) {
            continue;
        }
        if (mcp::findTool(word) == nullptr) {
            mari_ctx.fail(__FILE__, __LINE__, "안내문이 없는 도구를 가리킨다: " + word);
        } else {
            ++named;
        }
    }
    CHECK(named >= 5); // 최소한 시작·그리기·보기·스냅샷은 알려 준다
}

MARI_TEST(mcp_adds_no_capability_of_its_own) {
    // 🔴 MCP 는 얇은 어댑터다(docs/05 1절). 도구가 곧 연산이고, 그 반대도 참이어야 한다.
    //    도구 하나가 표에 없는 무언가를 하고 있으면 그게 "두 번째 구현"의 시작이다.
    const Json tools = mcp::toolsArray();
    for (usize i = 0; i < tools.size(); ++i) {
        const std::string name = tools.at(i)["name"].asString();
        const agent::OpSpec* spec = mcp::findTool(name);
        CHECK(spec != nullptr);
        if (spec != nullptr) {
            CHECK(agent::findOp(spec->name) == spec); // 표의 그 항목 **자체**다
        }
    }
    // 반대 방향: 지원되는 연산 중 도구가 없는 것은 없다.
    for (const agent::OpSpec& op : agent::opTable()) {
        if (mcp::isExported(op)) {
            CHECK(mcp::findTool(mcp::toolNameOf(op.name)) == &op);
        }
    }
}

MARI_TEST_MAIN()
