// Mari Paint — 에이전트 스트로크 진입점 (docs/05 3.1 · A0)
//
// 🔴 이 파일의 유일한 약속:
//    **에이전트 경로로 들어온 획은 무조건 `StrokeOrigin::Agent` 다.**
//    게이트는 origin 을 **인자로 받지 않는다.** 고를 수 없으니 거짓말할 수도 없다.
//    (docs/05 3.1 — "Mari 는 origin 을 위조할 수단을 제공하지 않는다.")
//
// 앞으로 A1(agent-api 코어)·A3(MCP 어댑터)·A2(헤드리스 CLI)가 붙더라도
// **스트로크를 만드는 길은 이 게이트 하나여야 한다.** 다른 길을 뚫으면 그게 뒷문이다.
#ifndef MARI_AGENT_STROKE_GATE_HPP
#define MARI_AGENT_STROKE_GATE_HPP

#include <mari/core/origin.hpp>

#include <string_view>

namespace mari::agent {

/// 에이전트 하나가 그리는 동안 들고 있는 게이트.
///
/// 만드는 법은 `open(agentId)` 하나뿐이고, 만들어진 게이트가 내놓는 출처는
/// **항상** `Agent` 다. 사람 출처를 내놓는 경로는 이 클래스에 없다.
class AgentStrokeGate {
public:
    /// 에이전트 식별자로 게이트를 연다. 식별자가 없거나 형식이 틀리면 열리지 않는다
    /// — **익명 에이전트 획은 만들지 않는다.**
    [[nodiscard]] static Result<AgentStrokeGate> open(std::string_view agentId) {
        auto id = AgentId::make(agentId);
        if (!id.ok()) {
            return id.error();
        }
        return Ok(AgentStrokeGate(std::move(id).value()));
    }

    /// 이미 검증된 식별자로 연다.
    [[nodiscard]] static Result<AgentStrokeGate> open(const AgentId& agentId) {
        if (agentId.empty()) {
            return Err("agentId 가 비었다 — 익명 에이전트 획은 만들지 않는다",
                       ErrorCode::InvalidArgument);
        }
        return Ok(AgentStrokeGate(agentId));
    }

    /// 🔴 이 게이트가 만드는 출처. **인자가 없다.** origin 은 여기서 박힌다.
    [[nodiscard]] StrokeSource source() const noexcept {
        return StrokeSource(StrokeOrigin::Agent, agent_);
    }

    [[nodiscard]] const AgentId& agentId() const noexcept { return agent_; }

private:
    explicit AgentStrokeGate(AgentId id) noexcept : agent_(id) {}

    AgentId agent_;
};

} // namespace mari::agent

#endif // MARI_AGENT_STROKE_GATE_HPP
