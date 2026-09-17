// Mari Paint — 최소 JSON 구현. 선언은 include/mari/agent/json.hpp.
#include <mari/agent/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mari::agent {
namespace {

const Json& nullValue() {
    static const Json kNull{};
    return kNull;
}

/// 실수를 로케일 영향 없이 찍는다. 뒤따르는 0 은 깎되 최소 한 자리는 남긴다.
std::string numberToString(f64 v) {
    if (!std::isfinite(v)) {
        return "0"; // JSON 에는 NaN/Inf 가 없다. 거짓말 대신 0 으로 눕힌다.
    }
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "%.10g", v);
    if (n <= 0) {
        return "0";
    }
    return std::string(buf, static_cast<usize>(n));
}

void escapeInto(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<u8>(c) < 0x20u) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(static_cast<u8>(c)));
                out += esc;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

void indentInto(std::string& out, int indent, int depth) {
    if (indent <= 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<usize>(indent) * static_cast<usize>(depth), ' ');
}

/// 재귀 하강 파서. 입력을 소비하며 커서를 옮긴다.
class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Result<Json> parseDocument() {
        skipSpace();
        Result<Json> v = parseValue(0);
        if (!v.ok()) {
            return v;
        }
        skipSpace();
        if (i_ != s_.size()) {
            return Err("JSON 뒤에 남은 문자가 있다(위치 " + std::to_string(i_) + ")",
                       ErrorCode::ParseError);
        }
        return v;
    }

private:
    [[nodiscard]] bool done() const { return i_ >= s_.size(); }
    [[nodiscard]] char peek() const { return s_[i_]; }

    void skipSpace() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    Error unexpected(const char* what) const {
        return Err(std::string("JSON 파싱 실패: ") + what + " (위치 " + std::to_string(i_) + ")",
                   ErrorCode::ParseError);
    }

    Result<Json> parseValue(int depth) {
        if (depth > Json::kMaxDepth) {
            return Err("JSON 중첩이 너무 깊다", ErrorCode::ParseError);
        }
        if (done()) {
            return unexpected("값이 없다");
        }
        switch (peek()) {
        case '{':
            return parseObject(depth);
        case '[':
            return parseArray(depth);
        case '"': {
            Result<std::string> s = parseString();
            if (!s.ok()) {
                return s.error();
            }
            return Json::string(std::move(s).value());
        }
        case 't':
            return literal("true", Json::boolean(true));
        case 'f':
            return literal("false", Json::boolean(false));
        case 'n':
            return literal("null", Json::null());
        default:
            return parseNumber();
        }
    }

    Result<Json> literal(std::string_view word, Json v) {
        if (s_.compare(i_, word.size(), word) != 0) {
            return unexpected("모르는 리터럴");
        }
        i_ += word.size();
        return v;
    }

    Result<Json> parseNumber() {
        const usize start = i_;
        if (!done() && (peek() == '-' || peek() == '+')) {
            ++i_;
        }
        bool digits = false;
        bool real = false;
        while (!done()) {
            const char c = peek();
            if (c >= '0' && c <= '9') {
                digits = true;
                ++i_;
            } else if (c == '.' || c == 'e' || c == 'E') {
                real = true;
                ++i_;
            } else if ((c == '-' || c == '+') && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E')) {
                ++i_;
            } else {
                break;
            }
        }
        if (!digits) {
            return unexpected("숫자가 아니다");
        }
        const std::string text(s_.substr(start, i_ - start));
        if (!real) {
            errno = 0;
            char* end = nullptr;
            const long long ll = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0') {
                return Json::integer(static_cast<i64>(ll));
            }
        }
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return unexpected("숫자를 읽을 수 없다");
        }
        return Json::number(d);
    }

    /// UTF-8 로 한 코드포인트를 덧붙인다.
    static void appendUtf8(std::string& out, u32 cp) {
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    Result<u32> parseHex4() {
        if (i_ + 4 > s_.size()) {
            return unexpected("\\u 뒤가 짧다");
        }
        u32 v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<usize>(k)];
            u32 d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<u32>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<u32>(c - 'A') + 10u;
            } else {
                return unexpected("\\u 에 16진수가 아닌 문자가 있다");
            }
            v = (v << 4) | d;
        }
        i_ += 4;
        return Ok(v);
    }

    Result<std::string> parseString() {
        if (done() || peek() != '"') {
            return unexpected("문자열이 아니다");
        }
        ++i_;
        std::string out;
        while (true) {
            if (done()) {
                return unexpected("문자열이 닫히지 않았다");
            }
            const char c = s_[i_++];
            if (c == '"') {
                return Ok(std::move(out));
            }
            if (c != '\\') {
                if (static_cast<u8>(c) < 0x20u) {
                    return unexpected("문자열 안에 제어 문자가 있다");
                }
                out.push_back(c);
                continue;
            }
            if (done()) {
                return unexpected("이스케이프가 끊겼다");
            }
            const char e = s_[i_++];
            switch (e) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                Result<u32> hi = parseHex4();
                if (!hi.ok()) {
                    return hi.error();
                }
                u32 cp = hi.value();
                if (cp >= 0xD800u && cp <= 0xDBFFu && i_ + 1 < s_.size() && s_[i_] == '\\' &&
                    s_[i_ + 1] == 'u') {
                    const usize save = i_;
                    i_ += 2;
                    Result<u32> lo = parseHex4();
                    if (!lo.ok()) {
                        return lo.error();
                    }
                    if (lo.value() >= 0xDC00u && lo.value() <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo.value() - 0xDC00u);
                    } else {
                        i_ = save; // 짝이 아니면 되돌린다. 홀로 남은 서로게이트는 그대로 둔다.
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return unexpected("모르는 이스케이프");
            }
        }
    }

    Result<Json> parseArray(int depth) {
        ++i_; // '['
        Json out = Json::array();
        skipSpace();
        if (!done() && peek() == ']') {
            ++i_;
            return out;
        }
        while (true) {
            skipSpace();
            Result<Json> v = parseValue(depth + 1);
            if (!v.ok()) {
                return v;
            }
            out.push(std::move(v).value());
            skipSpace();
            if (done()) {
                return unexpected("배열이 닫히지 않았다");
            }
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == ']') {
                ++i_;
                return out;
            }
            return unexpected("배열에서 , 또는 ] 를 기대했다");
        }
    }

    Result<Json> parseObject(int depth) {
        ++i_; // '{'
        Json out = Json::object();
        skipSpace();
        if (!done() && peek() == '}') {
            ++i_;
            return out;
        }
        while (true) {
            skipSpace();
            Result<std::string> key = parseString();
            if (!key.ok()) {
                return key.error();
            }
            skipSpace();
            if (done() || peek() != ':') {
                return unexpected("객체에서 : 를 기대했다");
            }
            ++i_;
            skipSpace();
            Result<Json> v = parseValue(depth + 1);
            if (!v.ok()) {
                return v;
            }
            out.set(std::move(key).value(), std::move(v).value());
            skipSpace();
            if (done()) {
                return unexpected("객체가 닫히지 않았다");
            }
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == '}') {
                ++i_;
                return out;
            }
            return unexpected("객체에서 , 또는 } 를 기대했다");
        }
    }

    std::string_view s_;
    usize i_ = 0;
};

} // namespace

// ── 만들기 ───────────────────────────────────────────────────────────────

Json Json::boolean(bool v) {
    Json j;
    j.kind_ = Kind::Bool;
    j.bool_ = v;
    return j;
}

Json Json::integerImpl(i64 v) {
    Json j;
    j.kind_ = Kind::Number;
    j.integral_ = true;
    j.int_ = v;
    j.num_ = static_cast<f64>(v);
    return j;
}

Json Json::number(f64 v) {
    Json j;
    j.kind_ = Kind::Number;
    j.num_ = v;
    j.int_ = static_cast<i64>(v);
    return j;
}

Json Json::string(std::string v) {
    Json j;
    j.kind_ = Kind::String;
    j.str_ = std::move(v);
    return j;
}

Json Json::array() {
    Json j;
    j.kind_ = Kind::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.kind_ = Kind::Object;
    return j;
}

// ── 보기 ─────────────────────────────────────────────────────────────────

bool Json::asBool(bool fallback) const noexcept { return kind_ == Kind::Bool ? bool_ : fallback; }

f64 Json::asNumber(f64 fallback) const noexcept { return kind_ == Kind::Number ? num_ : fallback; }

i64 Json::asInt(i64 fallback) const noexcept {
    if (kind_ != Kind::Number) {
        return fallback;
    }
    return integral_ ? int_ : static_cast<i64>(num_);
}

const std::string& Json::asString() const noexcept {
    static const std::string kEmpty;
    return kind_ == Kind::String ? str_ : kEmpty;
}

usize Json::size() const noexcept {
    if (kind_ == Kind::Array) {
        return arr_.size();
    }
    if (kind_ == Kind::Object) {
        return keys_.size();
    }
    return 0;
}

const Json& Json::at(usize i) const noexcept {
    if (kind_ != Kind::Array || i >= arr_.size()) {
        return nullValue();
    }
    return arr_[i];
}

const Json& Json::operator[](std::string_view key) const noexcept {
    if (kind_ != Kind::Object) {
        return nullValue();
    }
    for (usize i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            return vals_[i];
        }
    }
    return nullValue();
}

bool Json::has(std::string_view key) const noexcept {
    if (kind_ != Kind::Object) {
        return false;
    }
    return std::find(keys_.begin(), keys_.end(), key) != keys_.end();
}

// ── 쌓기 ─────────────────────────────────────────────────────────────────

void Json::push(Json v) {
    if (kind_ != Kind::Array) {
        *this = array();
    }
    arr_.push_back(std::move(v));
}

void Json::set(std::string key, Json v) {
    if (kind_ != Kind::Object) {
        *this = object();
    }
    for (usize i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            vals_[i] = std::move(v);
            return;
        }
    }
    keys_.push_back(std::move(key));
    vals_.push_back(std::move(v));
}

// ── 직렬화 ───────────────────────────────────────────────────────────────

void Json::appendTo(std::string& out, int indent, int depth) const {
    switch (kind_) {
    case Kind::Null:
        out += "null";
        return;
    case Kind::Bool:
        out += bool_ ? "true" : "false";
        return;
    case Kind::Number:
        out += integral_ ? std::to_string(int_) : numberToString(num_);
        return;
    case Kind::String:
        escapeInto(out, str_);
        return;
    case Kind::Array: {
        if (arr_.empty()) {
            out += "[]";
            return;
        }
        out.push_back('[');
        for (usize i = 0; i < arr_.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            indentInto(out, indent, depth + 1);
            if (depth + 1 > kMaxDepth) {
                out += "null"; // 깊이 상한. 스택을 깨는 대신 잘라 낸다.
            } else {
                arr_[i].appendTo(out, indent, depth + 1);
            }
        }
        indentInto(out, indent, depth);
        out.push_back(']');
        return;
    }
    case Kind::Object: {
        if (keys_.empty()) {
            out += "{}";
            return;
        }
        out.push_back('{');
        for (usize i = 0; i < keys_.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            indentInto(out, indent, depth + 1);
            escapeInto(out, keys_[i]);
            out.push_back(':');
            if (indent > 0) {
                out.push_back(' ');
            }
            if (depth + 1 > kMaxDepth) {
                out += "null";
            } else {
                vals_[i].appendTo(out, indent, depth + 1);
            }
        }
        indentInto(out, indent, depth);
        out.push_back('}');
        return;
    }
    }
}

std::string Json::dump() const { return dump(0); }

std::string Json::dump(int indent) const {
    std::string out;
    out.reserve(256);
    appendTo(out, indent, 0);
    return out;
}

Result<Json> Json::parse(std::string_view text) {
    Parser p(text);
    return p.parseDocument();
}

bool operator==(const Json& a, const Json& b) {
    if (a.kind_ != b.kind_) {
        return false;
    }
    switch (a.kind_) {
    case Json::Kind::Null:
        return true;
    case Json::Kind::Bool:
        return a.bool_ == b.bool_;
    case Json::Kind::Number:
        // JSON 에 숫자 타입은 하나뿐이다. 1 과 1.0 은 **같은 값**이다 —
        // 정수 표기 여부는 찍는 방식일 뿐이라 동등성에 넣지 않는다(왕복이 깨진다).
        return a.num_ == b.num_;
    case Json::Kind::String:
        return a.str_ == b.str_;
    case Json::Kind::Array:
        return a.arr_ == b.arr_;
    case Json::Kind::Object:
        return a.keys_ == b.keys_ && a.vals_ == b.vals_;
    }
    return false;
}

// ── 도우미 ───────────────────────────────────────────────────────────────

Json jsonObject(std::initializer_list<std::pair<const char*, Json>> members) {
    Json j = Json::object();
    for (const auto& m : members) {
        j.set(m.first, m.second);
    }
    return j;
}

Json jsonArray(std::initializer_list<Json> items) {
    Json j = Json::array();
    for (const auto& v : items) {
        j.push(v);
    }
    return j;
}

Json jsonRect(const Rect& r) {
    Json j = Json::array();
    j.push(Json::integer(static_cast<i64>(r.x)));
    j.push(Json::integer(static_cast<i64>(r.y)));
    j.push(Json::integer(static_cast<i64>(r.width)));
    j.push(Json::integer(static_cast<i64>(r.height)));
    return j;
}

Result<Rect> rectFromJson(const Json& v) {
    if (!v.isArray() || v.size() != 4) {
        return Err("영역은 [x, y, w, h] 네 정수여야 한다", ErrorCode::InvalidArgument);
    }
    for (usize i = 0; i < 4; ++i) {
        if (!v.at(i).isNumber()) {
            return Err("영역 성분이 숫자가 아니다", ErrorCode::InvalidArgument);
        }
    }
    Rect r{};
    r.x = static_cast<i32>(v.at(0).asInt());
    r.y = static_cast<i32>(v.at(1).asInt());
    r.width = static_cast<i32>(v.at(2).asInt());
    r.height = static_cast<i32>(v.at(3).asInt());
    if (r.width < 0 || r.height < 0) {
        return Err("영역의 폭·높이가 음수다", ErrorCode::InvalidArgument);
    }
    return Ok(r);
}

} // namespace mari::agent
