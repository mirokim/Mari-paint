// Mari Paint — 헤드리스 모드와 CLI (docs/05 2.8 · A2)
//
// 🔴 이 계층은 **얇다.** 능력은 전부 `mari::agent::AgentSession`(A1) 에 있다.
//    CLI 가 하는 일은 네 가지뿐이고, 그 넷은 전부 전송·입출력 문제다:
//      ① 명령줄과 스크립트 파일을 읽어 연산 목록으로 바꾼다
//      ② 연산을 순서대로 세션에 넣는다 (오류 시 중단/계속)
//      ③ 응답 안의 base64 PNG 를 파일로 떨군다 (`--out-dir`)
//      ④ 결과를 stdout 으로, 진행 상황을 stderr 로, 성패를 종료 코드로 낸다
//    연산을 여기서 다시 구현하지 않는다. 두 번째 구현이 생기면 "CLI 로는 되는데
//    MCP 로는 안 되는 것"이 생기고, 그게 docs/05 1절이 막으려는 바로 그 상황이다.
//
// 🔴 출력 규약 — 어기면 CLI 가 파이프에서 못 쓰게 된다.
//    · **stdout 은 JSON 값 하나뿐이다.** 배너·진행·경고는 전부 stderr 로 간다.
//    · 종료 코드: 0 성공 · 1 실행 실패 · 2 사용법 오류.
//      "실패했는데 0" 은 스크립트를 조용히 망가뜨린다.
//
// 🔴 출처(origin): CLI 에는 origin 을 고르는 옵션이 **없다.** `--agent-id` 로 이름만
//    댈 수 있고, 그 이름이 없으면 세션이 열리지 않는다(익명 AI 획 금지).
#ifndef MARI_CLI_RUNNER_HPP
#define MARI_CLI_RUNNER_HPP

#include <mari/agent/json.hpp>
#include <mari/agent/session.hpp>

#include <iosfwd>
#include <string>
#include <vector>

namespace mari::record {
/// 기록 배선의 공장. 정의는 include/mari/record/sigan_recorder.hpp.
/// 🔴 CLI 는 이걸 **꽂기만** 한다. publish() 를 부르지 않는다(docs/06 결정 ③).
class SiganRecorderFactory;
} // namespace mari::record

namespace mari::cli {

using agent::Json;

/// 종료 코드. 셸이 분기할 수 있게 셋으로 나눈다.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;

/// 명령줄 옵션.
struct CliOptions {
    bool headless = false;         ///< `--headless` (이 빌드는 어차피 헤드리스뿐이다)
    bool showVersion = false;      ///< `--version`
    bool showCapabilities = false; ///< `--capabilities`
    bool help = false;             ///< `--help`
    std::string scriptPath;        ///< `--script <file>`
    std::string execJson;          ///< `--exec '<json>'`
    std::string serveAddr;         ///< `--serve :7777`
    bool mcp = false;              ///< `--mcp` — MCP 서버로 기동(docs/05 2.8 · 4절)
    bool stdio = false;            ///< `--stdio` — MCP 전송을 표준입출력으로
    std::string outDir;            ///< `--out-dir <dir>`
    /// `--proof-out <path>` — 이 세션의 기록을 무서명 과정 로그(JSON)로 떨군다(docs/03 6절).
    /// 🔴 인증서가 아니다. 서명도 해시체인도 없다 — 등급은 "unsigned" 하나뿐이다.
    std::string proofOut;
    /// `--journal-dir <dir>` — 저널을 놓을 디렉터리. 주면 기록이 켜진다(docs/06).
    std::string journalDir;
    std::string agentId = "mari-cli"; ///< `--agent-id <id>`
    std::string view;              ///< `--view none|dirty|full|...` (연산이 직접 정하면 그쪽이 이긴다)
    bool continueOnError = false;  ///< `--continue-on-error`
    bool embedImages = false;      ///< `--embed-images` (base64 PNG 를 stdout 에 남긴다)
    bool pretty = false;           ///< `--pretty`
    bool quiet = false;            ///< `--quiet`
};

/// argv 를 읽는다. 모르는 옵션은 **거절한다** — 오타가 조용히 무시되면 안 된다.
[[nodiscard]] Result<CliOptions> parseArgs(const std::vector<std::string>& args);

/// 사용법 텍스트(stderr 로 나간다).
[[nodiscard]] std::string usageText();

/// 스크립트 한 벌. 배열이면 그대로 연산 목록이고,
/// 객체면 `{ "agentId": ..., "onError": "stop"|"continue", "ops": [...] }` 다.
struct Script {
    std::vector<Json> ops;
    std::string agentId;       ///< 비어 있으면 CLI 옵션을 쓴다
    bool continueOnError = false;
    bool hasOnError = false;   ///< 스크립트가 onError 를 직접 정했나
};
[[nodiscard]] Result<Script> parseScript(const Json& root);

/// 응답 봉투 안의 base64 PNG 를 파일로 떨군다.
/// · `outDir` 이 비어 있으면 아무 것도 하지 않는다.
/// · `keepBase64` 가 false 면 떨군 뒤 `png` 키를 **뺀다** — stdout 이 파이프를 타므로
///   같은 픽셀을 두 번 흘릴 이유가 없다. 대신 `path` 를 넣는다.
[[nodiscard]] Result<void> spillImage(Json& envelope, const std::string& outDir, usize seq,
                                      bool keepBase64);

/// 연산 목록을 실행한다. 진행 상황은 err 로, 결과 JSON 을 돌려준다.
/// `recorders` 를 주면 리포트에 문서 구간 집계가 함께 실린다(기록이 켜져 있을 때).
[[nodiscard]] Json runOps(agent::AgentSession& session, const std::vector<Json>& ops,
                          const CliOptions& opt, std::ostream& err, usize& failed,
                          const record::SiganRecorderFactory* recorders = nullptr);

/// CLI 본체. main() 은 이걸 부르기만 한다(테스트가 스트림을 갈아끼울 수 있게).
/// 표준입력은 `--mcp --stdio` 일 때만 읽는다.
[[nodiscard]] int runCli(const std::vector<std::string>& args, std::ostream& out,
                         std::ostream& err, std::istream& in);
/// 표준입력을 쓰는 짧은 형태. 실행파일이 부른다.
[[nodiscard]] int runCli(const std::vector<std::string>& args, std::ostream& out,
                         std::ostream& err);

} // namespace mari::cli

#endif // MARI_CLI_RUNNER_HPP
