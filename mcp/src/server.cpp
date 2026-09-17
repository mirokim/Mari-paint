// Mari Paint — MCP 서버 어댑터 본체. 선언은 include/mari/mcp/server.hpp.
//
// 🔴 이 파일에 연산이 하나도 없다. 전부 `AgentSession::execute()` 로 간다(docs/05 1절).
#include <mari/mcp/server.hpp>

#include <istream>
#include <ostream>
#include <string>
#include <utility>

namespace mari::mcp {
namespace {

/// 앞뒤 공백·CR 를 걷어낸다. 줄 구분 프레이밍이라 CRLF 가 섞여 들어올 수 있다.
std::string_view trim(std::string_view s) noexcept {
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string rpcErrorLine(const Json& id, i64 code, const std::string& message) {
    Json root = Json::object();
    root.set("jsonrpc", Json::string("2.0"));
    root.set("id", id); // 🔴 못 읽은 요청의 id 는 null 이다(JSON-RPC 2.0 §5).
    Json e = Json::object();
    e.set("code", Json::integer(code));
    e.set("message", Json::string(message));
    root.set("error", std::move(e));
    return root.dump();
}

std::string rpcResultLine(const Json& id, Json result) {
    Json root = Json::object();
    root.set("jsonrpc", Json::string("2.0"));
    root.set("id", id);
    root.set("result", std::move(result));
    return root.dump();
}

Json textContent(std::string text) {
    Json c = Json::object();
    c.set("type", Json::string("text"));
    c.set("text", Json::string(std::move(text)));
    return c;
}

/// 봉투를 복사하되 이미지의 base64 만 뺀다. 나머지(좌표·크기·배율)는 남긴다 —
/// AI 가 "이 그림이 캔버스 어디인지"를 알아야 다음 획을 놓을 수 있다.
Json envelopeWithoutPixels(const Json& envelope) {
    Json out = Json::object();
    for (const std::string& key : envelope.keys()) {
        if (key != "image") {
            out.set(key, envelope[key]);
            continue;
        }
        const Json& img = envelope[key];
        if (!img.isObject()) {
            out.set(key, img);
            continue;
        }
        Json meta = Json::object();
        for (const std::string& ik : img.keys()) {
            if (ik == "png") {
                continue; // 같은 바이트를 두 번 싣지 않는다
            }
            meta.set(ik, img[ik]);
        }
        // 픽셀이 사라진 게 아니라 **이미지 콘텐츠로 옮겨 갔다**는 사실을 남긴다.
        meta.set("deliveredAs", Json::string("mcp-image-content"));
        out.set(key, std::move(meta));
    }
    return out;
}

} // namespace

bool isKnownProtocolVersion(std::string_view v) noexcept {
    return v == "2024-11-05" || v == "2025-03-26" || v == "2025-06-18";
}

Json contentFromEnvelope(const Json& envelope) {
    Json content = Json::array();

    // ① 무슨 일이 있었는지 — 텍스트가 먼저다. 그림만 오면 AI 가 좌표를 못 읽는다.
    content.push(textContent(envelopeWithoutPixels(envelope).dump()));

    // ② 🔴 그림. **AI 가 자기가 그린 걸 봐야 한다**(docs/05 2.1).
    const Json& img = envelope["image"];
    if (img.isObject() && img["png"].isString() && !img["png"].asString().empty()) {
        Json c = Json::object();
        c.set("type", Json::string("image"));
        c.set("data", img["png"]); // base64. MCP 이미지 콘텐츠는 base64 를 그대로 받는다
        c.set("mimeType", Json::string("image/png"));
        content.push(std::move(c));
    }
    return content;
}

Json McpServer::initialize(const Json& params) {
    const std::string asked = params["protocolVersion"].asString();
    // 규약: 클라이언트가 아는 판을 대면 그 판으로 맞춘다. 모르면 우리 판을 제시하고
    // 받아들일지는 클라이언트가 정한다.
    version_ = isKnownProtocolVersion(asked) ? asked : std::string(kProtocolVersion);
    initialized_ = true;

    Json caps = Json::object();
    Json tools = Json::object();
    // 도구 목록은 연산 표에서 생성되고 런타임에 늘어나지 않는다 → listChanged 는 false 다.
    tools.set("listChanged", Json::boolean(false));
    caps.set("tools", std::move(tools));

    Json info = Json::object();
    info.set("name", Json::string(kServerName));
    info.set("version", Json::string(session_.application().version()));

    Json out = Json::object();
    out.set("protocolVersion", Json::string(version_));
    out.set("capabilities", std::move(caps));
    out.set("serverInfo", std::move(info));
    // 🔴 여기 적는 것은 **쓰는 법**이지 능력이 아니다. 능력은 도구 목록이 말한다.
    out.set("instructions",
            Json::string(
                "Mari Paint — 타일 캔버스 페인트 엔진.\n"
                "· 먼저 doc_create 로 문서를 하나 연다. 그 다음에야 그릴 수 있다.\n"
                "· 그리기는 stroke 다. 점+필압 배열을 사람 펜과 같은 파이프라인에 태운다.\n"
                "· 🔴 결과는 이미지로 돌아온다. 몇 획마다 한 번은 render 로 눈으로 확인해라.\n"
                "· 실험하기 전에 snapshot 을 찍어라. O(1) 이라 공짜다. restore 로 되돌린다.\n"
                "· 획이 많으면 batch 로 묶어라. 왕복이 줄고 atomic 이면 롤백된다.\n"
                "· 어떤 붓·블렌드 모드가 있는지는 capabilities 가 런타임에 알려준다.\n"
                "· 🔴 이 서버로 들어온 획은 전부 origin=agent 로 기록된다. 고를 수 없다.\n"
                // 🔴 여기서 "밀어 준다"고 쓰면 거짓말이다. MCP 규약에는 응용 이벤트를
                //    모델에게 밀어 넣는 채널이 없다(docs/07 5절). 당겨 가라고 말한다.
                "· 이벤트는 밀어 주지 못한다(MCP 규약의 한계다). events_poll 로 당겨 가라."));
    return out;
}

McpServer::RpcOutcome McpServer::callTool(const Json& params) {
    RpcOutcome out;
    if (!params.isObject() || !params["name"].isString()) {
        out.errorCode = kRpcInvalidParams;
        out.errorMessage = "tools/call 에는 \"name\" 문자열이 필요하다";
        return out;
    }
    const std::string toolName = params["name"].asString();
    const agent::OpSpec* spec = findTool(toolName);
    if (spec == nullptr) {
        // 🔴 없는 도구는 규약 실패다 — 클라이언트가 우리 도구 목록을 안 읽었다는 뜻이다.
        out.errorCode = kRpcInvalidParams;
        out.errorMessage = "모르는 도구다: \"" + toolName + "\" (tools/list 로 목록을 받아라)";
        return out;
    }

    const Json& args = params["arguments"];
    if (!args.isNull() && !args.isObject()) {
        out.errorCode = kRpcInvalidParams;
        out.errorMessage = "\"arguments\" 는 객체여야 한다";
        return out;
    }

    // 🔴 인자를 그대로 연산 객체로 접는다. 여기서 값을 만들거나 고치지 않는다 —
    //    검증은 agent-api 의 표가 한다(모르는 키는 거기서 거절된다).
    Json request = args.isObject() ? args : Json::object();
    request.set(agent::kOpKey, Json::string(spec->name));

    const Json envelope = session_.execute(request);

    Json result = Json::object();
    result.set("content", contentFromEnvelope(envelope));
    // 연산 실패는 **성공 응답 안의 isError** 다. 그래야 이유가 AI 에게 보인다.
    result.set("isError", Json::boolean(!envelope["ok"].asBool(false)));
    // 기계가 읽을 몫. base64 는 이미지 콘텐츠로 이미 갔으므로 여기서는 뺀다.
    result.set("structuredContent", envelopeWithoutPixels(envelope));
    out.payload = std::move(result);
    return out;
}

std::string McpServer::handleLine(std::string_view line) {
    const std::string_view text = trim(line);
    if (text.empty()) {
        return {}; // 빈 줄은 메시지가 아니다
    }
    ++handled_;

    Result<Json> parsed = Json::parse(text);
    if (!parsed.ok()) {
        return rpcErrorLine(Json::null(), kRpcParseError,
                            "JSON 을 읽을 수 없다: " + parsed.message());
    }
    const Json& req = parsed.value();
    if (!req.isObject()) {
        return rpcErrorLine(Json::null(), kRpcInvalidRequest, "메시지는 JSON 객체여야 한다");
    }

    // 클라이언트가 보낸 **응답**(우리가 요청한 적 없다)은 조용히 흘린다.
    // 응답에 응답하면 무한 왕복이 된다.
    if (!req.has("method") && (req.has("result") || req.has("error"))) {
        return {};
    }

    const Json& id = req["id"];
    // 🔴 알림은 id 가 없다. JSON-RPC 2.0 이 알림에 **응답을 금지한다**.
    const bool notification = !req.has("id") || id.isNull();

    const Json& methodJson = req["method"];
    if (!methodJson.isString()) {
        if (notification) {
            return {};
        }
        return rpcErrorLine(id, kRpcInvalidRequest, "\"method\" 문자열이 없다");
    }
    const std::string method = methodJson.asString();
    const Json& params = req["params"];

    RpcOutcome outcome;
    if (method == "initialize") {
        outcome.payload = initialize(params);
    } else if (method == "ping") {
        outcome.payload = Json::object(); // 규약대로 빈 객체
    } else if (method == "tools/list") {
        Json r = Json::object();
        r.set("tools", toolsArray()); // 🔴 연산 표에서 생성된다. 손으로 유지하는 목록이 없다
        outcome.payload = std::move(r);
    } else if (method == "tools/call") {
        outcome = callTool(params);
    } else if (method.rfind("notifications/", 0) == 0) {
        return {}; // 알 든 모르든 알림에는 응답하지 않는다
    } else {
        outcome.errorCode = kRpcMethodNotFound;
        outcome.errorMessage = "모르는 메서드다: \"" + method + "\"";
    }

    if (notification) {
        return {};
    }
    if (outcome.errorCode != 0) {
        return rpcErrorLine(id, outcome.errorCode, outcome.errorMessage);
    }
    return rpcResultLine(id, std::move(outcome.payload));
}

int serveStdio(agent::AgentSession& session, std::istream& in, std::ostream& out,
               std::ostream& err) {
    McpServer server(session);
    std::string line;
    while (std::getline(in, line)) {
        const std::string response = server.handleLine(line);
        if (response.empty()) {
            continue; // 알림·빈 줄 — 규약상 응답이 없다
        }
        // 🔴 한 줄 = 메시지 하나. 매번 flush 한다 — 버퍼에 남으면 상대가 영원히 기다린다.
        out << response << "\n";
        out.flush();
    }
    err << "mcp: 입력이 끝났다 (메시지 " << server.handled() << "건 처리)\n";
    return 0;
}

} // namespace mari::mcp
