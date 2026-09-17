// Mari Paint — 줄 단위 JSON-RPC 서버 (docs/05 2.8 "--serve :7777")
//
// 프레이밍은 **한 줄 = 요청 하나, 한 줄 = 응답 하나**다. 길이 헤더를 쓰지 않는다 —
// `nc`/`socat`/파이썬 한 줄로 두드려 볼 수 있는 게 에이전트 입장에서 훨씬 낫다.
//
// 두 가지 요청 형태를 다 받는다:
//   · `{"op":"doc.describe"}`  → agent-api 의 응답 봉투 그대로
//   · `{"jsonrpc":"2.0","method":"doc.describe","params":{...},"id":1}` → JSON-RPC 2.0 응답
// 어느 쪽이든 **같은 AgentSession 을 지난다.** 전송 규약이 능력을 바꾸지 않는다(docs/05 1절).
//
// 🔴 `quit` 은 **전송 계층의 명령이다.** 능력 계층(agent-api)에는 그런 연산이 없고,
//    여기서도 세션에 밀어 넣지 않는다 — 연결을 끊고 서버를 멈출 뿐이다.
// 🔴 기본 바인딩은 루프백이다. 이 서버에는 인증이 없다 —
//    외부에 열어 주는 편의 코드를 여기 넣지 않는다.
#ifndef MARI_CLI_SERVER_HPP
#define MARI_CLI_SERVER_HPP

#include <mari/agent/session.hpp>

#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>

namespace mari::cli {

/// 바인딩 주소.
struct ServeAddress {
    std::string host = "127.0.0.1";
    u16 port = 0; ///< 0 이면 커널이 고른다(테스트가 이걸 쓴다)
};

/// ":7777" · "127.0.0.1:7777" · "7777" 을 읽는다.
/// 호스트를 생략하면 루프백이다 — 실수로 0.0.0.0 에 열리지 않게 한다.
[[nodiscard]] Result<ServeAddress> parseServeAddress(std::string_view spec);

/// 요청 한 줄을 처리해 응답 한 줄을 만든다(개행은 붙이지 않는다).
/// `quit` 요청이면 `*quit` 을 true 로 세운다. **던지지 않는다** —
/// 깨진 JSON 도 오류 응답 한 줄로 돌아온다.
[[nodiscard]] std::string handleRpcLine(agent::AgentSession& session, std::string_view line,
                                        bool* quit = nullptr);

/// TCP 로 받는다. 한 번에 한 연결을 처리한다(세션이 스레드 하나 규약이라 그게 맞다).
/// `onReady` 는 실제로 바인딩된 포트를 알려준다(port 0 일 때 필요하다).
/// `quit` 이 들어오면 그 응답을 보낸 뒤 서버를 멈춘다.
[[nodiscard]] Result<void> serveTcp(agent::AgentSession& session, const ServeAddress& addr,
                                    std::ostream& err,
                                    const std::function<void(u16)>& onReady = {});

} // namespace mari::cli

#endif // MARI_CLI_SERVER_HPP
