// Mari Paint — ByteReader 의 유니코드 문자열 읽기.
#include <mari/io/brush/byte_reader.hpp>

namespace mari::io::brush {
namespace {

/// 코드포인트 하나를 UTF-8 로 덧붙인다.
void appendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

std::string ByteReader::unicodeString(usize maxChars) noexcept {
    const u32 count = u32be();
    if (failed_)
        return {};
    // 길이 필드가 거짓말을 하는 경우가 실제로 온다. 남은 바이트로 먼저 거른다.
    if (count > maxChars || !has(static_cast<usize>(count) * 2)) {
        fail();
        return {};
    }
    std::string out;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u32 unit = u16be();
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < count) {
            const u32 lo = u16be();
            ++i;
            if (lo >= 0xDC00 && lo <= 0xDFFF)
                unit = 0x10000u + ((unit - 0xD800u) << 10) + (lo - 0xDC00u);
            else
                unit = 0xFFFD; // 짝이 안 맞는 서러게이트 — 대체 문자로 둔다
        }
        if (unit == 0 && i + 1 == count)
            break; // 끝의 NUL 종단은 버린다
        appendUtf8(out, unit);
    }
    return out;
}

} // namespace mari::io::brush
