// Mari Paint — append-only 저널 (docs/03 4.2 · 5.4)
#include <mari/sigan/journal.hpp>

#include <bit>
#include <cstring>

namespace mari::sigan {
namespace {

constexpr usize kBufCapacity = 64u * 1024u; ///< 미리 잡는다. 핫 패스에서 할당하지 않는다
constexpr usize kRecordOverhead = 1 + 4 + 4; ///< tag + len + crc

void putU32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
    p[2] = static_cast<u8>((v >> 16) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24) & 0xFFu);
}

void putU64(u8* p, u64 v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>((v >> (8 * i)) & 0xFFull);
    }
}

u32 getU32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

u64 getU64(const u8* p) noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(p[i]) << (8 * i);
    }
    return v;
}

} // namespace

std::vector<u64> JournalScan::seqGaps() const {
    std::vector<u64> gaps;
    for (usize i = 1; i < frames.size(); ++i) {
        const u64 prev = frames[i - 1].seq;
        for (u64 s = prev + 1; s < frames[i].seq; ++s) {
            gaps.push_back(s);
            if (gaps.size() > 1024) { // 구멍이 이렇게 많으면 이미 설계가 깨진 거다
                return gaps;
            }
        }
    }
    return gaps;
}

Journal::~Journal() {
    flush();
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

Result<JournalPtr> Journal::create(const std::string& path, u64 sessionId, i64 wallAnchorUnixMs) {
    JournalPtr j(new Journal());
    j->path_ = path;
    j->fp_ = std::fopen(path.c_str(), "wb");
    if (j->fp_ == nullptr) {
        return Err("저널 파일을 열지 못했다: " + path, ErrorCode::IoError);
    }
    j->buf_.resize(kBufCapacity);
    j->bufLen_ = 0;

    u8 header[kJournalHeaderSize]{};
    std::memcpy(header, kJournalMagic, sizeof(kJournalMagic));
    putU32(header + 8, kJournalVersion);
    putU32(header + 12, static_cast<u32>(kJournalHeaderSize));
    putU64(header + 16, sessionId);
    putU64(header + 24, static_cast<u64>(wallAnchorUnixMs));
    if (std::fwrite(header, 1, kJournalHeaderSize, j->fp_) != kJournalHeaderSize) {
        return Err("저널 헤더를 쓰지 못했다: " + path, ErrorCode::IoError);
    }
    std::fflush(j->fp_);
    return Ok(std::move(j));
}

bool Journal::writeRecord(JournalRecord tag, const u8* payload, u32 len) noexcept {
    if (fp_ == nullptr) {
        failed_ = true;
        return false;
    }
    const usize need = kRecordOverhead + len;
    if (need > buf_.size()) { // 비정상적으로 큰 레코드는 버퍼를 거치지 않는다
        failed_ = true;
        return false;
    }
    if (bufLen_ + need > buf_.size() && !flush()) {
        return false;
    }
    u8* p = buf_.data() + bufLen_;
    p[0] = static_cast<u8>(tag);
    putU32(p + 1, len);
    if (len != 0 && payload != nullptr) {
        std::memcpy(p + 5, payload, len);
    }
    const u32 c = crc32(p, 5 + len);
    putU32(p + 5 + len, c);
    bufLen_ += need;
    return true;
}

bool Journal::appendFrame(const StrokeFrame& f) noexcept {
    u8 wire[kFrameSize];
    encodeFrame(f, wire);
    if (!writeRecord(JournalRecord::Frame, wire, static_cast<u32>(kFrameSize))) {
        return false;
    }
    ++frameCount_;
    return true;
}

bool Journal::markStrokeEnd(u64 seq) noexcept {
    u8 payload[8];
    putU64(payload, seq);
    // 획 끝마다 flush. fsync 는 하지 않는다 — 16ms 예산(docs/03 5.4).
    return writeRecord(JournalRecord::StrokeEnd, payload, 8) && flush();
}

bool Journal::appendNote(const std::string& text) noexcept {
    const usize maxLen = buf_.size() - kRecordOverhead;
    const usize n = text.size() < maxLen ? text.size() : maxLen;
    return writeRecord(JournalRecord::Note, reinterpret_cast<const u8*>(text.data()),
                       static_cast<u32>(n));
}

bool Journal::flush() noexcept {
    if (fp_ == nullptr) {
        return false;
    }
    if (bufLen_ != 0) {
        const usize want = bufLen_;
        const usize written = std::fwrite(buf_.data(), 1, bufLen_, fp_);
        bufLen_ = 0;
        // 🔴 **부분 쓰기도 실패다.** 반만 들어간 레코드는 기록이 아니다.
        //    예전에는 written == 0 만 봤다 — 디스크가 도중에 찬 경우를 놓친다.
        if (written != want) {
            failed_ = true;
            return false;
        }
    }
    // 🔴 fflush 실패를 failed_ 로 남긴다. 이걸 빠뜨리면 "쓴 줄 알았는데 아무 것도
    //    안 남은" 상태가 조용히 계속된다 — 가장 조용하고 가장 큰 거짓이다
    //    (docs/06 6절 H1). 정직한 실패가 조용한 성공보다 낫다.
    if (std::fflush(fp_) != 0) {
        failed_ = true;
        return false;
    }
    return true;
}

Result<JournalScan> Journal::scan(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return Err("저널 파일을 열지 못했다: " + path, ErrorCode::NotFound);
    }
    std::vector<u8> data;
    u8 chunk[8192];
    for (;;) {
        const usize n = std::fread(chunk, 1, sizeof(chunk), fp);
        if (n == 0) {
            break;
        }
        data.insert(data.end(), chunk, chunk + n);
    }
    std::fclose(fp);

    if (data.size() < kJournalHeaderSize ||
        std::memcmp(data.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
        return Err("저널 헤더가 깨졌다: " + path, ErrorCode::ParseError);
    }

    JournalScan out;
    out.version = getU32(data.data() + 8);
    out.sessionId = getU64(data.data() + 16);
    out.wallAnchorUnixMs = static_cast<i64>(getU64(data.data() + 24));

    const usize headerSize = getU32(data.data() + 12);
    usize i = headerSize < kJournalHeaderSize ? kJournalHeaderSize : headerSize;
    while (i < data.size()) {
        // 꼬리가 잘렸는지 먼저 본다. 크래시는 조용히 넘기지 않는다.
        if (i + 5 > data.size()) {
            out.truncated = true;
            break;
        }
        const u8 tag = data[i];
        const u32 len = getU32(data.data() + i + 1);
        const usize total = kRecordOverhead + len;
        if (i + total > data.size()) {
            out.truncated = true;
            break;
        }
        const u32 stored = getU32(data.data() + i + 5 + len);
        if (crc32(data.data() + i, 5 + len) != stored) {
            out.truncated = true; // 반쯤 쓰인 레코드 — 여기서 멈춘다
            break;
        }
        switch (static_cast<JournalRecord>(tag)) {
        case JournalRecord::Frame: {
            if (len != kFrameSize) {
                out.truncated = true;
                i = data.size();
                break;
            }
            StrokeFrame f;
            decodeFrame(data.data() + i + 5, f);
            out.frames.push_back(f);
            break;
        }
        case JournalRecord::StrokeEnd:
            if (len == 8) {
                out.lastCompleteSeq = getU64(data.data() + i + 5);
                out.completeCount = out.frames.size(); // 여기까지가 완성된 획이다
            }
            break;
        case JournalRecord::Note:
            out.notes.emplace_back(reinterpret_cast<const char*>(data.data() + i + 5), len);
            break;
        default:
            // 모르는 레코드 종류는 건너뛴다(앞으로 더해질 수 있다).
            break;
        }
        i += total;
    }
    return Ok(std::move(out));
}

Result<std::vector<StrokeFrame>> Journal::framesAfter(u64 afterSeq) {
    if (!flush()) {
        return Err("저널을 flush 하지 못했다", ErrorCode::IoError);
    }
    auto scanned = scan(path_);
    if (!scanned) {
        return scanned.error();
    }
    std::vector<StrokeFrame> out;
    for (const auto& f : scanned.value().frames) {
        if (f.seq > afterSeq) {
            out.push_back(f);
        }
    }
    return Ok(std::move(out));
}

} // namespace mari::sigan
