// Mari Paint — SHA-256 (FIPS 180-4). 외부 의존성 없음.
//
// 🔴 경계선(docs/03 2절) — 이 파일이 하는 일과 안 하는 일
//    ✅ 해시 **계산**은 Mari 의 몫이다. 픽셀 스냅샷·저장 파일의 SHA-256 을 낸다.
//    ❌ **해시체인 봉인**은 Sigan 의 몫이다. prevHash 를 엮어 체인을 만드는 코드,
//       기기 키 서명(ES256), 신뢰 등급 판정은 **이 리포에 들어오지 않는다.**
//       여기 있는 것은 "바이트 뭉치 하나 → 32바이트" 단발 함수뿐이다.
//       체인을 만들고 싶어지면 그 코드는 Sigan 쪽에 있어야 한다.
//
// 왜 직접 구현했나: 리포에 암호 라이브러리 의존이 없고(zlib·sqlite3·libpng 뿐),
// SHA-256 은 FIPS 180-4 에 완전히 적혀 있는 200줄짜리다. 의존을 하나 더 끌어오는 것보다
// NIST 공식 테스트 벡터로 못 박는 편이 싸다(tests/crypto/test_sha256.cpp).
//
// 스트리밍이 기본이다. 8K 캔버스를 한 번에 메모리에 올리지 않으려면 update/finish 가
// 필요하다 — 한 방 API 만 두면 호출자가 반드시 통짜 버퍼를 만들게 된다.
#ifndef MARI_CRYPTO_SHA256_HPP
#define MARI_CRYPTO_SHA256_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <array>
#include <string>
#include <string_view>

namespace mari::crypto {

/// 다이제스트 바이트 수. SHA-256 이므로 32 고정이다.
inline constexpr usize kSha256DigestSize = 32;
/// 압축 함수 블록 크기(바이트).
inline constexpr usize kSha256BlockSize = 64;

/// 32바이트 다이제스트. **빅엔디안 바이트 순서**다(FIPS 180-4 가 정한 대로).
/// 즉 toHex() 결과가 표준 `sha256sum` 출력과 같은 순서로 나온다.
using Sha256Digest = std::array<u8, kSha256DigestSize>;

/// 다이제스트를 소문자 16진 64자로. COM 브리지의 `canvasHash()` 가 이 형식을 돌려준다.
[[nodiscard]] std::string toHex(const Sha256Digest& d);

/// 스트리밍 SHA-256.
///
/// 쓰는 법:
///     Sha256 h;
///     h.update(part1.data(), part1.size());
///     h.update(part2.data(), part2.size());
///     const std::string hex = toHex(h.finish());
///
/// 규약:
///   · `finish()` 는 **한 번만** 부른다. 다시 쓰려면 `reset()`.
///     finish() 뒤에 update() 를 부르면 아무 일도 하지 않는다(조용히 무시하지 않고
///     finished() 로 확인할 수 있게 상태를 남긴다).
///   · 할당하지 않는다. 예외를 던지지 않는다. 핫 패스에서 불려도 된다.
class Sha256 {
public:
    Sha256() noexcept { reset(); }

    /// 초기 상태로 되돌린다. 같은 객체를 여러 번 쓸 때 부른다.
    void reset() noexcept;

    /// 바이트를 먹인다. 몇 번에 나눠 부르든 결과는 이어붙인 것과 같다.
    void update(const void* data, usize len) noexcept;
    void update(std::string_view s) noexcept { update(s.data(), s.size()); }

    /// 패딩·길이를 붙이고 다이제스트를 낸다. 두 번째 호출부터는 같은 값을 그대로 돌려준다.
    [[nodiscard]] Sha256Digest finish() noexcept;

    /// 지금까지 먹인 바이트 수.
    [[nodiscard]] u64 byteCount() const noexcept { return total_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    /// 64바이트 블록 하나를 압축한다(FIPS 180-4 6.2.2).
    void compress(const u8* block) noexcept;

    u32 h_[8]{};                  ///< 작업 변수 H0..H7
    u8 buf_[kSha256BlockSize]{};  ///< 64바이트가 안 찬 꼬리
    usize bufLen_ = 0;
    u64 total_ = 0;               ///< 먹인 총 바이트 수(패딩의 길이 필드가 된다)
    bool finished_ = false;
    Sha256Digest digest_{};       ///< finish() 결과 보관 — 재호출 시 그대로 준다
};

// ── 한 방 helper ─────────────────────────────────────────────────────────

[[nodiscard]] Sha256Digest sha256(const void* data, usize len) noexcept;
[[nodiscard]] std::string sha256Hex(const void* data, usize len);
[[nodiscard]] inline std::string sha256Hex(std::string_view s) {
    return sha256Hex(s.data(), s.size());
}

// ── 파일 해시 (IDL 의 Outputs · OnDocumentSaved 용) ───────────────────────

/// 파일 내용의 SHA-256(소문자 16진).
///
/// 🔴 바이트 순서 규약: **파일 바이트를 처음부터 끝까지 그대로** 먹인다.
///    도메인 접두사도, 길이 헤더도, 경로도 섞지 않는다 —
///    그래야 `sha256sum <파일>` 과 값이 **정확히 같고**, Sigan 이든 사람이든
///    표준 도구로 재계산해서 대조할 수 있다.
///    (캔버스/레이어 해시는 반대다. 거기는 도메인 접두사가 붙는다 — canvas_hash.hpp 참고.)
///
/// 파일을 통째로 올리지 않는다. 64KiB 씩 스트리밍한다.
[[nodiscard]] Result<std::string> sha256FileHex(const std::string& path);

} // namespace mari::crypto

#endif // MARI_CRYPTO_SHA256_HPP
