// Mari Paint — 최소 tar(ustar) 리더 구현.
#include <mari/io/brush/tar.hpp>

#include <cstring>

namespace mari::io::brush {
namespace {

constexpr usize kBlock = 512;
constexpr usize kMaxEntries = 4096;

/// ustar 헤더의 8진수 필드를 읽는다. 공백·NUL 패딩을 허용한다.
bool parseOctal(const u8* field, usize len, u64& out) {
    u64 v = 0;
    bool any = false;
    for (usize i = 0; i < len; ++i) {
        const char c = static_cast<char>(field[i]);
        if (c == ' ' || c == '\0')
            continue;
        if (c < '0' || c > '7')
            return false;
        v = v * 8 + static_cast<u64>(c - '0');
        any = true;
    }
    if (!any)
        return false;
    out = v;
    return true;
}

/// 헤더 체크섬. 체크섬 필드 자체는 공백으로 간주하고 더한다.
u32 headerChecksum(const u8* h) {
    u32 sum = 0;
    for (usize i = 0; i < kBlock; ++i)
        sum += (i >= 148 && i < 156) ? 32u : h[i];
    return sum;
}

bool isZeroBlock(const u8* h) {
    for (usize i = 0; i < kBlock; ++i)
        if (h[i] != 0)
            return false;
    return true;
}

std::string fieldString(const u8* p, usize len) {
    usize n = 0;
    while (n < len && p[n] != '\0')
        ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

} // namespace

bool looksLikeTar(const u8* data, usize size) {
    if (data == nullptr || size < kBlock)
        return false;
    if (std::memcmp(data + 257, "ustar", 5) == 0)
        return true;
    u64 stored = 0;
    return parseOctal(data + 148, 8, stored) && stored == headerChecksum(data);
}

Result<std::vector<TarEntry>> readTar(const u8* data, usize size) {
    if (data == nullptr || size == 0)
        return Err("tar 버퍼가 비어 있다", ErrorCode::InvalidArgument);
    if (size < kBlock)
        return Err("tar 아카이브가 헤더 한 블록보다 짧다", ErrorCode::ParseError);

    std::vector<TarEntry> entries;
    usize pos = 0;
    while (pos + kBlock <= size) {
        const u8* h = data + pos;
        if (isZeroBlock(h))
            break; // 끝 표식(보통 0 블록 2개)

        u64 stored = 0;
        if (!parseOctal(h + 148, 8, stored))
            return Err("tar 헤더 체크섬 필드를 읽지 못했다", ErrorCode::ParseError);
        if (stored != headerChecksum(h))
            return Err("tar 헤더 체크섬이 맞지 않는다", ErrorCode::ParseError);

        u64 entrySize = 0;
        if (!parseOctal(h + 124, 12, entrySize))
            return Err("tar 항목 크기 필드를 읽지 못했다", ErrorCode::ParseError);

        TarEntry e;
        e.name = fieldString(h + 0, 100);
        // ustar prefix 필드가 있으면 앞에 붙인다.
        if (std::memcmp(h + 257, "ustar", 5) == 0) {
            const std::string prefix = fieldString(h + 345, 155);
            if (!prefix.empty())
                e.name = prefix + "/" + e.name;
        }
        e.typeFlag = static_cast<char>(h[156]);
        e.offset = pos + kBlock;
        e.size = static_cast<usize>(entrySize);

        if (e.size > size || e.offset > size - e.size)
            return Err("tar 항목이 아카이브 밖을 가리킨다", ErrorCode::ParseError);

        const usize padded = (e.size + kBlock - 1) / kBlock * kBlock;
        if (e.offset > size - padded && padded != 0)
            return Err("tar 항목 데이터가 잘렸다", ErrorCode::ParseError);

        entries.push_back(std::move(e));
        if (entries.size() > kMaxEntries)
            return Err("tar 항목이 너무 많다", ErrorCode::ParseError);

        pos = entries.back().offset + padded;
    }

    return Ok(std::move(entries));
}

} // namespace mari::io::brush
