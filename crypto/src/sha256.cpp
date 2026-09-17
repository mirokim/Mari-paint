// Mari Paint — SHA-256 구현 (FIPS 180-4 §6.2)
//
// 상수·의사코드는 전부 규격에서 온 것이다. 최적화하지 않았다 —
// 여기서 노리는 것은 속도가 아니라 **NIST 벡터와 한 비트도 안 틀리는 것**이다.
#include <mari/crypto/sha256.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mari::crypto {
namespace {

/// FIPS 180-4 §4.2.2 — 처음 64개 소수의 세제곱근 소수부 32비트.
constexpr u32 kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

/// FIPS 180-4 §5.3.3 — 처음 8개 소수의 제곱근 소수부 32비트.
constexpr u32 kInit[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

[[nodiscard]] constexpr u32 rotr(u32 x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}
[[nodiscard]] constexpr u32 ch(u32 x, u32 y, u32 z) noexcept { return (x & y) ^ (~x & z); }
[[nodiscard]] constexpr u32 maj(u32 x, u32 y, u32 z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}
[[nodiscard]] constexpr u32 bigSigma0(u32 x) noexcept {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
[[nodiscard]] constexpr u32 bigSigma1(u32 x) noexcept {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
[[nodiscard]] constexpr u32 smallSigma0(u32 x) noexcept {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
[[nodiscard]] constexpr u32 smallSigma1(u32 x) noexcept {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

/// 빅엔디안 4바이트 읽기. 호스트 엔디안에 의존하지 않는다.
[[nodiscard]] constexpr u32 beLoad32(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

constexpr void beStore32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>((v >> 24) & 0xFFu);
    p[1] = static_cast<u8>((v >> 16) & 0xFFu);
    p[2] = static_cast<u8>((v >> 8) & 0xFFu);
    p[3] = static_cast<u8>(v & 0xFFu);
}

} // namespace

void Sha256::reset() noexcept {
    for (usize i = 0; i < 8; ++i) {
        h_[i] = kInit[i];
    }
    std::memset(buf_, 0, sizeof(buf_));
    bufLen_ = 0;
    total_ = 0;
    finished_ = false;
    digest_.fill(0);
}

void Sha256::compress(const u8* block) noexcept {
    // §6.2.2 1단계 — 메시지 스케줄 W[0..63].
    u32 w[64];
    for (usize t = 0; t < 16; ++t) {
        w[t] = beLoad32(block + t * 4);
    }
    for (usize t = 16; t < 64; ++t) {
        w[t] = smallSigma1(w[t - 2]) + w[t - 7] + smallSigma0(w[t - 15]) + w[t - 16];
    }

    // 2단계 — 작업 변수.
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    u32 e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

    // 3단계 — 64 라운드.
    for (usize t = 0; t < 64; ++t) {
        const u32 t1 = hh + bigSigma1(e) + ch(e, f, g) + kK[t] + w[t];
        const u32 t2 = bigSigma0(a) + maj(a, b, c);
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    // 4단계 — 중간 해시 갱신.
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
}

void Sha256::update(const void* data, usize len) noexcept {
    if (finished_ || data == nullptr || len == 0) {
        return;
    }
    const u8* p = static_cast<const u8*>(data);
    total_ += static_cast<u64>(len);

    // 꼬리 버퍼를 먼저 채운다.
    if (bufLen_ > 0) {
        const usize need = kSha256BlockSize - bufLen_;
        const usize take = len < need ? len : need;
        std::memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ < kSha256BlockSize) {
            return; // 아직 한 블록이 안 된다
        }
        compress(buf_);
        bufLen_ = 0;
    }

    // 통짜 블록들. 복사하지 않고 그 자리에서 먹인다.
    while (len >= kSha256BlockSize) {
        compress(p);
        p += kSha256BlockSize;
        len -= kSha256BlockSize;
    }

    // 남은 꼬리를 보관한다.
    if (len > 0) {
        std::memcpy(buf_, p, len);
        bufLen_ = len;
    }
}

Sha256Digest Sha256::finish() noexcept {
    if (finished_) {
        return digest_;
    }
    // §5.1.1 패딩: 0x80, 0x00.., 그리고 **비트 길이 64비트 빅엔디안**.
    const u64 bitLen = total_ * 8u;
    u8 pad[kSha256BlockSize * 2]{};
    pad[0] = 0x80u;
    // 패딩 뒤 남는 자리가 8바이트 미만이면 블록 하나를 더 쓴다.
    const usize rem = bufLen_ + 1; // 0x80 까지 포함한 길이
    const usize padZeros = (rem <= kSha256BlockSize - 8)
                               ? (kSha256BlockSize - 8 - rem)
                               : (kSha256BlockSize * 2 - 8 - rem);
    const usize tailLen = 1 + padZeros + 8;
    for (usize i = 0; i < 8; ++i) {
        pad[tailLen - 8 + i] = static_cast<u8>((bitLen >> (56u - 8u * i)) & 0xFFu);
    }
    // update() 는 total_ 를 늘리지만 여기서는 이미 길이를 굳혔으므로 직접 먹인다.
    const u64 saved = total_;
    update(pad, tailLen);
    total_ = saved;

    for (usize i = 0; i < 8; ++i) {
        beStore32(digest_.data() + i * 4, h_[i]);
    }
    finished_ = true;
    return digest_;
}

std::string toHex(const Sha256Digest& d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(kSha256DigestSize * 2);
    for (usize i = 0; i < kSha256DigestSize; ++i) {
        out[i * 2] = kHex[(d[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = kHex[d[i] & 0x0Fu];
    }
    return out;
}

Sha256Digest sha256(const void* data, usize len) noexcept {
    Sha256 h;
    h.update(data, len);
    return h.finish();
}

std::string sha256Hex(const void* data, usize len) { return toHex(sha256(data, len)); }

Result<std::string> sha256FileHex(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return Err("파일을 열 수 없다: " + path, ErrorCode::IoError);
    }
    Sha256 h;
    // 64KiB 씩만 들고 있는다. 큰 .ora 를 통째로 올리지 않는다.
    static constexpr usize kChunk = 64u * 1024u;
    std::vector<u8> buf(kChunk);
    for (;;) {
        const usize got = std::fread(buf.data(), 1, kChunk, f);
        if (got > 0) {
            h.update(buf.data(), got);
        }
        if (got < kChunk) {
            const bool bad = std::ferror(f) != 0;
            std::fclose(f);
            if (bad) {
                return Err("파일 읽기 실패: " + path, ErrorCode::IoError);
            }
            break;
        }
    }
    return Ok(toHex(h.finish()));
}

} // namespace mari::crypto
