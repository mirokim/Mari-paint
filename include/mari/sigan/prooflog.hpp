// Mari Paint — 로컬 무서명 과정 로그 (docs/03 6절)
//
// 🔴 Sigan 이 없을 때도 기록은 남긴다. 단, 등급은 **`unsigned` 하나뿐이다.**
//    native / deferred / rawinput 을 Mari 가 자칭하면 설계 위반이다(docs/03 2절·6절).
//    "인증서 아님"을 로그 안에 문자열로 박아 둔다.
#ifndef MARI_SIGAN_PROOFLOG_HPP
#define MARI_SIGAN_PROOFLOG_HPP

#include <mari/core/result.hpp>
#include <mari/sigan/journal.hpp>

#include <string>

namespace mari::sigan {

/// Mari 가 붙일 수 있는 유일한 등급. 다른 값은 존재하지 않는다.
inline constexpr const char* kGradeUnsigned = "unsigned";

/// .ora 에 넣을 prooflog.json 을 만들 때 쓰는 입력.
struct ProofLogInput {
    std::string app = "mari-paint/0.1.0";
    u64 sessionId = 0;
    i64 wallAnchorUnixMs = 0;
    const JournalScan* scan = nullptr; ///< 필수
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
};

/// 저널 스캔에서 획 단위 요약을 뽑는다.
std::vector<ProofStrokeSummary> summarizeStrokes(const JournalScan& scan);

/// prooflog.json 문자열을 만든다. 서명하지 않는다 — 해시체인도 만들지 않는다.
Result<std::string> buildProofLog(const ProofLogInput& in);

} // namespace mari::sigan

#endif // MARI_SIGAN_PROOFLOG_HPP
