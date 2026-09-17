// Mari Paint — 최소 XML 파서 구현. 선언은 include/mari/ora/xml.hpp.
#include <mari/ora/xml.hpp>

#include <cstdlib>

namespace mari::ora {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// XML 이름에 쓸 수 있는 글자인지. 스펙보다 관대하게 본다(접두사 ':' 포함).
bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80;
}

/// UTF-8 한 글자를 덧붙인다.
void appendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
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

/// 엔티티를 푼다. 모르는 엔티티는 원문 그대로 둔다(정보를 잃지 않는다).
std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size();) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        const usize semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back(s[i++]);
            continue;
        }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        if (name == "amp") {
            out.push_back('&');
        } else if (name == "lt") {
            out.push_back('<');
        } else if (name == "gt") {
            out.push_back('>');
        } else if (name == "quot") {
            out.push_back('"');
        } else if (name == "apos") {
            out.push_back('\'');
        } else if (name.size() > 1 && name[0] == '#') {
            const std::string digits(name.substr(name[1] == 'x' || name[1] == 'X' ? 2 : 1));
            const int base = (name[1] == 'x' || name[1] == 'X') ? 16 : 10;
            char* end = nullptr;
            const unsigned long cp = std::strtoul(digits.c_str(), &end, base);
            if (end != nullptr && *end == '\0' && cp != 0 && cp <= 0x10FFFFul)
                appendUtf8(out, static_cast<u32>(cp));
            else
                out.append(s.substr(i, semi - i + 1));
        } else {
            out.append(s.substr(i, semi - i + 1));
            i = semi + 1;
            continue;
        }
        i = semi + 1;
    }
    return out;
}

/// 재귀 하강 파서. 입력 전체를 한 번만 훑는다.
class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<XmlNode> run() {
        skipMisc();
        if (pos_ >= s_.size() || s_[pos_] != '<')
            return Err("XML: 루트 엘리먼트가 없다", ErrorCode::ParseError);
        auto root = parseElement();
        if (!root)
            return root.error();
        return root;
    }

private:
    void skipSpace() {
        while (pos_ < s_.size() && isSpace(s_[pos_]))
            ++pos_;
    }

    /// 선언(<?...?>), 주석(<!-- -->), DOCTYPE(<!...>) 을 건너뛴다.
    void skipMisc() {
        for (;;) {
            skipSpace();
            if (pos_ + 1 >= s_.size() || s_[pos_] != '<')
                return;
            if (s_[pos_ + 1] == '?') {
                const usize e = s_.find("?>", pos_ + 2);
                pos_ = (e == std::string_view::npos) ? s_.size() : e + 2;
            } else if (s_.compare(pos_, 4, "<!--") == 0) {
                const usize e = s_.find("-->", pos_ + 4);
                pos_ = (e == std::string_view::npos) ? s_.size() : e + 3;
            } else if (s_[pos_ + 1] == '!') {
                const usize e = s_.find('>', pos_ + 2);
                pos_ = (e == std::string_view::npos) ? s_.size() : e + 1;
            } else {
                return;
            }
        }
    }

    std::string_view parseName() {
        const usize start = pos_;
        while (pos_ < s_.size() && isNameChar(s_[pos_]))
            ++pos_;
        return s_.substr(start, pos_ - start);
    }

    /// '<' 위에서 시작해 해당 엘리먼트를 통째로 읽는다.
    Result<XmlNode> parseElement() {
        if (++depth_ > kMaxDepth)
            return Err("XML: 중첩이 너무 깊다", ErrorCode::ParseError);

        ++pos_; // '<'
        XmlNode node;
        node.name = std::string(parseName());
        if (node.name.empty())
            return Err("XML: 태그 이름이 비었다", ErrorCode::ParseError);

        for (;;) {
            skipSpace();
            if (pos_ >= s_.size())
                return Err("XML: 태그가 닫히지 않았다", ErrorCode::ParseError);
            if (s_[pos_] == '/') {
                if (pos_ + 1 >= s_.size() || s_[pos_ + 1] != '>')
                    return Err("XML: '/' 뒤에 '>' 가 없다", ErrorCode::ParseError);
                pos_ += 2;
                --depth_;
                return node; // 자기닫힘
            }
            if (s_[pos_] == '>') {
                ++pos_;
                break;
            }
            const std::string_view key = parseName();
            if (key.empty())
                return Err("XML: 속성 이름이 비었다", ErrorCode::ParseError);
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '=')
                return Err("XML: 속성에 '=' 가 없다", ErrorCode::ParseError);
            ++pos_;
            skipSpace();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\''))
                return Err("XML: 속성 값이 따옴표로 싸이지 않았다", ErrorCode::ParseError);
            const char quote = s_[pos_++];
            const usize vstart = pos_;
            while (pos_ < s_.size() && s_[pos_] != quote)
                ++pos_;
            if (pos_ >= s_.size())
                return Err("XML: 속성 값이 닫히지 않았다", ErrorCode::ParseError);
            node.attrs.emplace_back(std::string(key), unescape(s_.substr(vstart, pos_ - vstart)));
            ++pos_;
        }

        // 자식과 텍스트. 텍스트는 버린다.
        for (;;) {
            const usize lt = s_.find('<', pos_);
            if (lt == std::string_view::npos)
                return Err("XML: 닫는 태그가 없다: " + node.name, ErrorCode::ParseError);
            pos_ = lt;
            if (pos_ + 1 >= s_.size())
                return Err("XML: 입력이 중간에 끊겼다", ErrorCode::ParseError);
            if (s_[pos_ + 1] == '/') {
                pos_ += 2;
                const std::string_view close = parseName();
                skipSpace();
                if (pos_ >= s_.size() || s_[pos_] != '>')
                    return Err("XML: 닫는 태그가 깨졌다", ErrorCode::ParseError);
                ++pos_;
                if (close != node.name)
                    return Err("XML: 태그 짝이 안 맞는다: " + node.name, ErrorCode::ParseError);
                --depth_;
                return node;
            }
            if (s_.compare(pos_, 4, "<!--") == 0) {
                const usize e = s_.find("-->", pos_ + 4);
                if (e == std::string_view::npos)
                    return Err("XML: 주석이 닫히지 않았다", ErrorCode::ParseError);
                pos_ = e + 3;
                continue;
            }
            if (s_.compare(pos_, 9, "<![CDATA[") == 0) {
                const usize e = s_.find("]]>", pos_ + 9);
                if (e == std::string_view::npos)
                    return Err("XML: CDATA 가 닫히지 않았다", ErrorCode::ParseError);
                pos_ = e + 3;
                continue;
            }
            if (s_[pos_ + 1] == '?' || s_[pos_ + 1] == '!') {
                const usize e = s_.find('>', pos_ + 2);
                if (e == std::string_view::npos)
                    return Err("XML: 선언이 닫히지 않았다", ErrorCode::ParseError);
                pos_ = e + 1;
                continue;
            }
            auto child = parseElement();
            if (!child)
                return child.error();
            node.children.push_back(std::move(child).value());
        }
    }

    static constexpr int kMaxDepth = 256;

    std::string_view s_;
    usize pos_ = 0;
    int depth_ = 0;
};

} // namespace

const std::string* XmlNode::attr(std::string_view key) const {
    for (const auto& [k, v] : attrs)
        if (k == key)
            return &v;
    return nullptr;
}

std::string XmlNode::attrOr(std::string_view key, std::string fallback) const {
    const std::string* v = attr(key);
    return v != nullptr ? *v : std::move(fallback);
}

Result<XmlNode> parseXml(std::string_view text) {
    if (text.empty())
        return Err("XML: 입력이 비었다", ErrorCode::ParseError);
    Parser p(text);
    return p.run();
}

std::string xmlEscape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

} // namespace mari::ora
