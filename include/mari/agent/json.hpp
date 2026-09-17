// Mari Paint — 최소 JSON (에이전트 API 의 입출력 표현)
//
// 🔴 왜 직접 만드나
//    docs/05 1절: "MCP 는 전송 규약일 뿐이고 몇 년 뒤 다른 게 표준이 될 수도 있다."
//    그래서 능력 계층은 **어떤 외부 JSON 라이브러리에도 묶이지 않는다.** 리포에는
//    네트워크도 서드파티 의존도 없다(docs/04). 필요한 만큼만 여기에 둔다.
//
// 성격:
//   · 키 순서를 **입력 순서 그대로 유지한다.** 응답이 매번 같은 모양이어야 AI 가
//     읽기 쉽고, 테스트가 골든 문자열로 비교할 수 있다.
//   · 정수와 실수를 구분해서 찍는다(1 을 1.0 으로 만들지 않는다 — 좌표가 정수다).
//   · 파싱 실패는 **예외가 아니라** Result<Json> 로 돌려준다(API 경계 밖으로 예외를
//     흘리지 않는다는 docs/05 구현 규칙).
//   · 깊이 제한이 있다. 악의적인 중첩으로 스택을 깨뜨릴 수 없다.
#ifndef MARI_AGENT_JSON_HPP
#define MARI_AGENT_JSON_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <initializer_list>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>

namespace mari::agent {

/// JSON 값 하나. 복사 가능한 값 타입이다(트리 통째로 복사된다).
class Json {
public:
    enum class Kind : u8 { Null = 0, Bool, Number, String, Array, Object };

    /// 중첩 깊이 상한. 파싱·직렬화 양쪽에 건다.
    static constexpr int kMaxDepth = 64;

    Json() = default;

    // ── 만들기 ───────────────────────────────────────────────────────────
    [[nodiscard]] static Json null() { return Json{}; }
    [[nodiscard]] static Json boolean(bool v);
    /// 정수로 찍힌다(소수점이 붙지 않는다).
    /// 정수 타입이면 무엇이든 받는다 — 호출부마다 캐스팅을 달게 하면 그게 버그의 씨앗이다.
    /// (i64 · u64 두 개를 겹쳐 두면 `int` 인자에서 오버로드가 모호해진다.)
    template <typename T, typename = std::enable_if_t<std::is_integral_v<T> &&
                                                      !std::is_same_v<std::decay_t<T>, bool>>>
    [[nodiscard]] static Json integer(T v) {
        return integerImpl(static_cast<i64>(v));
    }
    /// 실수로 찍힌다. 유효자릿수는 dump 가 정한다.
    [[nodiscard]] static Json number(f64 v);
    [[nodiscard]] static Json string(std::string v);
    [[nodiscard]] static Json array();
    [[nodiscard]] static Json object();

    // ── 보기 ─────────────────────────────────────────────────────────────
    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool isNull() const noexcept { return kind_ == Kind::Null; }
    [[nodiscard]] bool isBool() const noexcept { return kind_ == Kind::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return kind_ == Kind::Number; }
    [[nodiscard]] bool isString() const noexcept { return kind_ == Kind::String; }
    [[nodiscard]] bool isArray() const noexcept { return kind_ == Kind::Array; }
    [[nodiscard]] bool isObject() const noexcept { return kind_ == Kind::Object; }
    /// 정수로 들어온 값인가. **찍는 방식에만 영향을 준다** —
    /// 동등 비교는 값으로만 한다(1 과 1.0 은 같은 값이다).
    [[nodiscard]] bool isIntegral() const noexcept { return kind_ == Kind::Number && integral_; }

    [[nodiscard]] bool asBool(bool fallback = false) const noexcept;
    [[nodiscard]] f64 asNumber(f64 fallback = 0.0) const noexcept;
    [[nodiscard]] i64 asInt(i64 fallback = 0) const noexcept;
    [[nodiscard]] const std::string& asString() const noexcept;

    /// 배열 원소 수 / 객체 멤버 수. 그 외에는 0.
    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// 배열 원소. 범위를 벗어나면 null 을 돌려준다(던지지 않는다).
    [[nodiscard]] const Json& at(usize i) const noexcept;
    /// 객체 멤버. 없으면 null 을 돌려준다.
    [[nodiscard]] const Json& operator[](std::string_view key) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept;
    /// 객체의 키들(입력 순서).
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return keys_; }

    // ── 쌓기 ─────────────────────────────────────────────────────────────
    /// 배열에 덧붙인다. 배열이 아니면 배열로 바꾼다.
    void push(Json v);
    /// 객체 멤버를 넣는다(같은 키가 있으면 덮어쓴다. 자리는 유지된다).
    /// 객체가 아니면 객체로 바꾼다.
    void set(std::string key, Json v);

    // ── 직렬화 ───────────────────────────────────────────────────────────
    /// 한 줄. 사람이 안 읽는 경로(전송)용.
    [[nodiscard]] std::string dump() const;
    /// 들여쓰기. indent <= 0 이면 한 줄과 같다.
    [[nodiscard]] std::string dump(int indent) const;

    /// 파싱. 뒤에 쓰레기가 붙어 있으면 실패다(조용히 넘기지 않는다).
    [[nodiscard]] static Result<Json> parse(std::string_view text);

    friend bool operator==(const Json& a, const Json& b);

private:
    [[nodiscard]] static Json integerImpl(i64 v);
    void appendTo(std::string& out, int indent, int depth) const;

    Kind kind_ = Kind::Null;
    bool bool_ = false;
    bool integral_ = false;
    f64 num_ = 0.0;
    i64 int_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    // 객체는 키/값 두 벡터로 든다 — 순서를 유지하면서 불완전 타입 문제를 피한다.
    std::vector<std::string> keys_;
    std::vector<Json> vals_;
};

/// 객체를 한 줄로 만드는 도우미. `jsonObject({{"ok", Json::boolean(true)}})`
[[nodiscard]] Json jsonObject(std::initializer_list<std::pair<const char*, Json>> members);
/// 배열을 한 줄로.
[[nodiscard]] Json jsonArray(std::initializer_list<Json> items);
/// 정수 4개짜리 사각형 `[x, y, w, h]`. 에이전트 API 의 영역 표기 정본이다.
[[nodiscard]] Json jsonRect(const Rect& r);
/// `[x, y, w, h]` 를 Rect 로. 형식이 아니면 실패.
[[nodiscard]] Result<Rect> rectFromJson(const Json& v);

} // namespace mari::agent

#endif // MARI_AGENT_JSON_HPP
