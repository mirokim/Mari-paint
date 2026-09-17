// Mari Paint — 핸드셰이크 · 버전 협상 (docs/03 5.5)
#include <mari/sigan/handshake.hpp>

#include <algorithm>
#include <cstdlib>

namespace mari::sigan {
namespace {

void skipWs(const std::string& s, usize& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

/// JSON 문자열 하나를 읽는다. 실패하면 false.
bool parseString(const std::string& s, usize& i, std::string& out) {
    skipWs(s, i);
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            default: out.push_back(s[i]); break;
            }
        } else {
            out.push_back(s[i]);
        }
        ++i;
    }
    if (i >= s.size()) {
        return false;
    }
    ++i; // 닫는 따옴표
    return true;
}

/// 값 하나를 건너뛰거나(무시) 필요한 형태면 받아 간다.
/// **모르는 키는 여기서 조용히 사라진다**(docs/03 5.5 규칙 2).
bool skipValue(const std::string& s, usize& i);

bool skipContainer(const std::string& s, usize& i, char open, char close) {
    if (i >= s.size() || s[i] != open) {
        return false;
    }
    ++i;
    for (;;) {
        skipWs(s, i);
        if (i >= s.size()) {
            return false;
        }
        if (s[i] == close) {
            ++i;
            return true;
        }
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (open == '{') {
            std::string key;
            if (!parseString(s, i, key)) {
                return false;
            }
            skipWs(s, i);
            if (i >= s.size() || s[i] != ':') {
                return false;
            }
            ++i;
        }
        if (!skipValue(s, i)) {
            return false;
        }
    }
}

bool skipValue(const std::string& s, usize& i) {
    skipWs(s, i);
    if (i >= s.size()) {
        return false;
    }
    if (s[i] == '"') {
        std::string tmp;
        return parseString(s, i, tmp);
    }
    if (s[i] == '{') {
        return skipContainer(s, i, '{', '}');
    }
    if (s[i] == '[') {
        return skipContainer(s, i, '[', ']');
    }
    const usize start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') {
        ++i;
    }
    return i > start;
}

bool parseNumber(const std::string& s, usize& i, long long& out) {
    skipWs(s, i);
    const usize start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        ++i;
    }
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i == start) {
        return false;
    }
    out = std::strtoll(s.substr(start, i - start).c_str(), nullptr, 10);
    // 소수부가 붙어 있으면 버린다(정수 필드다).
    while (i < s.size() && (s[i] == '.' || (s[i] >= '0' && s[i] <= '9') || s[i] == 'e' ||
                            s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
        ++i;
    }
    return true;
}

bool parseStringArray(const std::string& s, usize& i, std::vector<std::string>& out) {
    skipWs(s, i);
    if (i >= s.size() || s[i] != '[') {
        return false;
    }
    ++i;
    for (;;) {
        skipWs(s, i);
        if (i >= s.size()) {
            return false;
        }
        if (s[i] == ']') {
            ++i;
            return true;
        }
        if (s[i] == ',') {
            ++i;
            continue;
        }
        std::string v;
        if (s[i] == '"') {
            if (!parseString(s, i, v)) {
                return false;
            }
            out.push_back(std::move(v));
        } else if (!skipValue(s, i)) { // 문자열이 아닌 원소는 무시한다
            return false;
        }
    }
}

void appendEscaped(std::string& out, const std::string& v) {
    out.push_back('"');
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

} // namespace

bool Handshake::hasCap(const std::string& c) const noexcept {
    return std::find(caps.begin(), caps.end(), c) != caps.end();
}

bool Negotiated::hasCap(const std::string& c) const noexcept {
    return std::find(caps.begin(), caps.end(), c) != caps.end();
}

Negotiated negotiate(const Handshake& local, const Handshake& remote) noexcept {
    Negotiated n;
    // 규칙 3: proto 가 다르면 **공통 최소로 내려간다. 끊지 않는다.**
    n.proto = std::min(local.proto, remote.proto);
    n.protoDowngraded = local.proto != remote.proto;
    try {
        for (const auto& c : local.caps) {
            if (std::find(remote.caps.begin(), remote.caps.end(), c) != remote.caps.end()) {
                n.caps.push_back(c);
            } else {
                // 상대가 모르는 능력. 에러가 아니다 — 생략하고 정직하게 남긴다.
                n.dropped.push_back(c);
            }
        }
    } catch (...) {
        // 할당 실패해도 연결을 끊지 않는다. 최소 능력으로 계속 간다.
    }
    return n;
}

std::string encodeHandshake(const Handshake& h) {
    std::string out = "{\"proto\":";
    out += std::to_string(h.proto);
    out += ",\"app\":";
    appendEscaped(out, h.app);
    out += ",\"caps\":[";
    for (usize i = 0; i < h.caps.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        appendEscaped(out, h.caps[i]);
    }
    out += "],\"sessionId\":";
    out += std::to_string(h.sessionId);
    out += ",\"resumeFromSeq\":";
    out += std::to_string(h.resumeFromSeq);
    out += ",\"wallAnchorUnixMs\":";
    out += std::to_string(h.wallAnchorUnixMs);
    out += "}\n";
    return out;
}

Result<Handshake> decodeHandshake(const std::string& json) {
    Handshake h;
    h.caps.clear();
    bool sawCaps = false;
    usize i = 0;
    skipWs(json, i);
    if (i >= json.size() || json[i] != '{') {
        return Err("핸드셰이크가 JSON 오브젝트가 아니다", ErrorCode::ParseError);
    }
    ++i;
    for (;;) {
        skipWs(json, i);
        if (i >= json.size()) {
            return Err("핸드셰이크가 중간에 끊겼다", ErrorCode::ParseError);
        }
        if (json[i] == '}') {
            break;
        }
        if (json[i] == ',') {
            ++i;
            continue;
        }
        std::string key;
        if (!parseString(json, i, key)) {
            return Err("핸드셰이크 키를 읽지 못했다", ErrorCode::ParseError);
        }
        skipWs(json, i);
        if (i >= json.size() || json[i] != ':') {
            return Err("핸드셰이크에 ':' 가 없다", ErrorCode::ParseError);
        }
        ++i;
        bool consumed = false;
        if (key == "proto") {
            long long v = 0;
            if (parseNumber(json, i, v)) {
                h.proto = static_cast<u32>(v < 0 ? 0 : v);
                consumed = true;
            }
        } else if (key == "app") {
            consumed = parseString(json, i, h.app);
        } else if (key == "caps" || key == "accepts") {
            // Sigan 쪽은 "accepts" 로 답한다. 둘 다 같은 뜻으로 받는다.
            consumed = parseStringArray(json, i, h.caps);
            sawCaps = sawCaps || consumed;
        } else if (key == "sessionId") {
            long long v = 0;
            if (parseNumber(json, i, v)) {
                h.sessionId = static_cast<u64>(v);
                consumed = true;
            }
        } else if (key == "resumeFromSeq") {
            long long v = 0;
            if (parseNumber(json, i, v)) {
                h.resumeFromSeq = static_cast<u64>(v);
                consumed = true;
            }
        } else if (key == "wallAnchorUnixMs") {
            long long v = 0;
            if (parseNumber(json, i, v)) {
                h.wallAnchorUnixMs = static_cast<i64>(v);
                consumed = true;
            }
        }
        if (!consumed) {
            // 규칙 2: **모르는 필드(또는 타입이 다른 필드)는 무시한다. 에러가 아니다.**
            if (!skipValue(json, i)) {
                return Err("핸드셰이크 값을 건너뛰지 못했다", ErrorCode::ParseError);
            }
        }
    }
    (void)sawCaps;
    return Ok(std::move(h));
}

} // namespace mari::sigan
