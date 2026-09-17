// Mari Paint — 자기 설명 (docs/05 2.6 — "AI 가 문서를 안 읽어도 된다")
//
// 🔴 **이 표가 스키마 정본이다.**
//    · 디스패치가 이 표를 보고 연산을 찾는다 → 표에 없는 연산은 실행되지 않는다.
//    · 디스패치가 이 표를 보고 **모르는 파라미터를 거절한다** → 표에 없는 파라미터는
//      존재하지 않는다. 그래서 표와 구현이 **어긋날 수가 없다.**
//    · `capabilities` 연산은 이 표를 JSON 으로 그대로 내보낸다 →
//      MCP 도구 목록이 여기서 **자동 생성된다.** 손으로 유지하는 두 번째 목록이 없다
//      (docs/05 4절 "MCP 서버는 이 표를 읽어서 도구 목록을 생성할 뿐").
//
// 🔴 이 표에 **origin 파라미터는 없다.** 있으면 그게 위조 수단이다(docs/05 3.1).
//    `tests/agent/test_api.cpp` 의 `no_origin_override` 가 표 전체를 훑어 그것을 검사한다.
#ifndef MARI_AGENT_CAPABILITIES_HPP
#define MARI_AGENT_CAPABILITIES_HPP

#include <mari/agent/json.hpp>
#include <mari/agent/view.hpp>
#include <mari/core/result.hpp>

#include <string_view>
#include <vector>

namespace mari::agent {

class AgentSession;

/// 연산 하나의 파라미터 한 칸.
struct ParamSpec {
    const char* name = "";
    /// "string" | "int" | "number" | "bool" | "array" | "object" | "layer" | "region" | "color"
    /// — `layer`/`region`/`color` 는 시맨틱 주소·색 표기를 뜻하는 별도 타입이다.
    const char* type = "string";
    bool required = false;
    const char* desc = "";
};

/// 연산 실행 함수. 결과 **본문**만 돌려준다 — 봉투(ok/op/image)는 디스패치가 씌운다.
/// 바뀐 영역은 `AgentSession::noteDirty()` 로 알린다.
using OpFn = Result<Json> (*)(AgentSession&, const Json&);

/// 연산 하나.
struct OpSpec {
    const char* name = "";
    /// docs/05 4절 표의 영역: doc | snapshot | layer | draw | select | brush | visual | batch | event
    const char* group = "";
    const char* summary = "";
    /// 이 빌드에서 실제로 되는가. **되는 척하지 않는다**(docs/04 6절).
    bool supported = true;
    /// supported 가 false 일 때의 이유. 사용자에게 그대로 보여 준다.
    const char* unsupportedReason = "";
    /// 캔버스를 바꾸는가. batch 원자성과 저장 상태가 이걸 본다.
    bool mutates = false;
    /// view 를 주지 않았을 때의 기본 모드.
    ViewMode defaultView = ViewMode::Dirty;
    std::vector<ParamSpec> params;
    OpFn fn = nullptr;
};

/// 연산 표 전체. 한 번 만들어 두고 계속 쓴다.
[[nodiscard]] const std::vector<OpSpec>& opTable();
/// 이름으로 찾는다. 없으면 nullptr.
[[nodiscard]] const OpSpec* findOp(std::string_view name);

/// 모든 연산이 공통으로 받는 파라미터. **전 연산이 시각 피드백을 받는다**(docs/05 2.1).
inline constexpr const char* kUniversalViewParam = "view";
/// 요청에서 연산 이름을 담는 키.
inline constexpr const char* kOpKey = "op";

/// 스키마 이름과 판. 어댑터(MCP 등)가 호환성을 판단하는 값이다.
inline constexpr const char* kApiSchema = "mari-agent-api/1";
inline constexpr const char* kApiVersion = "0.1.0";

/// 런타임 한계. 하드코딩된 문서 대신 이걸 읽게 한다.
struct ApiLimits {
    i64 maxCanvas = 16384;
    i64 maxBatchOps = 1000;
    i64 maxStrokePoints = 100000;
    i64 maxImageSide = kMaxViewSide;
    i64 maxSnapshots = 256;
    i64 tileSize = kTileSize;
    i64 agentIdMaxLen = 31;
};

/// `capabilities` 응답 본문을 만든다. 세션이 설치된 브러시 목록을 채워 준다.
[[nodiscard]] Json buildCapabilities(const AgentSession& session);

} // namespace mari::agent

#endif // MARI_AGENT_CAPABILITIES_HPP
