// Mari Paint — MCP 도구 스키마 생성 (docs/05 4절)
//
// 🔴 **여기에 도구 목록이 없다.** 있으면 안 된다.
//    docs/05 4절: "MCP 서버는 이 표를 읽어서 도구 목록을 생성할 뿐,
//    따로 손으로 유지하지 않는다."
//    이 파일이 하는 일은 `mari::agent::opTable()` 한 벌을 MCP 가 읽는 모양으로
//    **번역**하는 것뿐이다. 연산을 하나 더하면 도구도 저절로 하나 더 생기고,
//    파라미터를 하나 더하면 스키마도 저절로 따라온다.
//    `tests/mcp/test_mcp.cpp` 의 `mcp_tools_generated` 가 그 일치를 강제한다.
//
// 🔴 능력을 여기 두지 마라(docs/05 1절). 이 모듈은 얇은 어댑터다 —
//    연산도, 픽셀도, 출처 판단도 여기에 없다. 전부 agent-api 가 갖고 있다.
//
// 🔴 출처(origin): MCP 로 들어온 호출도 같은 `AgentSession` 을 지난다.
//    그래서 origin 은 무조건 Agent 다. 도구 스키마 어디에도 origin 파라미터가 없다 —
//    agent-api 의 표에 없으니 생성될 수가 없다(docs/05 3.1).
#ifndef MARI_MCP_TOOLS_HPP
#define MARI_MCP_TOOLS_HPP

#include <mari/agent/capabilities.hpp>
#include <mari/agent/json.hpp>

#include <string>
#include <string_view>

namespace mari::mcp {

using agent::Json;

/// 연산 이름 → MCP 도구 이름. `doc.create` → `doc_create`.
///
/// 왜 바꾸나: MCP 클라이언트 다수가 도구 이름을 `[A-Za-z0-9_-]{1,64}` 로 제한한다.
/// 점을 그대로 흘리면 어떤 클라이언트에서는 도구가 조용히 사라진다.
/// 번역은 **여기 한 곳**에서만 한다 — 연산 이름의 정본은 여전히 agent-api 다.
[[nodiscard]] std::string toolNameOf(std::string_view opName);

/// 반대 방향. 표에 없는 이름이면 빈 문자열.
[[nodiscard]] std::string opNameOf(std::string_view toolName);

/// 이 연산을 도구로 내보내는가.
///
/// 🔴 `supported == false` 인 연산은 내보내지 않는다. "되는 척하지 않는다"(docs/04 6절)를
///    MCP 에서는 **도구를 주지 않는 것**으로 지킨다 — 주면 AI 가 반드시 한 번은 불러 보고
///    실패한다. 대신 `capabilities` 도구가 미지원 연산과 그 이유를 전부 돌려주므로
///    정보가 사라지지는 않는다.
[[nodiscard]] bool isExported(const agent::OpSpec& op) noexcept;

/// 도구 이름으로 연산을 찾는다. 없거나 미지원이면 nullptr.
[[nodiscard]] const agent::OpSpec* findTool(std::string_view toolName);

/// 그룹별 "언제 쓰나" 한 줄(docs/05 요구: 각 도구에 언제 쓰는지).
///
/// 도구마다 손으로 적지 않는다 — 그러면 그게 두 번째 목록이다.
/// 연산 표의 `group` 을 키로 삼아 **그룹 수만큼만** 문장을 둔다.
/// 모르는 그룹이면 nullptr 를 돌려주고, 테스트가 그 구멍을 잡는다.
[[nodiscard]] const char* groupUsage(std::string_view group) noexcept;

/// 파라미터 타입 문자열(agent-api 표기)을 JSON Schema `type` 값으로.
/// `"string|int"` 처럼 `|` 로 갈라진 것과 `layer`·`region`·`color`·`view` 같은
/// 시맨틱 타입을 모두 받는다. 모르는 표기는 제약을 걸지 않는다(막지 않는다).
[[nodiscard]] Json schemaTypeOf(std::string_view apiType);

/// 연산 하나의 `inputSchema`(JSON Schema draft-07 부분집합).
[[nodiscard]] Json inputSchemaOf(const agent::OpSpec& op);

/// 도구 하나 `{name, description, inputSchema}`.
[[nodiscard]] Json toolJson(const agent::OpSpec& op);

/// `tools/list` 의 `tools` 배열. **전부 `opTable()` 에서 나온다.**
[[nodiscard]] Json toolsArray();

} // namespace mari::mcp

#endif // MARI_MCP_TOOLS_HPP
