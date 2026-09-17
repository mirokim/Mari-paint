// Mari Paint — 로컬 무서명 과정 로그 (docs/03 6절)
//
// 🔴 여기에는 서명도, 해시체인도, 등급 판정도 없다.
//    Mari 가 붙일 수 있는 등급은 "unsigned" 하나뿐이다.
#include <mari/sigan/prooflog.hpp>

#include <cstdio>

namespace mari::sigan {
namespace {

/// f64 를 소수점 3자리 고정으로. 로케일 영향을 받지 않게 직접 만든다.
std::string num3(f64 v) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "%.3f", v);
    if (n <= 0) {
        return "0.000";
    }
    return std::string(buf, static_cast<usize>(n));
}

void appendEscaped(std::string& out, const std::string& v) {
    out.push_back('"');
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
}

} // namespace

std::vector<ProofStrokeSummary> summarizeStrokes(const JournalScan& scan) {
    std::vector<ProofStrokeSummary> out;
    ProofStrokeSummary cur;
    bool open = false;
    for (const auto& f : scan.frames) {
        if (!open || hasFlag(f.flags, FrameFlag::Down)) {
            if (open) {
                out.push_back(cur); // up 없이 끊긴 획도 숨기지 않는다
            }
            cur = ProofStrokeSummary{};
            cur.seqFirst = f.seq;
            cur.tStartMs = f.tMs;
            cur.layerId = f.layerId;
            cur.brushId = f.brushId;
            cur.eraser = hasFlag(f.flags, FrameFlag::Eraser);
            open = true;
        }
        cur.seqLast = f.seq;
        cur.tEndMs = f.tMs;
        ++cur.pointCount;
        if (hasFlag(f.flags, FrameFlag::Up)) {
            out.push_back(cur);
            open = false;
        }
    }
    if (open) {
        out.push_back(cur);
    }
    return out;
}

Result<std::string> buildProofLog(const ProofLogInput& in) {
    if (in.scan == nullptr) {
        return Err("prooflog 를 만들 저널 스캔이 없다", ErrorCode::InvalidArgument);
    }
    const JournalScan& scan = *in.scan;
    const auto strokes = summarizeStrokes(scan);
    const auto gaps = scan.seqGaps();

    std::string out;
    out.reserve(256 + strokes.size() * 128);
    out += "{\n  \"schema\": \"mari-prooflog/1\",\n";
    // 🔴 등급은 이것뿐이다. native/deferred 를 Mari 가 자칭하면 설계 위반이다.
    out += "  \"grade\": \"";
    out += kGradeUnsigned;
    out += "\",\n";
    out += "  \"notice\": \"이 로그는 인증서가 아니다. Mari 는 증명하지 않는다 — "
           "기록만 남긴다. 봉인·서명은 Sigan 이 한다.\",\n";
    out += "  \"signed\": false,\n";
    out += "  \"app\": ";
    appendEscaped(out, in.app);
    out += ",\n  \"sessionId\": " + std::to_string(in.sessionId);
    out += ",\n  \"wallClockAnchorUnixMs\": " + std::to_string(in.wallAnchorUnixMs);
    out += ",\n  \"clock\": \"steady-relative-ms\"";
    out += ",\n  \"frameCount\": " + std::to_string(scan.frames.size());
    out += ",\n  \"seqFirst\": " +
           std::to_string(scan.frames.empty() ? 0ull : scan.frames.front().seq);
    out += ",\n  \"seqLast\": " +
           std::to_string(scan.frames.empty() ? 0ull : scan.frames.back().seq);
    // seq 구멍은 정직하게 드러낸다(docs/03 4.2). 비어 있어야 정상이다.
    out += ",\n  \"lostFrames\": " + std::to_string(gaps.size());
    out += ",\n  \"journalTruncated\": ";
    out += scan.truncated ? "true" : "false";
    out += ",\n  \"strokes\": [";
    for (usize i = 0; i < strokes.size(); ++i) {
        const auto& s = strokes[i];
        out += i == 0 ? "\n    " : ",\n    ";
        out += "{\"seqFirst\": " + std::to_string(s.seqFirst);
        out += ", \"seqLast\": " + std::to_string(s.seqLast);
        out += ", \"tStartMs\": " + num3(s.tStartMs);
        out += ", \"tEndMs\": " + num3(s.tEndMs);
        out += ", \"layerId\": " + std::to_string(s.layerId);
        out += ", \"brushId\": " + std::to_string(s.brushId);
        out += ", \"points\": " + std::to_string(s.pointCount);
        out += ", \"eraser\": ";
        out += s.eraser ? "true" : "false";
        out += "}";
    }
    out += strokes.empty() ? "]" : "\n  ]";
    out += "\n}\n";
    return Ok(std::move(out));
}

} // namespace mari::sigan
