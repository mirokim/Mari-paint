// Mari Paint — 스트로크 프레임 인코딩 (docs/03 4.1)
#include <mari/sigan/frame.hpp>

#include <bit>

namespace mari::sigan {
namespace {

// 리틀엔디안 고정. 호스트 엔디안을 믿지 않는다 — 바이트를 직접 쌓는다.
inline void putU32(u8*& p, u32 v) noexcept {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
    p[2] = static_cast<u8>((v >> 16) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24) & 0xFFu);
    p += 4;
}

inline void putU64(u8*& p, u64 v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>((v >> (8 * i)) & 0xFFull);
    }
    p += 8;
}

inline void putF32(u8*& p, f32 v) noexcept { putU32(p, std::bit_cast<u32>(v)); }
inline void putF64(u8*& p, f64 v) noexcept { putU64(p, std::bit_cast<u64>(v)); }

inline u32 getU32(const u8*& p) noexcept {
    const u32 v = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                  (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
    p += 4;
    return v;
}

inline u64 getU64(const u8*& p) noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(p[i]) << (8 * i);
    }
    p += 8;
    return v;
}

inline f32 getF32(const u8*& p) noexcept { return std::bit_cast<f32>(getU32(p)); }
inline f64 getF64(const u8*& p) noexcept { return std::bit_cast<f64>(getU64(p)); }

/// CRC-32 테이블(IEEE 802.3 역다항식 0xEDB88320). 컴파일 타임에 만든다.
struct Crc32Table {
    u32 v[256]{};
    constexpr Crc32Table() {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};
constexpr Crc32Table kCrcTable{};

} // namespace

void encodeFrame(const StrokeFrame& in, u8* out) noexcept {
    u8* p = out;
    putU64(p, in.seq);
    putF64(p, in.tMs);
    putF32(p, in.cx);
    putF32(p, in.cy);
    putF32(p, in.pressure);
    putF32(p, in.tiltX);
    putF32(p, in.tiltY);
    putF32(p, in.rotation);
    putF32(p, in.velocity);
    putU32(p, in.layerId);
    putU32(p, in.brushId);
    putU32(p, in.flags);
}

void decodeFrame(const u8* in, StrokeFrame& out) noexcept {
    const u8* p = in;
    out.seq = getU64(p);
    out.tMs = getF64(p);
    out.cx = getF32(p);
    out.cy = getF32(p);
    out.pressure = getF32(p);
    out.tiltX = getF32(p);
    out.tiltY = getF32(p);
    out.rotation = getF32(p);
    out.velocity = getF32(p);
    out.layerId = getU32(p);
    out.brushId = getU32(p);
    out.flags = getU32(p);
}

u32 narrowId(u64 id, bool& truncated) noexcept {
    if (id > 0xFFFFFFFFull) {
        truncated = true;
    }
    return static_cast<u32>(id & 0xFFFFFFFFull);
}

u32 crc32(const u8* data, usize len, u32 seed) noexcept {
    u32 c = ~seed;
    for (usize i = 0; i < len; ++i) {
        c = kCrcTable.v[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

} // namespace mari::sigan
