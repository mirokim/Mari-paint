// Mari Paint — 획의 출처 StrokeOrigin (docs/05 3절 · docs/03 4.1)
//
// 🔴 이 헤더가 존재하는 이유:
//    Mari 는 AI 에게 붓을 쥐여준다. AI 획을 사람 획과 **구별 없이** Sigan 에 넘기면
//    Mari Paint 는 Sigan 인증서 위조기가 된다. 부작용이 아니라 설계 결함이다.
//    그래서 모든 획은 출처를 달고 다니고, 그 출처는 **서명 정본 안**으로 들어간다.
//
// 🔴 경계선(docs/03 2절): 여기에는 **판정이 없다.** 숫자와 사실만 있다.
//    "사람만 그림" · "AI 보조" 같은 **등급 문자열을 이 파일에 만들지 마라.**
//    비율까지가 Mari 의 몫이고, 그 위의 해석은 Sigan 의 몫이다(docs/05 3.3 표).
//    tests/agent/test_origin.cpp 가 이 파일에 등급 문자열이 없는 것을 검사한다.
//
// 🔴 위조 방지 규약 — **감사 지점은 이 파일 하나다.**
//    1. origin 을 인자로 받거나 돌려 세팅하는 **공개 생성자·세터가 없다.**
//    2. `StrokeSource` 는 기본 생성자가 없다 — 출처를 빠뜨리면 **컴파일이 깨진다.**
//       기본값이 있으면 실수로 AI 획이 사람 획이 된다. 그래서 기본값을 두지 않는다.
//    3. `StrokeOrigin::Agent` 를 만들 수 있는 것은 `mari::agent::AgentStrokeGate`
//       하나뿐이다(private 생성자 + friend). 에이전트 진입점은 origin 을 **인자로
//       받지 않는다** — 안에서 박는다.
#ifndef MARI_CORE_ORIGIN_HPP
#define MARI_CORE_ORIGIN_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <string_view>

namespace mari::agent {
/// 에이전트 스트로크 진입점. 정의는 include/mari/agent/stroke_gate.hpp.
class AgentStrokeGate;
} // namespace mari::agent

namespace mari {

/// 획 하나의 출처. **값은 와이어에 직렬화된다 — 뒤에만 더해라.**
///
/// ⚠️ 0 을 비워 둔 것은 의도다. 값 초기화(`{}`)로 만들어진 프레임이
///    **사람 획으로 보이면 안 되기 때문이다.** 0 은 "미지정"이고, 그건 사람이 아니다.
enum class StrokeOrigin : u8 {
    HumanPen = 1,   ///< 필압 있는 실제 펜
    HumanMouse = 2, ///< 마우스
    Agent = 3,      ///< AI/스크립트 — agentId 동반
    Imported = 4,   ///< 붙여넣기·외부 반입
    Filter = 5,     ///< 필터·플러그인이 생성
};

/// 와이어에서 "출처 미지정"을 뜻하는 값. 정상 기록에는 나오면 안 된다.
inline constexpr u8 kOriginUnspecified = 0;
/// 집계 배열 칸 수(0 = 미지정 포함).
inline constexpr usize kStrokeOriginSlots = 6;

/// 알려진 출처인가. 미지정(0)이나 모르는 값이면 false.
[[nodiscard]] constexpr bool isKnownOrigin(StrokeOrigin o) noexcept {
    const u8 v = static_cast<u8>(o);
    return v >= static_cast<u8>(StrokeOrigin::HumanPen) &&
           v <= static_cast<u8>(StrokeOrigin::Filter);
}

/// 직렬화·로그용 안정 문자열. 모르는 값은 "unspecified".
/// **등급이 아니다.** 사실을 가리키는 이름일 뿐이다.
[[nodiscard]] constexpr const char* strokeOriginName(StrokeOrigin o) noexcept {
    switch (o) {
    case StrokeOrigin::HumanPen:
        return "humanPen";
    case StrokeOrigin::HumanMouse:
        return "humanMouse";
    case StrokeOrigin::Agent:
        return "agent";
    case StrokeOrigin::Imported:
        return "imported";
    case StrokeOrigin::Filter:
        return "filter";
    }
    return "unspecified";
}

/// 사람 손이 직접 만든 획인가(펜 또는 마우스).
/// 사실 분류다 — 등급이 아니다. "AI 안 씀"을 뜻하지 않는다.
[[nodiscard]] constexpr bool isHumanOrigin(StrokeOrigin o) noexcept {
    return o == StrokeOrigin::HumanPen || o == StrokeOrigin::HumanMouse;
}

// ── agentId ──────────────────────────────────────────────────────────────

/// FNV-1a 32비트. agentId 문자열을 와이어 폭(u32)으로 접는 데 쓴다.
/// **해시체인도 서명도 아니다**(docs/03 2절) — 대조용 다이제스트일 뿐이다.
[[nodiscard]] constexpr u32 fnv1a32(const char* s, usize n) noexcept {
    u32 h = 2166136261u;
    for (usize i = 0; i < n; ++i) {
        h ^= static_cast<u32>(static_cast<u8>(s[i]));
        h *= 16777619u;
    }
    return h;
}

/// 에이전트 식별자. 짧은 문자열 + 와이어용 32비트 다이제스트.
///
/// 고정 배열로 담는다 — `StrokeSource` 가 핫 패스(프레임 발행)를 타므로
/// 복사가 **할당을 하면 안 되기 때문이다.** std::string 을 쓰지 않는 이유가 이것이다.
class AgentId {
public:
    /// 이름 최대 길이. 와이어에는 다이제스트만 나가므로 길 이유가 없다.
    static constexpr usize kMaxLen = 31;

    /// 빈 식별자 = 에이전트가 아니다. `Agent` 출처는 빈 식별자를 허용하지 않는다.
    constexpr AgentId() noexcept = default;

    /// 이름을 검증해서 만든다. 1..31자, `[A-Za-z0-9._:+/-]` 만 허용한다.
    /// 좁게 잡은 이유: 이 문자열이 prooflog.json 에 그대로 실린다. 이스케이프로
    /// 빠져나갈 여지를 아예 없앤다.
    [[nodiscard]] static Result<AgentId> make(std::string_view name) {
        if (name.empty()) {
            return Err("agentId 가 비었다 — 에이전트 획은 식별자를 반드시 동반한다",
                       ErrorCode::InvalidArgument);
        }
        if (name.size() > kMaxLen) {
            return Err("agentId 가 너무 길다(최대 31자)", ErrorCode::InvalidArgument);
        }
        for (const char c : name) {
            const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
                                c == '+' || c == '/' || c == '-';
            if (!okChar) {
                return Err("agentId 에 쓸 수 없는 문자가 있다: [A-Za-z0-9._:+/-] 만 된다",
                           ErrorCode::InvalidArgument);
            }
        }
        AgentId id;
        for (usize i = 0; i < name.size(); ++i) {
            id.buf_[i] = name[i];
        }
        id.len_ = static_cast<u8>(name.size());
        id.digest_ = fnv1a32(id.buf_, name.size());
        return Ok(id);
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept { return buf_; }
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(buf_, len_);
    }
    [[nodiscard]] constexpr usize size() const noexcept { return len_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return len_ == 0; }
    /// 와이어에 실리는 32비트 다이제스트. 빈 식별자는 0 이다.
    [[nodiscard]] constexpr u32 digest() const noexcept { return digest_; }

    friend constexpr bool operator==(const AgentId& a, const AgentId& b) noexcept {
        return a.len_ == b.len_ && a.digest_ == b.digest_ && a.view() == b.view();
    }

private:
    char buf_[kMaxLen + 1]{};
    u8 len_ = 0;
    u32 digest_ = 0;
};

// ── StrokeSource ─────────────────────────────────────────────────────────

/// 획 하나의 출처 토큰. **이 타입이 위조 방지의 핵심이다.**
///
/// · 기본 생성자가 없다 → 출처를 빠뜨린 코드는 컴파일되지 않는다.
/// · origin 을 받는 공개 생성자·세터가 없다 → 바깥에서 값을 고를 수 없다.
/// · 만드는 길은 아래 팩토리 넷뿐이고, 각각이 자기 origin 을 **박아 넣는다.**
/// · `Agent` 팩토리는 여기 **없다.** mari::agent::AgentStrokeGate 만이 만든다.
///
/// 복사는 값 복사이고 할당을 하지 않는다(핫 패스 안전).
class StrokeSource {
public:
    /// 🔴 기본값 금지. "그냥 두면 사람 획"이 되는 실수를 컴파일 타임에 막는다.
    StrokeSource() = delete;

    [[nodiscard]] static constexpr StrokeSource humanPen() noexcept {
        return StrokeSource(StrokeOrigin::HumanPen, AgentId{});
    }
    [[nodiscard]] static constexpr StrokeSource humanMouse() noexcept {
        return StrokeSource(StrokeOrigin::HumanMouse, AgentId{});
    }
    /// 붙여넣기·외부 반입. 사람이 그린 게 아니다.
    [[nodiscard]] static constexpr StrokeSource imported() noexcept {
        return StrokeSource(StrokeOrigin::Imported, AgentId{});
    }
    /// 필터·플러그인이 만든 획.
    [[nodiscard]] static constexpr StrokeSource filter() noexcept {
        return StrokeSource(StrokeOrigin::Filter, AgentId{});
    }

    [[nodiscard]] constexpr StrokeOrigin origin() const noexcept { return origin_; }
    [[nodiscard]] constexpr const AgentId& agent() const noexcept { return agent_; }
    /// 와이어에 실리는 에이전트 다이제스트. 에이전트가 아니면 0.
    [[nodiscard]] constexpr u32 agentDigest() const noexcept { return agent_.digest(); }
    [[nodiscard]] constexpr bool isAgent() const noexcept {
        return origin_ == StrokeOrigin::Agent;
    }

private:
    /// 🔴 유일한 생성자. private 이고, Agent 를 만들 수 있는 friend 는 하나뿐이다.
    constexpr StrokeSource(StrokeOrigin o, AgentId a) noexcept : origin_(o), agent_(a) {}

    friend class agent::AgentStrokeGate;

    StrokeOrigin origin_;
    AgentId agent_;
};

// ── 집계 ─────────────────────────────────────────────────────────────────

/// 출처별 획 수. docs/05 3.2 의 "사람 획 1,847 / AI 획 213 (10.3%)" 표시용 **원자료**다.
///
/// 🔴 비율까지만 준다. 등급 문자열(docs/05 3.3 표의 세 등급)을 여기서 만들지 마라 —
///    판정은 Sigan 의 몫이다(docs/03 2절).
class StrokeOriginStats {
public:
    /// 출처 o 인 획을 n 개 더한다. 모르는 값은 "미지정" 칸으로 간다 — 버리지 않는다.
    constexpr void add(StrokeOrigin o, u64 n = 1) noexcept { counts_[slot(o)] += n; }

    [[nodiscard]] constexpr u64 count(StrokeOrigin o) const noexcept { return counts_[slot(o)]; }
    /// 출처가 기록되지 않은 획. **0 이어야 정상이다.** 숨기지 않고 센다.
    [[nodiscard]] constexpr u64 unspecified() const noexcept { return counts_[0]; }
    [[nodiscard]] constexpr u64 total() const noexcept {
        u64 t = 0;
        for (usize i = 0; i < kStrokeOriginSlots; ++i) {
            t += counts_[i];
        }
        return t;
    }
    /// 사람 손이 직접 만든 획(펜 + 마우스).
    [[nodiscard]] constexpr u64 human() const noexcept {
        return count(StrokeOrigin::HumanPen) + count(StrokeOrigin::HumanMouse);
    }
    /// AI/스크립트가 만든 획.
    [[nodiscard]] constexpr u64 agent() const noexcept { return count(StrokeOrigin::Agent); }
    /// 전체 대비 AI 획 비율 0..1. 전체가 0 이면 0. **판정이 아니라 나눗셈이다.**
    [[nodiscard]] constexpr f64 agentRatio() const noexcept {
        const u64 t = total();
        return t == 0 ? 0.0 : static_cast<f64>(agent()) / static_cast<f64>(t);
    }

    friend constexpr bool operator==(const StrokeOriginStats&,
                                     const StrokeOriginStats&) = default;

private:
    [[nodiscard]] static constexpr usize slot(StrokeOrigin o) noexcept {
        const usize v = static_cast<usize>(static_cast<u8>(o));
        return v < kStrokeOriginSlots ? v : 0;
    }

    u64 counts_[kStrokeOriginSlots]{};
};

} // namespace mari

#endif // MARI_CORE_ORIGIN_HPP
