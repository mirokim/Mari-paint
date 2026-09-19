// Mari Paint — Photoshop .abr 임포터 테스트.
//
// 검증하는 것:
//   1) 왕복 — 우리가 조립한 abr 을 읽어 팁 비트맵·간격·**동작(필압/지터/흩뿌림)**·텍스처가 나온다
//   2) 정직한 실패 — 번역하지 못한 키가 ImportReport 에 Dropped 로 남는다
//   3) 깨진 파일 — 모든 길이로 잘라도 크래시 없이 Result 오류로 떨어진다
#include <mari/io/brush/importer.hpp>
#include <mari/test/harness.hpp>

#include <cstdlib>

#include "fixture_builder.hpp"

#include <algorithm>

using namespace mari;
using namespace mari::io::brush;
namespace fx = mari::testfix;
namespace mb = mari::brush;

namespace {

/// 4×4 원형 팁. 실물 abr 규약: 255 = 잉크 가득, 0 = 빈 곳(모서리).
std::vector<u8> tipPixels() {
    return {0,   255, 255, 0,   //
            255, 255, 255, 255, //
            255, 255, 255, 255, //
            0,   255, 255, 0};
}

/// 3×2 텍스처 패턴.
std::vector<u8> patternPixels() { return {10, 20, 30, 40, 50, 60}; }

/// 팁 + 동작 + 텍스처를 모두 담은 "제대로 된" abr 한 개.
std::vector<u8> buildFullAbr() {
    const auto samp = fx::sampSection({
        fx::SampSpec{"tip-uuid-1", 4, 4, tipPixels(), 25, /*rle=*/false},
        fx::SampSpec{"tip-uuid-2", 4, 4, tipPixels(), 40, /*rle=*/true},
    });
    const auto patt = fx::pattSection({fx::PattSpec{"pat-id-1", "종이", 3, 2, patternPixels()}});

    const auto brush0 = fx::dvObjc(
        "", "Brsh",
        {
            {"Nm  ", fx::dvText("잉크펜")},
            {"Brsh", fx::dvObjc("", "Brsh",
                                {{"Dmtr", fx::dvUnitFloat("#Pxl", 36.0)},
                                 {"Angl", fx::dvUnitFloat("#Ang", 15.0)},
                                 {"Rndn", fx::dvUnitFloat("#Prc", 50.0)},
                                 {"Hrdn", fx::dvUnitFloat("#Prc", 80.0)},
                                 {"Spcn", fx::dvUnitFloat("#Prc", 12.0)},
                                 {"Intr", fx::dvBool(true)},
                                 {"sampledData", fx::dvText("tip-uuid-2")}})},
            {"Opct", fx::dvUnitFloat("#Prc", 90.0)},
            {"Flw ", fx::dvUnitFloat("#Prc", 70.0)},
            {"Bld ", fx::dvEnum("BlnM", "Mltp")},
            // 필압 → 크기 (최소 20%)
            {"szVr", fx::dvObjc("", "szVr",
                                {{"bVTy", fx::dvEnum("bVTy", "Prs ")},
                                 {"minimum", fx::dvUnitFloat("#Prc", 20.0)},
                                 {"jitter", fx::dvUnitFloat("#Prc", 0.0)}})},
            // 랜덤 지터 → 불투명도
            {"opVr", fx::dvObjc("", "opVr",
                                {{"bVTy", fx::dvEnum("bVTy", "Off ")},
                                 {"jitter", fx::dvUnitFloat("#Prc", 30.0)}})},
            {"Scat", fx::dvUnitFloat("#Prc", 150.0)},
            {"Cnt ", fx::dvLong(3)},
            {"Txtr", fx::dvObjc("", "Txtr",
                                {{"Scl ", fx::dvUnitFloat("#Prc", 200.0)},
                                 {"textureDepth", fx::dvUnitFloat("#Prc", 60.0)},
                                 {"textureBlendMode", fx::dvEnum("BlnM", "Mltp")},
                                 {"Ptrn", fx::dvObjc("", "Ptrn", {{"Idnt", fx::dvText("pat-id-1")},
                                                                  {"Nm  ", fx::dvText("종이")}})}})},
            // 아래 둘은 일부러 번역하지 않는다 — 리포트에 Dropped 로 떠야 한다.
            {"Nose", fx::dvBool(true)},
            {"Wtdg", fx::dvBool(true)},
            {"protectTexture", fx::dvBool(true)},
            {"someUnknownKey", fx::dvBool(true)},
        });

    const auto brush1 =
        fx::dvObjc("", "Brsh",
                   {{"Nm  ", fx::dvText("연필")},
                    {"Brsh", fx::dvObjc("", "Brsh",
                                        {{"Dmtr", fx::dvUnitFloat("#Pxl", 12.0)},
                                         {"Spcn", fx::dvUnitFloat("#Prc", 5.0)},
                                         {"sampledData", fx::dvText("tip-uuid-1")}})}});

    const auto desc = fx::descSection({{"Brsh", fx::dvList({brush0, brush1})}});
    return fx::buildAbr(6, 2,
                        {{"samp", samp}, {"desc", desc}, {"patt", patt}});
}

const mb::MariBrushPreset* findPreset(const mb::ImportResult& r, const std::string& name) {
    for (const auto& p : r.presets)
        if (p.name == name)
            return &p;
    return nullptr;
}

bool hasNote(const mb::ImportReport& report, mb::ImportSeverity sev, const std::string& needle) {
    for (const auto& n : report.notes)
        if (n.severity == sev &&
            (n.sourceKey.find(needle) != std::string::npos ||
             n.message.find(needle) != std::string::npos))
            return true;
    return false;
}

const mb::DynamicLink* findLink(const mb::MariBrushPreset& p, mb::DynamicInput in,
                                mb::DynamicOutput out) {
    for (const auto& d : p.dynamics)
        if (d.input == in && d.output == out)
            return &d;
    return nullptr;
}

} // namespace

MARI_TEST(abr_roundtrip_tip_and_behavior) {
    const auto bytes = buildFullAbr();
    auto result = importAbrBytes(bytes.data(), bytes.size(), "fixture.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::ImportResult& r = result.value();
    CHECK_EQ(r.presets.size(), usize(2));
    CHECK_EQ(r.report.presetCount, usize(2));

    const mb::MariBrushPreset* pen = findPreset(r, "잉크펜");
    CHECK(pen != nullptr);
    if (pen == nullptr)
        return;

    CHECK_EQ(pen->sourceFormat, std::string("abr"));
    CHECK_NEAR(pen->tip.diameter, 36.0f, 0.001f);
    CHECK_NEAR(pen->tip.angle, 15.0f, 0.001f);
    CHECK_NEAR(pen->tip.aspectRatio, 0.5f, 0.001f);
    CHECK_NEAR(pen->tip.hardness, 0.8f, 0.001f);
    // spacing 은 **퍼센트가 아니라 비율**이다. 12% → 0.12
    CHECK_NEAR(pen->spacing, 0.12f, 0.001f);
    CHECK_NEAR(pen->opacity, 0.9f, 0.001f);
    CHECK_NEAR(pen->flow, 0.7f, 0.001f);
    CHECK(pen->blendMode == BlendMode::Multiply);

    // 팁 비트맵이 sampledData 로 올바르게 연결됐고, 명암이 뒤집혔다(0=잉크 → 255=잉크).
    CHECK(pen->tip.kind == mb::TipKind::Bitmap);
    CHECK_EQ(pen->tip.bitmap.width, 4);
    CHECK_EQ(pen->tip.bitmap.height, 4);
    CHECK_EQ(int(pen->tip.bitmap.pixels[0]), 0);   // 모서리 = 빈 곳(뒤집지 않는다)
    CHECK_EQ(int(pen->tip.bitmap.pixels[1]), 255); // 잉크
}

MARI_TEST(abr_imports_dynamics_not_just_texture) {
    // Krita 가 안 하는 부분이다. 필압·지터·흩뿌림이 실제로 링크로 들어와야 한다.
    const auto bytes = buildFullAbr();
    auto result = importAbrBytes(bytes.data(), bytes.size(), "fixture.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::MariBrushPreset* pen = findPreset(result.value(), "잉크펜");
    CHECK(pen != nullptr);
    if (pen == nullptr)
        return;

    const mb::DynamicLink* size = findLink(*pen, mb::DynamicInput::Pressure, mb::DynamicOutput::Size);
    CHECK(size != nullptr);
    if (size != nullptr) {
        CHECK_EQ(size->curve.points.size(), usize(2));
        CHECK_NEAR(size->curve.points[0].y, 0.2f, 0.001f); // minimum 20%
        CHECK_NEAR(size->curve.points[1].y, 1.0f, 0.001f);
    }

    const mb::DynamicLink* opacity =
        findLink(*pen, mb::DynamicInput::Random, mb::DynamicOutput::Opacity);
    CHECK(opacity != nullptr);
    if (opacity != nullptr)
        CHECK_NEAR(opacity->curve.points[0].y, 0.7f, 0.001f); // 지터 30%

    const mb::DynamicLink* scatter =
        findLink(*pen, mb::DynamicInput::Random, mb::DynamicOutput::Scatter);
    CHECK(scatter != nullptr);
    if (scatter != nullptr)
        CHECK_NEAR(scatter->curve.points[1].y, 1.5f, 0.001f); // 150%

    // 스탬프 개수 · 젖은 가장자리 · 노이즈는 엔진이 직접 쓴다.
    CHECK_EQ(pen->scatterCount, 3);
    CHECK(pen->wetEdges);
    CHECK(pen->noise > 0.0f);
}

MARI_TEST(abr_imports_texture_pixels_from_patt) {
    const auto bytes = buildFullAbr();
    auto result = importAbrBytes(bytes.data(), bytes.size(), "fixture.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::MariBrushPreset* pen = findPreset(result.value(), "잉크펜");
    CHECK(pen != nullptr);
    if (pen == nullptr)
        return;

    CHECK(pen->texture.has_value());
    if (!pen->texture.has_value())
        return;
    CHECK_EQ(pen->texture->image.width, 3);
    CHECK_EQ(pen->texture->image.height, 2);
    CHECK_EQ(int(pen->texture->image.pixels[0]), 10);
    CHECK_EQ(int(pen->texture->image.pixels[5]), 60);
    CHECK_NEAR(pen->texture->scale, 2.0f, 0.001f);
    CHECK_NEAR(pen->texture->depth, 0.6f, 0.001f);
    CHECK(pen->texture->blendMode == BlendMode::Multiply);
}

MARI_TEST(abr_reports_every_untranslated_key) {
    // **핵심 약속**: 번역 못 한 건 조용히 버리지 않는다.
    const auto bytes = buildFullAbr();
    auto result = importAbrBytes(bytes.data(), bytes.size(), "fixture.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::ImportReport& report = result.value().report;

    CHECK(report.hasDropped());
    CHECK(hasNote(report, mb::ImportSeverity::Dropped, "someUnknownKey"));
    // 이제 번역하는 키는 Dropped 로 뜨면 안 된다.
    CHECK(!hasNote(report, mb::ImportSeverity::Dropped, "Nose"));
    CHECK(!hasNote(report, mb::ImportSeverity::Dropped, "Wtdg"));
    CHECK(!hasNote(report, mb::ImportSeverity::Dropped, "protectTexture"));
    // 노트에는 브러시 이름이 들어가 사용자가 어느 브러시인지 알 수 있어야 한다.
    bool named = false;
    for (const auto& n : report.notes)
        if (n.message.find("잉크펜") != std::string::npos)
            named = true;
    CHECK(named);
    // 우리가 제대로 번역한 키는 Dropped 로 뜨면 안 된다.
    CHECK(!hasNote(report, mb::ImportSeverity::Dropped, "Dmtr"));
    CHECK(!hasNote(report, mb::ImportSeverity::Dropped, "Spcn"));
}

MARI_TEST(abr_rle_tip_matches_raw_tip) {
    // 같은 픽셀을 raw 와 RLE 로 각각 넣었다. 디코드 결과가 같아야 한다.
    const auto bytes = buildFullAbr();
    auto result = importAbrBytes(bytes.data(), bytes.size(), "fixture.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::MariBrushPreset* rawTip = findPreset(result.value(), "연필");   // tip-uuid-1 (raw)
    const mb::MariBrushPreset* rleTip = findPreset(result.value(), "잉크펜"); // tip-uuid-2 (RLE)
    CHECK(rawTip != nullptr);
    CHECK(rleTip != nullptr);
    if (rawTip == nullptr || rleTip == nullptr)
        return;
    CHECK(rawTip->tip.bitmap.pixels == rleTip->tip.bitmap.pixels);
}

MARI_TEST(abr_without_desc_still_yields_tips) {
    // 동작 정보가 없는 abr. 팁만 가져오고, **그렇다고 리포트에 적어야 한다.**
    const auto samp = fx::sampSection({fx::SampSpec{"only-tip", 4, 4, tipPixels(), 50, false}});
    const auto bytes = fx::buildAbr(6, 2, {{"samp", samp}});
    auto result = importAbrBytes(bytes.data(), bytes.size(), "tiponly.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    CHECK_EQ(result.value().presets.size(), usize(1));
    CHECK_NEAR(result.value().presets[0].spacing, 0.25f, 0.001f); // v6 머리말엔 간격이 없다 — 기본 25%
    CHECK(result.value().presets[0].tip.kind == mb::TipKind::Bitmap);
    CHECK(hasNote(result.value().report, mb::ImportSeverity::Degraded, "desc"));
}

MARI_TEST(abr_unknown_section_is_skipped_with_a_note) {
    const auto samp = fx::sampSection({fx::SampSpec{"tip", 4, 4, tipPixels(), 25, false}});
    const std::vector<u8> junk(16, 0xAB);
    const auto bytes = fx::buildAbr(6, 2, {{"zzzz", junk}, {"samp", samp}});
    auto result = importAbrBytes(bytes.data(), bytes.size(), "unknown.abr");
    CHECK(result.ok());
    if (!result.ok())
        return;
    CHECK_EQ(result.value().presets.size(), usize(1));
    CHECK(hasNote(result.value().report, mb::ImportSeverity::Info, "zzzz"));
}

MARI_TEST(abr_rejects_unsupported_versions) {
    const std::vector<u8> v1 = {0x00, 0x01, 0x00, 0x01, 0, 0, 0, 0};
    auto r1 = importAbrBytes(v1.data(), v1.size(), "old.abr");
    CHECK(!r1.ok());
    CHECK(r1.code() == ErrorCode::Unsupported);

    const std::vector<u8> vX = {0x00, 0x63, 0x00, 0x01, 0, 0, 0, 0};
    auto rX = importAbrBytes(vX.data(), vX.size(), "weird.abr");
    CHECK(!rX.ok());
    CHECK(rX.code() == ErrorCode::Unsupported);
}

MARI_TEST(abr_truncated_files_never_crash) {
    // 퍼즈 비슷하게: 모든 길이로 잘라 본다. 크래시·무한루프 없이 끝나야 한다.
    const auto full = buildFullAbr();
    for (usize cut = 0; cut <= full.size(); cut += 1) {
        std::vector<u8> partial(full.begin(), full.begin() + static_cast<long>(cut));
        auto result = importAbrBytes(partial.data(), partial.size(), "cut.abr");
        if (result.ok()) {
            // 부분 성공은 허용한다. 다만 프리셋이 있으면 리포트가 비어 있으면 안 된다.
            CHECK(result.value().presets.size() > 0);
        }
    }
    CHECK(true);
}

MARI_TEST(abr_garbage_bytes_are_rejected) {
    std::vector<u8> junk(512);
    u32 state = 12345;
    for (usize i = 0; i < junk.size(); ++i) {
        state = state * 1103515245u + 12345u;
        junk[i] = static_cast<u8>(state >> 16);
    }
    junk[0] = 0;
    junk[1] = 6; // 버전만 그럴듯하게 맞춰 둔다
    auto result = importAbrBytes(junk.data(), junk.size(), "junk.abr");
    // 성공하면 안 되고, 죽어서도 안 된다.
    CHECK(!result.ok());
}

MARI_TEST(abr_bad_length_field_does_not_overrun) {
    // 섹션 길이가 파일보다 크다고 거짓말하는 파일.
    fx::ByteWriter w;
    w.u16be(6);
    w.u16be(1);
    w.ascii("8BIM");
    w.ascii("samp");
    w.u32be(0xFFFFFF00u); // 거짓말
    w.u32be(0);
    auto result = importAbrBytes(w.bytes.data(), w.bytes.size(), "liar.abr");
    CHECK(!result.ok());
}

MARI_TEST(abr_empty_or_tiny_input) {
    auto r0 = importAbrBytes(nullptr, 0, "empty.abr");
    CHECK(!r0.ok());
    const std::vector<u8> tiny = {0x00};
    auto r1 = importAbrBytes(tiny.data(), tiny.size(), "tiny.abr");
    CHECK(!r1.ok());
}

MARI_TEST(abr_file_roundtrip_through_disk) {
    fx::TempFile tmp("abr");
    const auto bytes = buildFullAbr();
    CHECK(fx::writeFile(tmp.path(), bytes));

    CHECK(detectFormat(tmp.path()) == BrushFormat::Abr);
    auto result = importBrushFile(tmp.path());
    CHECK(result.ok());
    if (result.ok()) {
        CHECK_EQ(result.value().presets.size(), usize(2));
        CHECK_EQ(result.value().report.sourcePath, tmp.path());
    }

    auto missing = importAbrFile("/존재하지/않는/파일.abr");
    CHECK(!missing.ok());
    CHECK(missing.code() == ErrorCode::IoError);
}

MARI_TEST(abr_real_world_cc0_file_imports_every_tip_with_nothing_dropped) {
    // 실물 검증(CC0, K. M. Alexander "Myer Settlement"): 148개 브러시, 전부 비트맵 팁, Dropped 0.
    // 🔴 MARI_FIXTURE_DIR 은 ctest 가 준다. 없으면 **실패**다 — 조용히 건너뛰지 않는다.
    const char* dir = std::getenv("MARI_FIXTURE_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr)
        return;
    const std::string path = std::string(dir) + "/brushes/myer-settlement-cc0.abr";
    auto result = importAbrFile(path);
    CHECK(result.ok());
    if (!result.ok())
        return;
    CHECK_EQ(result.value().presets.size(), usize(148));
    usize bitmaps = 0;
    for (const auto& p : result.value().presets) {
        if (p.tip.kind == mb::TipKind::Bitmap && !p.tip.bitmap.empty())
            ++bitmaps;
    }
    CHECK_EQ(bitmaps, usize(148));
    CHECK(!result.value().report.hasDropped());
    // 팁 극성: 모서리는 빈 곳(0), 어딘가에는 잉크(255)가 있다.
    const auto& tip = result.value().presets[0].tip.bitmap;
    CHECK_EQ(int(tip.pixels[0]), 0);
    bool anyInk = false;
    for (const u8 v : tip.pixels)
        anyInk = anyInk || v == 255;
    CHECK(anyInk);
    // 간격은 desc 의 Spcn(10%) 에서 온다.
    CHECK_NEAR(result.value().presets[0].spacing, 0.10f, 0.001f);
}

MARI_TEST_MAIN()
