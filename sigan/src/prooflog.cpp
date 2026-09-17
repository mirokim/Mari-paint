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
            // 출처는 획 시작 프레임의 것을 쓴다. 한 획 안에서 바뀔 수 있는 값이 아니다.
            cur.origin = f.origin;
            cur.agentId = f.agentId;
            // 🔴 붓질인가 영역 연산인가도 프레임에서 읽는다(docs/06 결정 ①).
            cur.synthetic = hasFlag(f.flags, FrameFlag::Synthetic);
            cur.x0 = f.cx;
            cur.y0 = f.cy;
            open = true;
        }
        cur.seqLast = f.seq;
        cur.tEndMs = f.tMs;
        cur.x1 = f.cx;
        cur.y1 = f.cy;
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

StrokeOriginStats originStats(const std::vector<ProofStrokeSummary>& strokes) {
    StrokeOriginStats st;
    for (const auto& s : strokes) {
        st.add(s.origin); // 모르는 값도 버리지 않는다 — "미지정" 칸으로 간다
    }
    return st;
}

StrokeOriginStats brushStrokeStats(const std::vector<ProofStrokeSummary>& strokes) {
    StrokeOriginStats st;
    for (const auto& s : strokes) {
        if (!s.synthetic) {
            st.add(s.origin);
        }
    }
    return st;
}

StrokeOriginStats regionOpStats(const std::vector<ProofStrokeSummary>& strokes) {
    StrokeOriginStats st;
    for (const auto& s : strokes) {
        if (s.synthetic) {
            st.add(s.origin);
        }
    }
    return st;
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

    // ── 출처별 집계 (docs/05 3.2) ──────────────────────────────────────
    // 🔴 숫자만 싣는다. "human-only" · "ai-assisted" 같은 판정 문자열은 없다.
    //    의뢰인이 원하는 건 "AI 안 씀"이 아니라 "거짓말 안 함"이다. 사실만 적는다.
    // 🔴 세 축을 **따로** 싣는다(docs/06 결정 ② · 6절 H3).
    //    획 수만 실으면 캔버스 전체를 칠한 fill 이 "AI 0.05%" 가 되고,
    //    면적만 실으면 배경 그라데이션 하나가 사람의 선화 1,847획을 지운다.
    //    둘 다 참이고 둘 다 불완전하므로 둘 다 싣고, **하나로 합치지 않는다** —
    //    가중치를 고르는 순간 그건 판정이고, 판정은 Sigan 의 몫이다(docs/03 2절).
    const StrokeOriginStats origins = brushStrokeStats(strokes); // 축 A
    const StrokeOriginStats regions = regionOpStats(strokes);    // 축 B
    const StrokeOrigin kAll[] = {StrokeOrigin::HumanPen, StrokeOrigin::HumanMouse,
                                 StrokeOrigin::Agent, StrokeOrigin::Imported,
                                 StrokeOrigin::Filter};
    const auto countBlock = [&](const StrokeOriginStats& st) {
        std::string b = "{";
        for (usize i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
            b += i == 0 ? "\"" : ", \"";
            b += strokeOriginName(kAll[i]);
            b += "\": " + std::to_string(st.count(kAll[i]));
        }
        // 출처가 기록되지 않은 획. **0 이어야 정상이다.** 0 이 아니면 숨기지 않고 드러낸다.
        b += ", \"unspecified\": " + std::to_string(st.unspecified());
        b += "}";
        return b;
    };
    // 축 A — 붓질. 키 이름은 그대로 두되 뜻이 좁아졌다: **Synthetic 이 아닌 획만** 센다.
    out += ",\n  \"originCounts\": " + countBlock(origins);
    out += ",\n  \"humanStrokes\": " + std::to_string(origins.human());
    out += ",\n  \"agentStrokes\": " + std::to_string(origins.agent());
    out += ",\n  \"agentStrokeRatio\": " + num3(origins.agentRatio());
    // 축 B — 영역 연산(fill·erase·gradient 따위). 붓질과 **같은 칸에 세지 않는다.**
    out += ",\n  \"regionOpCounts\": " + countBlock(regions);
    out += ",\n  \"humanRegionOps\": " + std::to_string(regions.human());
    out += ",\n  \"agentRegionOps\": " + std::to_string(regions.agent());
    // 축 C — 변경된 타일 수. 픽셀이 아니라 타일이다(정수라 가장자리로 부풀릴 수 없다).
    out += ",\n  \"changedTiles\": {";
    for (usize i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
        const usize slot = static_cast<usize>(static_cast<u8>(kAll[i]));
        const u64 v = in.changedTilesByOrigin == nullptr ? 0ull : in.changedTilesByOrigin[slot];
        out += i == 0 ? "\"" : ", \"";
        out += strokeOriginName(kAll[i]);
        out += "\": " + std::to_string(v);
    }
    out += ", \"unspecified\": ";
    out += std::to_string(in.changedTilesByOrigin == nullptr ? 0ull : in.changedTilesByOrigin[0]);
    out += ", \"measured\": ";
    // 🔴 안 잰 것을 0 으로 적어 놓고 잰 척하지 않는다.
    out += in.changedTilesByOrigin != nullptr ? "true" : "false";
    out += "}";
    // 붓을 쥔 에이전트 목록. 와이어에는 다이제스트만 실리므로 이름을 여기서 붙여 준다.
    out += ",\n  \"agents\": [";
    for (usize i = 0; i < in.agents.size(); ++i) {
        out += i == 0 ? "{\"digest\": " : ", {\"digest\": ";
        out += std::to_string(in.agents[i].digest());
        out += ", \"id\": ";
        appendEscaped(out, std::string(in.agents[i].view()));
        out += "}";
    }
    out += "]";
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
        // 🔴 붓질이 아니었다면 그렇게 적는다. 이 값은 프레임 플래그에서 왔다.
        out += ", \"synthetic\": ";
        out += s.synthetic ? "true" : "false";
        if (s.synthetic) {
            // 합성 프레임 쌍의 두 점이 정한 영향 영역. **프레임 바이트 안**에서 나온 사실이다.
            const f64 lx = s.x0 < s.x1 ? s.x0 : s.x1;
            const f64 ly = s.y0 < s.y1 ? s.y0 : s.y1;
            const f64 hx = s.x0 < s.x1 ? s.x1 : s.x0;
            const f64 hy = s.y0 < s.y1 ? s.y1 : s.y0;
            out += ", \"area\": {\"x\": " + num3(lx) + ", \"y\": " + num3(ly) +
                   ", \"w\": " + num3(hx - lx + 1.0) + ", \"h\": " + num3(hy - ly + 1.0) + "}";
        }
        // 🔴 획마다 출처가 붙는다. 이 값은 저널(=서명 정본이 될 프레임)에서 왔다.
        out += ", \"origin\": \"";
        out += strokeOriginName(s.origin);
        out += "\"";
        out += ", \"agentId\": " + std::to_string(s.agentId);
        out += "}";
    }
    out += strokes.empty() ? "]" : "\n  ]";
    out += "\n}\n";
    return Ok(std::move(out));
}

} // namespace mari::sigan
