// Mari Paint — 로컬 무서명 과정 로그 (docs/03 6절)
//
// 🔴 Sigan 이 없을 때도 기록은 남긴다. 단, 등급은 **`unsigned` 하나뿐이다.**
//    native / deferred / rawinput 을 Mari 가 자칭하면 설계 위반이다(docs/03 2절·6절).
//    "인증서 아님"을 로그 안에 문자열로 박아 둔다.
#ifndef MARI_SIGAN_PROOFLOG_HPP
#define MARI_SIGAN_PROOFLOG_HPP

#include <mari/core/origin.hpp>
#include <mari/core/result.hpp>
#include <mari/sigan/journal.hpp>

#include <string>
#include <vector>

namespace mari::sigan {

/// Mari 가 붙일 수 있는 유일한 등급. 다른 값은 존재하지 않는다.
inline constexpr const char* kGradeUnsigned = "unsigned";

/// .ora 에 넣을 prooflog.json 을 만들 때 쓰는 입력.
struct ProofLogInput {
    std::string app = "mari-paint/0.1.0";
    u64 sessionId = 0;
    i64 wallAnchorUnixMs = 0;
    const JournalScan* scan = nullptr; ///< 필수
    /// 이 세션에서 붓을 쥔 에이전트들. 와이어에는 32비트 다이제스트만 실리므로,
    /// 사람이 읽을 이름은 여기서 받아 로그에 같이 적는다. 비어도 된다.
    std::vector<AgentId> agents;
    /// 출처별 **변경된 타일 수**(docs/06 결정 ② 축 C). `kStrokeOriginSlots` 칸짜리 배열이거나
    /// nullptr. 프레임에는 자리가 없는 값이라 기록한 쪽이 건네준다 —
    /// 이 로그 자체가 서명되지 않은 기록이므로(`grade: unsigned`) 여기 실리는 것이
    /// "검증된 사실"인 척하지 않는다. 붓질 수·영역 연산 수는 프레임에서 **직접 센다.**
    const u64* changedTilesByOrigin = nullptr;
};

/// 획 하나의 요약(로그 안에 들어간다).
struct ProofStrokeSummary {
    u64 seqFirst = 0;
    u64 seqLast = 0;
    f64 tStartMs = 0.0;
    f64 tEndMs = 0.0;
    u32 layerId = 0;
    u32 brushId = 0;
    u64 pointCount = 0;
    bool eraser = false;
    /// 🔴 획의 출처. 저널 프레임에서 그대로 읽는다 — 여기서 추측하지 않는다.
    StrokeOrigin origin = static_cast<StrokeOrigin>(kOriginUnspecified);
    u32 agentId = 0; ///< 에이전트 다이제스트. Agent 가 아니면 0
    /// 🔴 붓질이 아니라 영역 연산인가(`FrameFlag::Synthetic`, docs/06 결정 ①).
    ///    프레임 플래그에서 그대로 읽는다 — 여기서 추측하지 않는다.
    bool synthetic = false;
    /// 첫 프레임과 마지막 프레임의 좌표. 합성 프레임 쌍이면 이 둘이 **영향 영역**을 정한다.
    f32 x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

/// 저널 스캔에서 획 단위 요약을 뽑는다.
std::vector<ProofStrokeSummary> summarizeStrokes(const JournalScan& scan);

/// 획 단위 출처 집계. docs/05 3.2 의 "사람 획 N / AI 획 M" 원자료다.
/// 🔴 숫자만 낸다. human-only / ai-assisted 같은 등급 문자열은 만들지 않는다 —
///    판정은 Sigan 의 몫이다(docs/03 2절).
StrokeOriginStats originStats(const std::vector<ProofStrokeSummary>& strokes);

/// 축 A — **붓질만** 센다(`synthetic == false`). docs/06 결정 ②.
StrokeOriginStats brushStrokeStats(const std::vector<ProofStrokeSummary>& strokes);
/// 축 B — **영역 연산만** 센다(`synthetic == true`). 축 A 와 같은 칸에 섞지 않는다.
StrokeOriginStats regionOpStats(const std::vector<ProofStrokeSummary>& strokes);

/// prooflog.json 문자열을 만든다. 서명하지 않는다 — 해시체인도 만들지 않는다.
Result<std::string> buildProofLog(const ProofLogInput& in);

} // namespace mari::sigan

#endif // MARI_SIGAN_PROOFLOG_HPP
