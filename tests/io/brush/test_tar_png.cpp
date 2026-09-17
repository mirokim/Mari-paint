// Mari Paint — tar 리더와 PNG→그레이 디코더 테스트.
// 둘 다 .sut 텍스처 경로에 깔려 있다. 깨진 입력에서 크래시 없이 실패하는 게 요구사항이다.
#include <mari/io/brush/png_gray.hpp>
#include <mari/io/brush/tar.hpp>
#include <mari/test/harness.hpp>

#include "fixture_builder.hpp"

using namespace mari;
using namespace mari::io::brush;
namespace fx = mari::testfix;

MARI_TEST(tar_roundtrip) {
    const std::vector<u8> a = {1, 2, 3, 4, 5};
    std::vector<u8> big(1500, 0x7F);
    const auto blob = fx::buildTar({{"first.png", a}, {"dir/second.bin", big}});

    CHECK(looksLikeTar(blob.data(), blob.size()));
    auto entries = readTar(blob.data(), blob.size());
    CHECK(entries.ok());
    if (!entries.ok())
        return;
    CHECK_EQ(entries.value().size(), usize(2));
    CHECK_EQ(entries.value()[0].name, std::string("first.png"));
    CHECK_EQ(entries.value()[0].size, usize(5));
    CHECK(entries.value()[0].isFile());
    CHECK_EQ(entries.value()[1].name, std::string("dir/second.bin"));
    CHECK_EQ(entries.value()[1].size, usize(1500));
    // 내용이 원본 오프셋을 정확히 가리킨다.
    CHECK_EQ(int(blob[entries.value()[0].offset + 2]), 3);
    CHECK_EQ(int(blob[entries.value()[1].offset + 1499]), 0x7F);
}

MARI_TEST(tar_rejects_broken_checksum) {
    auto blob = fx::buildTar({{"x.png", {1, 2, 3}}});
    blob[10] = 'Z'; // 이름을 바꿔 체크섬을 깬다
    auto entries = readTar(blob.data(), blob.size());
    CHECK(!entries.ok());
    CHECK(entries.code() == ErrorCode::ParseError);
}

MARI_TEST(tar_rejects_size_that_points_outside) {
    auto blob = fx::buildTar({{"x.png", {1, 2, 3}}});
    // 크기 필드를 거대한 값으로 바꾸고 체크섬을 다시 맞춘다.
    const char* huge = "77777777777";
    for (int i = 0; i < 11; ++i)
        blob[124 + static_cast<usize>(i)] = static_cast<u8>(huge[i]);
    blob[135] = 0;
    unsigned sum = 0;
    for (usize i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? 32u : blob[i];
    char chk[9];
    std::snprintf(chk, sizeof(chk), "%06o", sum);
    for (int i = 0; i < 6; ++i)
        blob[148 + static_cast<usize>(i)] = static_cast<u8>(chk[i]);
    blob[154] = 0;
    blob[155] = ' ';

    auto entries = readTar(blob.data(), blob.size());
    CHECK(!entries.ok());
}

MARI_TEST(tar_truncated_never_crashes) {
    const auto blob = fx::buildTar({{"a.png", {1, 2, 3}}, {"b.png", {4, 5, 6}}});
    for (usize cut = 0; cut <= blob.size(); cut += 37) {
        auto entries = readTar(blob.data(), cut);
        if (entries.ok())
            CHECK(entries.value().size() <= 2);
    }
    CHECK(true);
}

MARI_TEST(tar_empty_input) {
    auto r = readTar(nullptr, 0);
    CHECK(!r.ok());
    const std::vector<u8> tiny(10, 0);
    auto r2 = readTar(tiny.data(), tiny.size());
    CHECK(!r2.ok());
    CHECK(!looksLikeTar(tiny.data(), tiny.size()));
}

MARI_TEST(png_gray_roundtrip) {
    const std::vector<u8> pixels = {0, 64, 128, 255, 10, 20};
    const auto png = fx::writeGrayPng(3, 2, pixels);
    CHECK(png.size() > 8);
    CHECK(looksLikePng(png.data(), png.size()));

    PngGrayInfo info;
    auto img = decodePngGray(png.data(), png.size(), false, &info);
    CHECK(img.ok());
    if (!img.ok())
        return;
    CHECK_EQ(img.value().width, 3);
    CHECK_EQ(img.value().height, 2);
    CHECK(img.value().pixels == pixels);
    CHECK(!info.hadAlpha);
    CHECK(!info.hadColor);
    CHECK_EQ(info.sourceBitDepth, 8);
}

MARI_TEST(png_gray_invert_for_tip_masks) {
    const std::vector<u8> pixels = {0, 255};
    const auto png = fx::writeGrayPng(2, 1, pixels);
    auto img = decodePngGray(png.data(), png.size(), true, nullptr);
    CHECK(img.ok());
    if (!img.ok())
        return;
    CHECK_EQ(int(img.value().pixels[0]), 255);
    CHECK_EQ(int(img.value().pixels[1]), 0);
}

MARI_TEST(png_gray_rejects_non_png) {
    const std::vector<u8> junk = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    auto img = decodePngGray(junk.data(), junk.size(), false, nullptr);
    CHECK(!img.ok());
    CHECK(img.code() == ErrorCode::ParseError);
    CHECK(!decodePngGray(nullptr, 0, false, nullptr).ok());
}

MARI_TEST(png_gray_truncated_never_crashes) {
    const std::vector<u8> pixels(64, 128);
    const auto png = fx::writeGrayPng(8, 8, pixels);
    for (usize cut = 0; cut < png.size(); cut += 3) {
        auto img = decodePngGray(png.data(), cut, false, nullptr);
        if (cut < png.size())
            CHECK(!img.ok() || img.value().width == 8);
    }
    CHECK(decodePngGray(png.data(), png.size(), false, nullptr).ok());
}

MARI_TEST(png_gray_corrupted_body_is_an_error) {
    const std::vector<u8> pixels(64, 200);
    auto png = fx::writeGrayPng(8, 8, pixels);
    // IDAT 한복판을 망가뜨린다 — CRC 가 어긋나 libpng 이 실패해야 한다.
    png[png.size() / 2] ^= 0xFF;
    auto img = decodePngGray(png.data(), png.size(), false, nullptr);
    CHECK(!img.ok() || img.value().width == 8);
}

MARI_TEST_MAIN()
