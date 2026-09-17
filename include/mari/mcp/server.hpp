// Mari Paint — MCP 서버 어댑터 (docs/05 1절 · 4절 · A3)
//
//                   mari-core
//                       │
//             ┌─────────▼──────────┐
//             │   mari-agent-api   │   ← 능력은 **전부** 여기 있다
//             └─────────┬──────────┘
//                 ┌─────┴─────┐
//              MCP 서버 ←여기  CLI · JSON-RPC · COM · UI
//
// 🔴 **얇다.** 이 모듈이 하는 일은 넷뿐이고 넷 다 전송 문제다:
//    ① MCP 규약(JSON-RPC 2.0 over stdio, 한 줄 = 메시지 하나) 을 말한다
//    ② `tools/list` 를 agent-api 연산 표에서 **생성한다**(tools.hpp)
//    ③ `tools/call` 의 인자를 `{"op": ...}` 로 접어 세션에 넘긴다
//    ④ 🔴 응답 봉투의 PNG 를 **MCP 이미지 콘텐츠로 싣는다** — AI 가 자기가 그린 걸
//       봐야 한다(docs/05 2.1). 이게 이 파일에서 제일 중요한 한 줄이다.
//    연산을 여기서 다시 구현하지 않는다. 두 벌이 되면 "CLI 로는 되는데 MCP 로는
//    안 되는 것"이 생기고, 그게 docs/05 1절이 막으려는 상황이다.
//
// 🔴 출처(origin): 같은 `AgentSession` 을 지나므로 MCP 로 들어온 획도 무조건 Agent 다.
//    도구 스키마에 origin 파라미터가 없다 — agent-api 표에 없으니 생길 수가 없다.
//
// 🔴 경계선(docs/03 2절): 서명도, 해시체인 봉인도, 등급 판정도 여기에 없다.
//
// 프레이밍: MCP stdio 전송은 **줄 구분 JSON** 이다. 길이 헤더(Content-Length)를 쓰지 않고,
// 메시지 안에 개행이 들어가지 않는다(`Json::dump()` 가 한 줄로 찍는다).
#ifndef MARI_MCP_SERVER_HPP
#define MARI_MCP_SERVER_HPP

#include <mari/agent/json.hpp>
#include <mari/agent/session.hpp>
#include <mari/mcp/tools.hpp>

#include <iosfwd>
#include <string>
#include <string_view>

namespace mari::mcp {

using agent::Json;

/// 클라이언트에게 대는 이름/판.
inline constexpr const char* kServerName = "mari-paint";
/// 우리가 말하는 MCP 규약 판. 클라이언트가 아는 판을 대면 그쪽에 맞춘다.
inline constexpr const char* kProtocolVersion = "2025-06-18";

/// 우리가 아는 규약 판인가. 모르는 판이면 우리 판을 그대로 돌려준다(규약이 시키는 대로).
[[nodiscard]] bool isKnownProtocolVersion(std::string_view v) noexcept;

/// JSON-RPC 2.0 표준 오류 코드. 숫자를 흩뿌리지 않는다.
inline constexpr i64 kRpcParseError = -32700;
inline constexpr i64 kRpcInvalidRequest = -32600;
inline constexpr i64 kRpcMethodNotFound = -32601;
inline constexpr i64 kRpcInvalidParams = -32602;
inline constexpr i64 kRpcInternalError = -32603;

/// 응답 봉투를 MCP `content` 배열로 바꾼다.
///
/// 🔴 이미지가 있으면 `{"type":"image","data":"<base64 png>","mimeType":"image/png"}` 를
///    싣는다. **AI 가 자기가 그린 걸 봐야 한다**(docs/05 2.1).
/// 🔴 텍스트 콘텐츠에서는 같은 base64 를 **뺀다.** 두 번 실으면 컨텍스트만 두 배로 먹고
///    AI 에게 새 정보는 0 이다. 대신 크기·좌표 같은 메타는 남긴다.
[[nodiscard]] Json contentFromEnvelope(const Json& envelope);

/// MCP 세션 하나. `AgentSession` 을 빌려 쓴다(소유하지 않는다).
///
/// 스레드 규약: 한 서버는 한 스레드가 쓴다(세션과 같은 규약).
class McpServer {
public:
    explicit McpServer(agent::AgentSession& session) noexcept : session_(session) {}

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    /// 메시지 한 줄을 처리해 응답 한 줄을 만든다(개행은 붙이지 않는다).
    ///
    /// 🔴 **절대 던지지 않는다.** 깨진 JSON 도, 모르는 메서드도 규약대로 된 오류 응답이다.
    /// 알림(`id` 가 없는 요청)과 빈 줄에는 **응답하지 않는다** — 빈 문자열을 돌려준다.
    /// JSON-RPC 2.0 이 알림에 응답을 금지한다.
    [[nodiscard]] std::string handleLine(std::string_view line);

    /// `initialize` 를 받았나(규약 순서 감시용).
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    /// 클라이언트와 합의한 규약 판.
    [[nodiscard]] const std::string& negotiatedVersion() const noexcept { return version_; }
    /// 처리한 메시지 수.
    [[nodiscard]] u64 handled() const noexcept { return handled_; }

private:
    /// 메서드 하나의 결과. `errorCode == 0` 이면 `payload` 가 곧 JSON-RPC `result` 다.
    ///
    /// 🔴 두 종류의 실패를 구분하려고 둔다 — MCP 가 그 둘을 다르게 다룬다:
    ///    · **규약 실패**(모르는 도구, 인자 모양이 틀림) → JSON-RPC `error`.
    ///      AI 가 고칠 수 있는 종류가 아니라 클라이언트가 볼 문제다.
    ///    · **연산 실패**(레이어가 없다, 문서가 안 열렸다) → `isError: true` 인 **성공 응답**.
    ///      MCP 가 이렇게 하라고 정한 이유가 있다: 그래야 그 문장이 AI 에게 보이고,
    ///      AI 가 스스로 고쳐서 다시 부른다. error 로 감추면 AI 는 이유를 못 본다.
    struct RpcOutcome {
        Json payload = Json::null();
        i64 errorCode = 0;
        std::string errorMessage;
    };

    [[nodiscard]] Json initialize(const Json& params);
    [[nodiscard]] RpcOutcome callTool(const Json& params);

    agent::AgentSession& session_;
    std::string version_ = kProtocolVersion;
    u64 handled_ = 0;
    bool initialized_ = false;
};

/// stdio 로 돈다. 입력이 끝나면(EOF) 0 을 돌려준다.
///
/// `err` 는 진단용이다 — 🔴 **stdout 에는 JSON 메시지 말고 아무 것도 쓰면 안 된다.**
/// 배너 한 줄이 섞이면 MCP 클라이언트가 곧바로 연결을 끊는다.
[[nodiscard]] int serveStdio(agent::AgentSession& session, std::istream& in, std::ostream& out,
                             std::ostream& err);

} // namespace mari::mcp

#endif // MARI_MCP_SERVER_HPP
