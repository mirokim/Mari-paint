// Mari Paint — SHA-256 검증 (FIPS 180-4 / NIST 공식 테스트 벡터)
//
// 🔴 직접 구현한 암호 프리미티브는 **공식 벡터로 못 박지 않으면 쓰면 안 된다.**
//    아래 기대값은 FIPS 180-2 부록 B 와 NIST CAVP SHA 예제에서 온 것이다.
#include <mari/crypto/sha256.hpp>
#include <mari/test/harness.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace mari;
using namespace mari::crypto;

namespace {

/// 한 방 해시의 16진 문자열.
std::string hexOf(const std::string& s) { return sha256Hex(s); }

/// 파일을 통째로 읽는다(경계선 검사용). 없으면 빈 문자열.
std::string readWholeFile(const std::filesystem::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr) {
        return std::string{};
    }
    std::string out;
    char buf[4096];
    usize got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, got);
    }
    (void)std::fclose(f);
    return out;
}

/// 주석을 걷어낸다. 경계선 검사는 **코드**를 봐야 한다 —
/// "ES256 은 넣지 않는다" 같은 설명문까지 금지어로 잡으면 주석을 못 쓴다.
std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (usize i = 0; i < src.size();) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') {
                ++i;
            }
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                ++i;
            }
            i = i + 2 < src.size() ? i + 2 : src.size();
        } else {
            out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

} // namespace

// ── NIST 공식 벡터 ───────────────────────────────────────────────────────

MARI_TEST(nist_empty_message) {
    // 빈 메시지 — 패딩 경로만으로 한 블록을 만드는 경계다.
    CHECK_EQ(hexOf(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

MARI_TEST(nist_abc) {
    // FIPS 180-2 B.1 — 한 블록 메시지.
    CHECK_EQ(hexOf("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

MARI_TEST(nist_448_bit_message) {
    // FIPS 180-2 B.2 — 56바이트. 패딩 + 길이가 **같은 블록에 못 들어가는** 경계다.
    CHECK_EQ(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

MARI_TEST(nist_896_bit_multi_block) {
    // FIPS 180-2 B.3 변형 — 112바이트, 다중 블록.
    CHECK_EQ(hexOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                   "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
             std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

MARI_TEST(nist_one_million_a) {
    // FIPS 180-2 B.3 — 'a' 100만 개. 스트리밍이 실제로 필요한 유일한 벡터다.
    Sha256 h;
    const std::vector<u8> chunk(1000, static_cast<u8>('a'));
    for (int i = 0; i < 1000; ++i) {
        h.update(chunk.data(), chunk.size());
    }
    CHECK_EQ(h.byteCount(), static_cast<u64>(1000000));
    CHECK_EQ(toHex(h.finish()),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// ── 스트리밍 규약 ────────────────────────────────────────────────────────

MARI_TEST(streaming_matches_one_shot_at_every_split) {
    // 어디서 잘라 먹여도 같은 값이 나와야 한다. 블록 경계(64) 앞뒤를 전부 훑는다.
    std::string msg;
    for (int i = 0; i < 200; ++i) {
        msg.push_back(static_cast<char>('A' + (i % 26)));
    }
    const std::string oneShot = sha256Hex(msg);
    for (usize cut = 0; cut <= msg.size(); ++cut) {
        Sha256 h;
        h.update(msg.data(), cut);
        h.update(msg.data() + cut, msg.size() - cut);
        CHECK_EQ(toHex(h.finish()), oneShot);
    }
}

MARI_TEST(streaming_honors_block_boundaries) {
    // 55/56/63/64/65 바이트 — 패딩 분기가 갈리는 지점들.
    for (const usize n : {usize{55}, usize{56}, usize{57}, usize{63}, usize{64}, usize{65},
                          usize{119}, usize{120}, usize{128}}) {
        std::vector<u8> data(n);
        for (usize i = 0; i < n; ++i) {
            data[i] = static_cast<u8>(i * 7u + 3u);
        }
        const std::string whole = sha256Hex(data.data(), n);
        Sha256 h;
        for (usize i = 0; i < n; ++i) {
            h.update(data.data() + i, 1); // 한 바이트씩
        }
        CHECK_EQ(toHex(h.finish()), whole);
    }
}

MARI_TEST(reset_makes_the_object_reusable) {
    Sha256 h;
    h.update("abc");
    const std::string first = toHex(h.finish());
    CHECK(h.finished());
    // finish() 뒤의 update() 는 값을 바꾸지 못한다 — 실수로 이어 먹이는 걸 막는다.
    h.update("xyz");
    CHECK_EQ(toHex(h.finish()), first);

    h.reset();
    CHECK(!h.finished());
    h.update("abc");
    CHECK_EQ(toHex(h.finish()), first);
}

MARI_TEST(digest_is_big_endian_like_sha256sum) {
    // 다이제스트 첫 바이트가 16진 앞 두 자리다. 순서가 뒤집히면 여기서 걸린다.
    const Sha256Digest d = sha256("abc", 3);
    CHECK_EQ(static_cast<int>(d[0]), 0xba);
    CHECK_EQ(static_cast<int>(d[1]), 0x78);
    CHECK_EQ(static_cast<int>(d[kSha256DigestSize - 1]), 0xad);
}

// ── 파일 해시 ────────────────────────────────────────────────────────────

MARI_TEST(file_hash_has_no_domain_prefix) {
    // 🔴 파일 해시는 **파일 바이트 그대로**다. sha256sum 과 값이 같아야 한다 —
    //    도메인 접두사를 붙이면 아무도 대조할 수 없게 된다.
    const std::string path = "mari_test_sha256_file.bin";
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            const char* body = "abc";
            (void)std::fwrite(body, 1, 3, f);
            (void)std::fclose(f);
        }
    }
    const Result<std::string> r = sha256FileHex(path);
    CHECK(r.ok());
    if (r.ok()) {
        CHECK_EQ(r.value(),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }
    (void)std::remove(path.c_str());
}

MARI_TEST(file_hash_reports_missing_file) {
    const Result<std::string> r = sha256FileHex("mari_test_does_not_exist_12345.bin");
    CHECK(!r.ok());
    if (!r.ok()) {
        CHECK(r.code() == ErrorCode::IoError);
    }
}

MARI_TEST(large_file_streams_in_chunks) {
    // 64KiB 청크 경계를 넘는 파일. 청크 경계에서 값이 틀어지면 여기서 잡힌다.
    const std::string path = "mari_test_sha256_large.bin";
    std::vector<u8> body(200u * 1024u);
    for (usize i = 0; i < body.size(); ++i) {
        body[i] = static_cast<u8>((i * 31u + 17u) & 0xFFu);
    }
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            (void)std::fwrite(body.data(), 1, body.size(), f);
            (void)std::fclose(f);
        }
    }
    const Result<std::string> r = sha256FileHex(path);
    CHECK(r.ok());
    if (r.ok()) {
        CHECK_EQ(r.value(), sha256Hex(body.data(), body.size()));
    }
    (void)std::remove(path.c_str());
}

// ── 경계선 (docs/03 2절) ─────────────────────────────────────────────────

MARI_TEST(crypto_headers_have_no_chain_or_signature_api) {
    // 🔴 Mari 는 증명하지 않는다. 해시 *계산*은 하되 체인 *봉인*·서명은 하지 않는다.
    //    헤더에 prevHash/sign/ES256 같은 식별자가 생기면 설계 위반이다.
    const std::filesystem::path root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const char* rels[] = {"include/mari/crypto/sha256.hpp", "include/mari/crypto/canvas_hash.hpp"};
    const char* banned[] = {"prevHash", "prev_hash", "chainHash", "sealChain",
                            "ES256",    "ecdsa",     "signDigest"};
    for (const char* rel : rels) {
        const std::string text = stripComments(readWholeFile(root / rel));
        CHECK(!text.empty()); // 헤더를 못 찾으면 검사 자체가 무의미하다
        for (const char* bad : banned) {
            if (text.find(bad) != std::string::npos) {
                mari_ctx.fail(__FILE__, __LINE__,
                              std::string(rel) + " 에 금지 식별자가 있다: " + bad);
            }
        }
    }
}

MARI_TEST_MAIN()
