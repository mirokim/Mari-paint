// Mari Paint — PIPL 리소스 파서 테스트
//
// .8bf 는 신뢰할 수 없는 파일이다. 잘린 리소스·거짓말하는 길이 필드를 만나면
// **거절하거나 note 를 남기고 멈춰야** 한다. 조용히 읽어 넘기면 안 된다.
#include <mari/host8bf/pipl.hpp>
#include <mari/test/harness.hpp>

#include <cstring>

using namespace mari;
using namespace mari::host8bf;

namespace {

/// 테스트용 PiPL 리소스를 조립한다.
class PiplBuilder {
public:
    explicit PiplBuilder(u16 version = 0) : version_(version) {}

    void add(const char* vendor, const char* key, const std::vector<u8>& data) {
        props_.push_back({vendor, key, data});
    }
    void addCString(const char* vendor, const char* key, const char* s) {
        std::vector<u8> d(s, s + std::strlen(s) + 1);
        add(vendor, key, d);
    }
    void addPascal(const char* vendor, const char* key, const char* s) {
        const usize n = std::strlen(s);
        std::vector<u8> d;
        d.push_back(static_cast<u8>(n));
        d.insert(d.end(), s, s + n);
        add(vendor, key, d);
    }
    void addU32(const char* vendor, const char* key, u32 v) {
        std::vector<u8> d{static_cast<u8>(v & 0xFFu), static_cast<u8>((v >> 8) & 0xFFu),
                          static_cast<u8>((v >> 16) & 0xFFu), static_cast<u8>((v >> 24) & 0xFFu)};
        add(vendor, key, d);
    }

    [[nodiscard]] std::vector<u8> build() const {
        std::vector<u8> out;
        push16(out, version_);
        push16(out, static_cast<u16>(props_.size()));
        for (const Prop& p : props_) {
            out.insert(out.end(), p.vendor, p.vendor + 4);
            out.insert(out.end(), p.key, p.key + 4);
            push32(out, 0u);
            push32(out, static_cast<u32>(p.data.size()));
            out.insert(out.end(), p.data.begin(), p.data.end());
            while (out.size() % 4u != 0u) {
                out.push_back(0u);
            }
        }
        return out;
    }

private:
    struct Prop {
        const char* vendor;
        const char* key;
        std::vector<u8> data;
    };
    static void push16(std::vector<u8>& v, u16 x) {
        v.push_back(static_cast<u8>(x & 0xFFu));
        v.push_back(static_cast<u8>((x >> 8) & 0xFFu));
    }
    static void push32(std::vector<u8>& v, u32 x) {
        v.push_back(static_cast<u8>(x & 0xFFu));
        v.push_back(static_cast<u8>((x >> 8) & 0xFFu));
        v.push_back(static_cast<u8>((x >> 16) & 0xFFu));
        v.push_back(static_cast<u8>((x >> 24) & 0xFFu));
    }

    u16 version_;
    std::vector<Prop> props_;
};

} // namespace

MARI_TEST(parses_a_plain_pipl) {
    PiplBuilder b;
    b.addPascal("8BIM", "catg", "Mari Test");
    b.addPascal("8BIM", "Nm  ", "Gaussian Smudge");
    b.addCString("8BIM", "8664", "PluginMain");
    b.addU32("8BIM", "mode", (1u << 3) | (1u << 1)); // RGB + Gray

    const std::vector<u8> res = b.build();
    std::vector<std::string> notes;
    auto r = parsePipl(res.data(), res.size(), notes);
    CHECK(r.ok());
    CHECK_EQ(r.value().size(), static_cast<usize>(4));
    CHECK_EQ(r.value()[0].key, std::string("catg"));
    CHECK_EQ(r.value()[0].vendorId, std::string("8BIM"));

    auto e = piplToEntry(r.value(), notes);
    CHECK(e.ok());
    CHECK_EQ(e.value().category, std::string("Mari Test"));
    CHECK_EQ(e.value().name, std::string("Gaussian Smudge"));
    CHECK_EQ(e.value().entryName, std::string("PluginMain"));
    CHECK(e.value().is64Bit);
    CHECK(e.value().supportsRGB);
    CHECK(e.value().supportsGray);
}

MARI_TEST(x86_only_plugin_is_32bit) {
    PiplBuilder b;
    b.addPascal("8BIM", "Nm  ", "Old Filter");
    b.addCString("8BIM", "wx86", "ENTRYPOINT");
    const std::vector<u8> res = b.build();
    std::vector<std::string> notes;
    auto props = parsePipl(res.data(), res.size(), notes).value();
    auto e = piplToEntry(props, notes);
    CHECK(e.ok());
    CHECK(!e.value().is64Bit);
    CHECK_EQ(e.value().entryName, std::string("ENTRYPOINT"));

    // 🔴 x86 플러그인을 x64 호스트로 보내면 안 된다. 그래서 호스트가 두 벌이다.
    CHECK(entryMatchesHost(e.value(), false));
    CHECK(!entryMatchesHost(e.value(), true));
}

MARI_TEST(x64_wins_when_both_present) {
    // 요즘 플러그인은 wx86 과 8664 를 둘 다 광고한다. x64 호스트면 8664 를 쓴다.
    PiplBuilder b;
    b.addCString("8BIM", "wx86", "PluginMain32");
    b.addCString("8BIM", "8664", "PluginMain64");
    const std::vector<u8> res = b.build();
    std::vector<std::string> notes;
    auto props = parsePipl(res.data(), res.size(), notes).value();
    auto e = piplToEntry(props, notes);
    CHECK(e.ok());
    CHECK(e.value().is64Bit);
    CHECK_EQ(e.value().entryName, std::string("PluginMain64"));
}

MARI_TEST(mac_only_plugin_is_refused) {
    // 코드 프로퍼티가 없으면 Windows 에서 돌릴 수 없다. 정직하게 거절한다.
    PiplBuilder b;
    b.addPascal("8BIM", "Nm  ", "Mac Only");
    b.addCString("8BIM", "mi32", "whatever");
    const std::vector<u8> res = b.build();
    std::vector<std::string> notes;
    auto props = parsePipl(res.data(), res.size(), notes).value();
    auto e = piplToEntry(props, notes);
    CHECK(!e.ok());
    CHECK_EQ(e.code(), ErrorCode::Unsupported);
}

MARI_TEST(unknown_properties_are_reported_not_dropped) {
    // 🔴 docs/02 5절: 번역 못 한 것은 조용히 버리지 않는다.
    PiplBuilder b;
    b.addCString("8BIM", "8664", "PluginMain");
    b.addU32("8BIM", "zzzz", 1u);
    b.addU32("8BIM", "qqqq", 2u);
    const std::vector<u8> res = b.build();
    std::vector<std::string> notes;
    auto props = parsePipl(res.data(), res.size(), notes).value();
    auto e = piplToEntry(props, notes);
    CHECK(e.ok());
    // 모르는 키는 **이름을 그대로** 리포트에 남긴다. 사용자가 뭐가 빠졌는지 알아야 한다.
    usize named = 0;
    for (const std::string& n : notes) {
        if (n.find("zzzz") != std::string::npos || n.find("qqqq") != std::string::npos) {
            ++named;
        }
    }
    CHECK_EQ(named, static_cast<usize>(2));
    // 이름을 못 읽은 것도 따로 남는다(여기서는 'Nm  ' 를 안 넣었다).
    CHECK(notes.size() >= 3);
}

MARI_TEST(truncated_resource_is_rejected) {
    PiplBuilder b;
    b.addCString("8BIM", "8664", "PluginMain");
    b.addPascal("8BIM", "Nm  ", "Something");
    std::vector<u8> res = b.build();

    // 리소스 꼬리를 자른다 — 손상된 파일이거나 공격이다.
    res.resize(res.size() - 6);
    std::vector<std::string> notes;
    auto r = parsePipl(res.data(), res.size(), notes);
    // 첫 프로퍼티는 읽히므로 성공하되, **잘렸다는 사실이 note 에 남아야 한다.**
    CHECK(r.ok());
    CHECK(!notes.empty());
}

MARI_TEST(lying_length_field_cannot_read_past_buffer) {
    // 길이 필드가 리소스 크기를 넘는다고 주장한다. 따라가면 버퍼 밖을 읽는다.
    std::vector<u8> res;
    auto push16 = [&res](u16 x) {
        res.push_back(static_cast<u8>(x & 0xFFu));
        res.push_back(static_cast<u8>(x >> 8));
    };
    auto push32 = [&res](u32 x) {
        for (int i = 0; i < 4; ++i) {
            res.push_back(static_cast<u8>((x >> (8 * i)) & 0xFFu));
        }
    };
    push16(0);   // version
    push16(1);   // count
    const char* v = "8BIM";
    const char* k = "Nm  ";
    res.insert(res.end(), v, v + 4);
    res.insert(res.end(), k, k + 4);
    push32(0);
    push32(0x7FFFFFFFu); // 거짓말하는 길이
    res.push_back('X');

    std::vector<std::string> notes;
    auto r = parsePipl(res.data(), res.size(), notes);
    // 읽어 낸 게 없으니 실패해야 한다. 절대 크래시하면 안 된다.
    CHECK(!r.ok());
    CHECK(!notes.empty());
}

MARI_TEST(empty_and_tiny_inputs) {
    std::vector<std::string> notes;
    CHECK(!parsePipl(nullptr, 0, notes).ok());
    const u8 tiny[2] = {0, 0};
    CHECK(!parsePipl(tiny, 2, notes).ok());

    // count == 0
    const u8 zero[4] = {0, 0, 0, 0};
    CHECK(!parsePipl(zero, 4, notes).ok());
}

MARI_TEST(count_larger_than_resource_is_rejected) {
    // "프로퍼티 1000개 있다" 고 주장하는 12바이트 리소스.
    std::vector<u8> res = {0, 0, 0xE8, 0x03};
    res.resize(12, 0);
    std::vector<std::string> notes;
    CHECK(!parsePipl(res.data(), res.size(), notes).ok());
}

MARI_TEST(pascal_and_cstring_names_both_work) {
    // 두 방식이 현장에 섞여 있다. 둘 다 읽어야 한다.
    CHECK_EQ(pstrOrCstrFrom({5, 'H', 'e', 'l', 'l', 'o'}), std::string("Hello"));
    CHECK_EQ(pstrOrCstrFrom({'H', 'i', 0}), std::string("Hi"));
    CHECK_EQ(pstrOrCstrFrom({}), std::string());
    // 널이 없어도 버퍼 밖으로 나가지 않는다.
    CHECK_EQ(cstrFrom({'A', 'B', 'C'}), std::string("ABC"));
}

MARI_TEST(unknown_version_is_noted_but_parsed) {
    PiplBuilder b(7);
    b.addCString("8BIM", "8664", "PluginMain");
    const std::vector<u8> res = b.build();
    std::vector<std::string> notes;
    auto r = parsePipl(res.data(), res.size(), notes);
    CHECK(r.ok());
    CHECK(!notes.empty()); // 모르는 버전이라고 말은 한다
}

MARI_TEST_MAIN()
