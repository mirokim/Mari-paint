// Mari Paint — Adobe ActionDescriptor 파서 테스트.
// 이 파서가 .abr 의 "동작까지 가져온다"를 떠받친다. 깨진 입력에서 죽지 않는 것이 요구사항이다.
#include <mari/io/brush/abr_descriptor.hpp>
#include <mari/test/harness.hpp>

#include "fixture_builder.hpp"

using namespace mari;
using namespace mari::io::brush;
namespace fx = mari::testfix;

namespace {

Result<DescriptorPtr> parseBytes(const std::vector<u8>& bytes) {
    ByteReader r(bytes.data(), bytes.size());
    return parseDescriptor(r);
}

} // namespace

MARI_TEST(descriptor_scalar_types) {
    const auto bytes = fx::descriptorBody("브러시", "Brsh",
                                          {{"Dmtr", fx::dvUnitFloat("#Pxl", 42.5)},
                                           {"Spcn", fx::dvUnitFloat("#Prc", 25.0)},
                                           {"Intr", fx::dvBool(true)},
                                           {"Cnt ", fx::dvLong(7)},
                                           {"Nm  ", fx::dvText("잉크펜")},
                                           {"Bld ", fx::dvEnum("BlnM", "Mltp")},
                                           {"dbl ", fx::dvDouble(1.25)}});
    auto result = parseBytes(bytes);
    CHECK(result.ok());
    const Descriptor& d = *result.value();
    CHECK_EQ(d.classId, std::string("Brsh"));
    CHECK_EQ(d.className, std::string("브러시"));
    CHECK_EQ(d.items.size(), usize(7));

    CHECK(d.find("Dmtr") != nullptr);
    CHECK_NEAR(d.find("Dmtr")->asNumber(), 42.5, 1e-9);
    CHECK_EQ(d.find("Dmtr")->unit, std::string("#Pxl"));
    CHECK(d.find("Intr")->asBool());
    CHECK_NEAR(d.find("Cnt ")->asNumber(), 7.0, 1e-9);
    CHECK_EQ(d.find("Nm  ")->text, std::string("잉크펜"));
    CHECK_EQ(d.find("Bld ")->enumValue, std::string("Mltp"));
    CHECK_NEAR(d.find("dbl ")->asNumber(), 1.25, 1e-9);
    CHECK(d.find("없는키") == nullptr);
}

MARI_TEST(descriptor_nested_and_list) {
    const auto inner = fx::dvObjc("", "Objc", {{"jitter", fx::dvUnitFloat("#Prc", 50.0)}});
    const auto bytes = fx::descriptorBody(
        "", "null",
        {{"szVr", inner},
         {"Brsh", fx::dvList({fx::dvObjc("", "Brsh", {{"Nm  ", fx::dvText("A")}}),
                              fx::dvObjc("", "Brsh", {{"Nm  ", fx::dvText("B")}})})}});
    auto result = parseBytes(bytes);
    CHECK(result.ok());
    const Descriptor& d = *result.value();

    const DescValue* var = d.find("szVr");
    CHECK(var != nullptr);
    CHECK(var->type == DescType::Descriptor);
    CHECK(var->descriptor != nullptr);
    CHECK_NEAR(var->descriptor->find("jitter")->asNumber(), 50.0, 1e-9);

    const DescValue* list = d.find("Brsh");
    CHECK(list != nullptr);
    CHECK(list->type == DescType::List);
    CHECK_EQ(list->list.size(), usize(2));
    CHECK_EQ(list->list[1].descriptor->find("Nm  ")->text, std::string("B"));

    // 키 경로 수집 — 임포터가 "안 읽은 키"를 찾는 데 쓴다.
    std::vector<std::string> paths;
    collectKeyPaths(d, "", paths);
    bool sawNested = false;
    bool sawListItem = false;
    for (const std::string& p : paths) {
        if (p == "szVr/jitter")
            sawNested = true;
        if (p == "Brsh[1]/Nm  ")
            sawListItem = true;
    }
    CHECK(sawNested);
    CHECK(sawListItem);
}

MARI_TEST(descriptor_unknown_ostype_is_an_error_not_a_crash) {
    fx::ByteWriter w;
    w.raw(fx::descriptorBody("", "null", {}));
    // 항목 수만 1 로 고치고 모르는 태그를 넣는다.
    std::vector<u8> bytes = w.bytes;
    bytes[bytes.size() - 1] = 1;
    fx::ByteWriter item;
    item.key("xxxx");
    item.ascii("ZZZZ"); // 존재하지 않는 OSType
    bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());

    auto result = parseBytes(bytes);
    CHECK(!result.ok());
    CHECK(result.code() == ErrorCode::ParseError);
    CHECK(result.message().find("ZZZZ") != std::string::npos);
}

MARI_TEST(descriptor_truncated_at_every_offset_never_crashes) {
    const auto full = fx::descriptorBody(
        "", "null",
        {{"Brsh", fx::dvList({fx::dvObjc("", "Brsh",
                                         {{"Nm  ", fx::dvText("펜")},
                                          {"Dmtr", fx::dvUnitFloat("#Pxl", 10.0)}})})}});
    for (usize cut = 0; cut < full.size(); ++cut) {
        std::vector<u8> partial(full.begin(), full.begin() + static_cast<long>(cut));
        auto result = parseBytes(partial);
        // 잘린 입력은 실패해야 한다. 크래시나 성공은 둘 다 버그다.
        CHECK(!result.ok());
    }
    CHECK(parseBytes(full).ok());
}

MARI_TEST(descriptor_absurd_item_count_is_rejected) {
    fx::ByteWriter w;
    w.unicodeString("");
    w.key("null");
    w.u32be(0xFFFFFFFFu); // 말도 안 되는 항목 수
    auto result = parseBytes(w.bytes);
    CHECK(!result.ok());
    CHECK(result.code() == ErrorCode::ParseError);
}

MARI_TEST(descriptor_deep_nesting_is_bounded) {
    // 64겹 중첩 — 파서의 깊이 상한(32)에 걸려 안전하게 실패해야 한다.
    fx::DescBytes value = fx::dvObjc("", "Objc", {});
    for (int i = 0; i < 64; ++i)
        value = fx::dvObjc("", "Objc", {{"deep", value}});
    const auto bytes = fx::descriptorBody("", "null", {{"root", value}});
    auto result = parseBytes(bytes);
    CHECK(!result.ok());
}

MARI_TEST(descriptor_section_version_check) {
    fx::ByteWriter w;
    w.u32be(99); // 모르는 섹션 버전
    w.raw(fx::descriptorBody("", "null", {}));
    ByteReader r(w.bytes.data(), w.bytes.size());
    auto bad = parseDescriptorSection(r);
    CHECK(!bad.ok());
    CHECK(bad.code() == ErrorCode::Unsupported);

    const auto good = fx::descSection({{"Nm  ", fx::dvText("좋다")}});
    ByteReader r2(good.data(), good.size());
    auto ok = parseDescriptorSection(r2);
    CHECK(ok.ok());
    CHECK_EQ(ok.value()->find("Nm  ")->text, std::string("좋다"));
}

MARI_TEST_MAIN()
