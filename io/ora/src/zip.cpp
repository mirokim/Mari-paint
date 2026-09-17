// Mari Paint — ZIP 컨테이너 구현. 선언은 include/mari/ora/zip.hpp.
//
// 바이트 레이아웃은 PKWARE APPNOTE 4.3 을 따른다. 리틀엔디안 고정.
//   로컬 파일 헤더   0x04034B50  30바이트 + 이름
//   중앙 디렉터리    0x02014B50  46바이트 + 이름
//   EOCD             0x06054B50  22바이트 + 코멘트
#include <mari/ora/zip.hpp>

#include <zlib.h>

#include <cstring>

namespace mari::ora {
namespace {

constexpr u32 kLocalSig = 0x04034B50u;
constexpr u32 kCentralSig = 0x02014B50u;
constexpr u32 kEocdSig = 0x06054B50u;
constexpr usize kLocalHeaderSize = 30;
constexpr usize kCentralHeaderSize = 46;
constexpr usize kEocdSize = 22;

void put16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v & 0xFFu));
    b.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}

void put32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFFu));
    b.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    b.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    b.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

u16 get16(const u8* p) { return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8)); }

u32 get32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

/// ZIP 은 8.3 시절의 DOS 타임스탬프를 쓴다. 재현 가능한 출력을 위해
/// 실제 시각 대신 고정값(1980-01-01 00:00:00)을 쓴다 — 같은 문서는 같은 바이트가 된다.
constexpr u16 kDosTime = 0;
constexpr u16 kDosDate = 0x0021; // 1980-01-01

} // namespace

// ── deflate / inflate ────────────────────────────────────────────────────

Result<std::vector<u8>> deflateRaw(const u8* data, usize size, int level) {
    if (size > 0 && data == nullptr)
        return Err("deflateRaw: 널 입력", ErrorCode::InvalidArgument);
    if (level < 0 || level > 9)
        level = 6;

    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return Err("deflateInit2 실패", ErrorCode::Unknown);

    std::vector<u8> out;
    out.resize(size / 2 + 1024);
    usize produced = 0;
    zs.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);

    for (;;) {
        if (produced == out.size())
            out.resize(out.size() * 2);
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(out.size() - produced);
        const int rc = deflate(&zs, Z_FINISH);
        produced = out.size() - zs.avail_out;
        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            deflateEnd(&zs);
            return Err("deflate 실패", ErrorCode::Unknown);
        }
        if (rc == Z_BUF_ERROR && zs.avail_out != 0) {
            deflateEnd(&zs);
            return Err("deflate 진행 불가", ErrorCode::Unknown);
        }
    }
    deflateEnd(&zs);
    out.resize(produced);
    return out;
}

Result<std::vector<u8>> inflateRaw(const u8* data, usize size, usize expectedSize) {
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return Err("inflateInit2 실패", ErrorCode::Unknown);

    std::vector<u8> out;
    out.resize(expectedSize > 0 ? expectedSize : (size * 4 + 1024));
    usize produced = 0;
    zs.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);

    for (;;) {
        if (produced == out.size())
            out.resize(out.size() * 2 + 1024);
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(out.size() - produced);
        const int rc = inflate(&zs, Z_NO_FLUSH);
        produced = out.size() - zs.avail_out;
        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK) {
            inflateEnd(&zs);
            return Err("inflate 실패: 압축 스트림이 깨졌다", ErrorCode::ParseError);
        }
        if (zs.avail_in == 0 && zs.avail_out != 0) {
            inflateEnd(&zs);
            return Err("inflate 실패: 입력이 중간에 끊겼다", ErrorCode::ParseError);
        }
    }
    inflateEnd(&zs);
    out.resize(produced);
    return out;
}

// ── ZipWriter ────────────────────────────────────────────────────────────

bool ZipWriter::contains(std::string_view name) const {
    for (const Pending& e : entries_)
        if (e.name == name)
            return true;
    return false;
}

Result<void> ZipWriter::beginOra() {
    if (!entries_.empty())
        return Err("beginOra: mimetype 은 첫 항목이어야 한다", ErrorCode::InvalidArgument);
    static constexpr std::string_view kMime = "image/openraster";
    return add("mimetype", kMime, ZipMethod::Store);
}

Result<void> ZipWriter::add(std::string name, std::string_view text, ZipMethod method, int level) {
    return add(std::move(name), reinterpret_cast<const u8*>(text.data()), text.size(), method,
               level);
}

Result<void> ZipWriter::add(std::string name, const u8* data, usize size, ZipMethod method,
                            int level) {
    if (name.empty())
        return Err("ZipWriter::add: 이름이 비었다", ErrorCode::InvalidArgument);
    if (name.size() > 0xFFFFu)
        return Err("ZipWriter::add: 이름이 너무 길다", ErrorCode::InvalidArgument);
    if (contains(name))
        return Err("ZipWriter::add: 이름이 중복됐다: " + name, ErrorCode::InvalidArgument);
    if (size > 0xFFFFFFFFull)
        return Err("ZipWriter::add: 4GB 이상은 지원하지 않는다(Zip64 없음)", ErrorCode::Unsupported);
    if (buffer_.size() > 0xFFFFFFFFull)
        return Err("ZipWriter::add: 아카이브가 4GB 를 넘었다", ErrorCode::Unsupported);

    std::vector<u8> payload;
    const u8* raw = data;
    usize rawSize = size;
    if (method == ZipMethod::Deflate) {
        auto z = deflateRaw(data, size, level);
        if (!z)
            return z.error();
        payload = std::move(z).value();
        // 압축이 되레 커지면 무압축으로 떨어뜨린다.
        if (payload.size() >= size) {
            method = ZipMethod::Store;
            payload.clear();
        }
    }
    const u8* stored = (method == ZipMethod::Deflate) ? payload.data() : raw;
    const usize storedSize = (method == ZipMethod::Deflate) ? payload.size() : rawSize;

    Pending p;
    p.name = std::move(name);
    p.method = method;
    p.crc = static_cast<u32>(
        crc32(crc32(0L, nullptr, 0), static_cast<const Bytef*>(data), static_cast<uInt>(size)));
    p.compressedSize = static_cast<u32>(storedSize);
    p.uncompressedSize = static_cast<u32>(size);
    p.localHeaderOffset = static_cast<u32>(buffer_.size());

    put32(buffer_, kLocalSig);
    put16(buffer_, method == ZipMethod::Store ? u16{10} : u16{20}); // 필요 버전
    put16(buffer_, 0);                                              // 플래그
    put16(buffer_, static_cast<u16>(method));
    put16(buffer_, kDosTime);
    put16(buffer_, kDosDate);
    put32(buffer_, p.crc);
    put32(buffer_, p.compressedSize);
    put32(buffer_, p.uncompressedSize);
    put16(buffer_, static_cast<u16>(p.name.size()));
    put16(buffer_, 0); // extra 없음 — 정렬 패딩도 넣지 않는다
    buffer_.insert(buffer_.end(), p.name.begin(), p.name.end());
    if (storedSize > 0)
        buffer_.insert(buffer_.end(), stored, stored + storedSize);

    entries_.push_back(std::move(p));
    return Ok();
}

Result<std::vector<u8>> ZipWriter::finish() {
    if (buffer_.size() > 0xFFFFFFFFull)
        return Err("ZipWriter::finish: 아카이브가 4GB 를 넘었다", ErrorCode::Unsupported);
    const u32 centralOffset = static_cast<u32>(buffer_.size());

    for (const Pending& e : entries_) {
        put32(buffer_, kCentralSig);
        put16(buffer_, 0x031E);                                        // made-by: unix, 3.0
        put16(buffer_, e.method == ZipMethod::Store ? u16{10} : u16{20});
        put16(buffer_, 0);
        put16(buffer_, static_cast<u16>(e.method));
        put16(buffer_, kDosTime);
        put16(buffer_, kDosDate);
        put32(buffer_, e.crc);
        put32(buffer_, e.compressedSize);
        put32(buffer_, e.uncompressedSize);
        put16(buffer_, static_cast<u16>(e.name.size()));
        put16(buffer_, 0); // extra
        put16(buffer_, 0); // comment
        put16(buffer_, 0); // disk number
        put16(buffer_, 0); // internal attrs
        put32(buffer_, 0x81A40000u); // external attrs: 0644 regular file
        put32(buffer_, e.localHeaderOffset);
        buffer_.insert(buffer_.end(), e.name.begin(), e.name.end());
    }

    const u32 centralSize = static_cast<u32>(buffer_.size()) - centralOffset;
    const u16 count = static_cast<u16>(entries_.size());
    put32(buffer_, kEocdSig);
    put16(buffer_, 0); // this disk
    put16(buffer_, 0); // central dir start disk
    put16(buffer_, count);
    put16(buffer_, count);
    put32(buffer_, centralSize);
    put32(buffer_, centralOffset);
    put16(buffer_, 0); // comment length

    std::vector<u8> out = std::move(buffer_);
    buffer_.clear();
    entries_.clear();
    return out;
}

// ── ZipReader ────────────────────────────────────────────────────────────

Result<ZipReader> ZipReader::open(std::vector<u8> bytes) {
    ZipReader r;
    r.bytes_ = std::move(bytes);
    const std::vector<u8>& b = r.bytes_;
    if (b.size() < kEocdSize)
        return Err("ZIP 이 아니다: 너무 짧다", ErrorCode::ParseError);

    // EOCD 를 뒤에서부터 찾는다. 코멘트는 최대 64KB.
    usize eocd = 0;
    bool found = false;
    const usize scanLimit = b.size() < (kEocdSize + 0xFFFFu) ? b.size() : (kEocdSize + 0xFFFFu);
    for (usize back = kEocdSize; back <= scanLimit; ++back) {
        const usize pos = b.size() - back;
        if (get32(b.data() + pos) == kEocdSig) {
            eocd = pos;
            found = true;
            break;
        }
    }
    if (!found)
        return Err("ZIP 이 아니다: EOCD 를 찾지 못했다", ErrorCode::ParseError);

    const u16 count = get16(b.data() + eocd + 10);
    const u32 centralSize = get32(b.data() + eocd + 12);
    const u32 centralOffset = get32(b.data() + eocd + 16);
    if (static_cast<u64>(centralOffset) + centralSize > b.size())
        return Err("ZIP 중앙 디렉터리가 파일 밖을 가리킨다", ErrorCode::ParseError);

    usize p = centralOffset;
    r.entries_.reserve(count);
    for (u16 i = 0; i < count; ++i) {
        if (p + kCentralHeaderSize > b.size() || get32(b.data() + p) != kCentralSig)
            return Err("ZIP 중앙 디렉터리 항목이 깨졌다", ErrorCode::ParseError);
        const u16 method = get16(b.data() + p + 10);
        const u32 crc = get32(b.data() + p + 16);
        const u32 csize = get32(b.data() + p + 20);
        const u32 usize_ = get32(b.data() + p + 24);
        const u16 nameLen = get16(b.data() + p + 28);
        const u16 extraLen = get16(b.data() + p + 30);
        const u16 commentLen = get16(b.data() + p + 32);
        const u32 localOffset = get32(b.data() + p + 42);
        if (p + kCentralHeaderSize + nameLen + extraLen + commentLen > b.size())
            return Err("ZIP 중앙 디렉터리 항목이 파일 밖을 가리킨다", ErrorCode::ParseError);
        if (method != 0 && method != 8)
            return Err("지원하지 않는 ZIP 압축 방식이다", ErrorCode::Unsupported);

        ZipEntryInfo info;
        info.name.assign(reinterpret_cast<const char*>(b.data() + p + kCentralHeaderSize), nameLen);
        info.method = static_cast<ZipMethod>(method);
        info.crc = crc;
        info.compressedSize = csize;
        info.uncompressedSize = usize_;
        info.localHeaderOffset = localOffset;
        r.entries_.push_back(std::move(info));
        p += kCentralHeaderSize + nameLen + extraLen + commentLen;
    }
    return r;
}

const ZipEntryInfo* ZipReader::find(std::string_view name) const {
    for (const ZipEntryInfo& e : entries_)
        if (e.name == name)
            return &e;
    return nullptr;
}

Result<std::vector<u8>> ZipReader::read(std::string_view name) const {
    const ZipEntryInfo* e = find(name);
    if (e == nullptr)
        return Err(std::string("ZIP 항목이 없다: ") + std::string(name), ErrorCode::NotFound);

    const std::vector<u8>& b = bytes_;
    const u64 lh = e->localHeaderOffset;
    if (lh + kLocalHeaderSize > b.size() || get32(b.data() + lh) != kLocalSig)
        return Err("ZIP 로컬 헤더가 깨졌다: " + e->name, ErrorCode::ParseError);
    const u16 nameLen = get16(b.data() + lh + 26);
    const u16 extraLen = get16(b.data() + lh + 28);
    const u64 dataOffset = lh + kLocalHeaderSize + nameLen + extraLen;
    if (dataOffset + e->compressedSize > b.size())
        return Err("ZIP 항목 데이터가 파일 밖을 가리킨다: " + e->name, ErrorCode::ParseError);

    const u8* src = b.data() + dataOffset;
    std::vector<u8> out;
    if (e->method == ZipMethod::Store) {
        out.assign(src, src + e->compressedSize);
    } else {
        auto inf = inflateRaw(src, static_cast<usize>(e->compressedSize),
                              static_cast<usize>(e->uncompressedSize));
        if (!inf)
            return inf.error();
        out = std::move(inf).value();
    }
    if (out.size() != e->uncompressedSize)
        return Err("ZIP 항목 길이가 헤더와 다르다: " + e->name, ErrorCode::ParseError);
    const u32 crc = static_cast<u32>(
        crc32(crc32(0L, nullptr, 0), out.data(), static_cast<uInt>(out.size())));
    if (crc != e->crc)
        return Err("ZIP 항목 CRC 불일치: " + e->name, ErrorCode::ParseError);
    return out;
}

} // namespace mari::ora
