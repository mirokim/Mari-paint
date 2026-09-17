// Mari Paint — Clip Studio .sut 임포터 테스트.
//
// .sut 는 통째로 SQLite 라서 픽스처를 **진짜 SQLite DB 로 만든다.**
// 가장 중요한 검증은 이것이다:
//   🔴 **컬럼 집합이 CSP 버전마다 다르다.** 그래서 스키마가 다른 두 버전을 모두 만들고,
//      컬럼이 빠진 쪽에서도 크래시 없이 부분 성공하며 리포트가 누락을 정확히 말하는지 본다.
//   🔴 effector blob 은 리버싱이 불확실하다. 해석 실패해도 팁·기본 파라미터는 살아야 한다.
#include <mari/io/brush/importer.hpp>
#include <mari/test/harness.hpp>

#include "fixture_builder.hpp"

#include <string>

using namespace mari;
using namespace mari::io::brush;
namespace fx = mari::testfix;
namespace mb = mari::brush;

namespace {

/// 텍스처용 tar(PNG 한 장) 를 만든다.
std::vector<u8> textureTarBlob() {
    const std::vector<u8> pixels = {10, 20, 30, 40, 50, 60, 70, 80, 90};
    const auto png = fx::writeGrayPng(3, 3, pixels);
    return fx::buildTar({{"texture/paper.png", png}, {"meta.txt", {'o', 'k'}}});
}

/// 신형 CSP 를 흉내낸 .sut — 컬럼이 다 있다.
bool buildSutV1(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
        return false;
    bool ok = fx::execSql(
        db,
        "CREATE TABLE Node(NodeId INTEGER PRIMARY KEY, NodeName TEXT, "
        "  NodeVariantId INTEGER, NodeInitVariantId INTEGER);"
        "CREATE TABLE Variant(VariantId INTEGER PRIMARY KEY, BrushSize REAL, BrushOpacity REAL,"
        "  BrushDensity REAL, BrushInterval REAL, BrushHardness REAL, BrushAngle REAL,"
        "  BrushFlatRate REAL, BrushBlendMode INTEGER, TextureScale REAL, TextureDensity REAL,"
        "  BrushSizePressureEffector BLOB, BrushOpacityPressureEffector BLOB,"
        "  BrushMysteryKnob REAL, SomeUnknownText TEXT);"
        "CREATE TABLE MaterialFile(FileId INTEGER PRIMARY KEY, FileData BLOB);"
        "INSERT INTO Node VALUES(1, '수채 둥근붓', 11, 10);"
        "INSERT INTO Node VALUES(2, 'G펜', 12, 10);"
        "INSERT INTO Variant(VariantId, BrushSize, BrushOpacity, BrushDensity, BrushInterval,"
        "  BrushHardness, BrushAngle, BrushFlatRate, BrushBlendMode, TextureScale, TextureDensity,"
        "  BrushMysteryKnob, SomeUnknownText)"
        "  VALUES(11, 45.0, 80.0, 60.0, 15.0, 70.0, 30.0, 30.0, 1, 200.0, 50.0, 7.5, '수수께끼');"
        "INSERT INTO Variant(VariantId, BrushSize, BrushOpacity, BrushInterval)"
        "  VALUES(12, 8.0, 100.0, 5.0);");
    if (ok) {
        // 브러시 11: 제대로 된 effector blob(가설 A)
        const auto good = fx::effectorBlobA({{0.0, 0.2}, {0.5, 0.6}, {1.0, 1.0}});
        ok = fx::bindBlobRow(db, "UPDATE Variant SET BrushSizePressureEffector = ? "
                                 "WHERE VariantId = 11",
                             good);
    }
    if (ok) {
        // 브러시 11: 우리가 아직 해석 못 하는 형태의 blob — 여기서 죽으면 안 된다.
        const std::vector<u8> mystery = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03,
                                         0x04, 0x05, 0x06, 0x07};
        ok = fx::bindBlobRow(db, "UPDATE Variant SET BrushOpacityPressureEffector = ? "
                                 "WHERE VariantId = 11",
                             mystery);
    }
    if (ok)
        ok = fx::bindBlobRow(db, "INSERT INTO MaterialFile(FileData) VALUES(?)", textureTarBlob());
    sqlite3_close(db);
    return ok;
}

/// 구형(또는 다른) CSP 를 흉내낸 .sut — 컬럼 집합이 다르고 대부분이 없다.
/// 테이블 이름도 다르고, 현재 설정(NodeVariantId) 컬럼조차 없다.
bool buildSutV2(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
        return false;
    const bool ok = fx::execSql(
        db, "CREATE TABLE Node(NodeId INTEGER PRIMARY KEY, NodeName TEXT, "
            "  NodeInitVariantId INTEGER);"
            "CREATE TABLE SubToolVariant(VariantId INTEGER PRIMARY KEY, BrushDiameter REAL,"
            "  Opacity REAL, Interval REAL);"
            "INSERT INTO Node VALUES(1, '구버전 펜', 21);"
            "INSERT INTO SubToolVariant VALUES(21, 24.0, 50.0, 0.25);");
    sqlite3_close(db);
    return ok;
}

bool hasNote(const mb::ImportReport& r, mb::ImportSeverity sev, const std::string& needle) {
    for (const auto& n : r.notes)
        if (n.severity == sev && (n.sourceKey.find(needle) != std::string::npos ||
                                  n.message.find(needle) != std::string::npos))
            return true;
    return false;
}

const mb::MariBrushPreset* findPreset(const mb::ImportResult& r, const std::string& name) {
    for (const auto& p : r.presets)
        if (p.name == name)
            return &p;
    return nullptr;
}

} // namespace

MARI_TEST(sut_v1_full_schema_roundtrip) {
    fx::TempFile tmp("sut_v1");
    CHECK(buildSutV1(tmp.path()));
    CHECK(detectFormat(tmp.path()) == BrushFormat::Sut);

    auto result = importSutFile(tmp.path());
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::ImportResult& r = result.value();
    CHECK_EQ(r.presets.size(), usize(2));
    CHECK_EQ(r.report.presetCount, usize(2));

    const mb::MariBrushPreset* brush = findPreset(r, "수채 둥근붓");
    CHECK(brush != nullptr);
    if (brush == nullptr)
        return;
    CHECK_EQ(brush->sourceFormat, std::string("sut"));
    CHECK_NEAR(brush->tip.diameter, 45.0f, 0.001f);
    CHECK_NEAR(brush->opacity, 0.8f, 0.001f);
    CHECK_NEAR(brush->flow, 0.6f, 0.001f);
    CHECK_NEAR(brush->spacing, 0.15f, 0.001f);
    CHECK_NEAR(brush->tip.hardness, 0.7f, 0.001f);
    CHECK_NEAR(brush->tip.angle, 30.0f, 0.001f);
    // CSP 의 "평평함" 30% → Mari 의 종횡비 0.7
    CHECK_NEAR(brush->tip.aspectRatio, 0.7f, 0.001f);
    CHECK(brush->blendMode == BlendMode::Multiply);
}

MARI_TEST(sut_effector_blob_becomes_a_pressure_curve) {
    fx::TempFile tmp("sut_curve");
    CHECK(buildSutV1(tmp.path()));
    auto result = importSutFile(tmp.path());
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::MariBrushPreset* brush = findPreset(result.value(), "수채 둥근붓");
    CHECK(brush != nullptr);
    if (brush == nullptr)
        return;

    const mb::DynamicLink* size = nullptr;
    for (const auto& d : brush->dynamics)
        if (d.output == mb::DynamicOutput::Size && d.input == mb::DynamicInput::Pressure)
            size = &d;
    CHECK(size != nullptr);
    if (size == nullptr)
        return;
    CHECK_EQ(size->curve.points.size(), usize(3));
    CHECK_NEAR(size->curve.points[0].y, 0.2f, 0.001f);
    CHECK_NEAR(size->curve.points[1].x, 0.5f, 0.001f);
    CHECK_NEAR(size->curve.points[2].y, 1.0f, 0.001f);
    // 어떤 가설로 읽었는지 사용자에게 말해야 한다.
    CHECK(hasNote(result.value().report, mb::ImportSeverity::Info, "가설 A"));
}

MARI_TEST(sut_unreadable_effector_blob_keeps_the_brush_alive) {
    // 🔴 요구사항: effector blob 해석 실패해도 팁 + 기본 파라미터는 살린다.
    fx::TempFile tmp("sut_badcurve");
    CHECK(buildSutV1(tmp.path()));
    auto result = importSutFile(tmp.path());
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::MariBrushPreset* brush = findPreset(result.value(), "수채 둥근붓");
    CHECK(brush != nullptr);
    if (brush == nullptr)
        return;

    // 불투명도 커브는 해석 실패했지만 브러시 자체는 온전하다.
    for (const auto& d : brush->dynamics)
        CHECK(d.output != mb::DynamicOutput::Opacity);
    CHECK_NEAR(brush->tip.diameter, 45.0f, 0.001f);
    CHECK_NEAR(brush->opacity, 0.8f, 0.001f);
    CHECK(hasNote(result.value().report, mb::ImportSeverity::Dropped,
                  "BrushOpacityPressureEffector"));
}

MARI_TEST(sut_texture_comes_out_of_the_tar_in_filedata) {
    fx::TempFile tmp("sut_tex");
    CHECK(buildSutV1(tmp.path()));
    auto result = importSutFile(tmp.path());
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::MariBrushPreset* brush = findPreset(result.value(), "수채 둥근붓");
    CHECK(brush != nullptr);
    if (brush == nullptr)
        return;
    CHECK(brush->texture.has_value());
    if (!brush->texture.has_value())
        return;
    CHECK_EQ(brush->texture->image.width, 3);
    CHECK_EQ(brush->texture->image.height, 3);
    CHECK_EQ(int(brush->texture->image.pixels[0]), 10);
    CHECK_EQ(int(brush->texture->image.pixels[8]), 90);
    CHECK_NEAR(brush->texture->scale, 2.0f, 0.001f);
    CHECK_NEAR(brush->texture->depth, 0.5f, 0.001f);
}

MARI_TEST(sut_reports_columns_it_could_not_translate) {
    fx::TempFile tmp("sut_report");
    CHECK(buildSutV1(tmp.path()));
    auto result = importSutFile(tmp.path());
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::ImportReport& report = result.value().report;
    CHECK(report.hasDropped());
    CHECK(hasNote(report, mb::ImportSeverity::Dropped, "brushmysteryknob"));
    CHECK(hasNote(report, mb::ImportSeverity::Dropped, "someunknowntext"));
    // 기본키는 파라미터가 아니므로 보고하지 않는다.
    CHECK(!hasNote(report, mb::ImportSeverity::Dropped, "variantid"));
}

MARI_TEST(sut_v2_missing_columns_still_partially_succeeds) {
    // 🔴 컬럼 집합이 다른 CSP 버전. 크래시 없이 읽히고, 없는 것은 리포트가 말해야 한다.
    fx::TempFile tmp("sut_v2");
    CHECK(buildSutV2(tmp.path()));

    auto result = importSutFile(tmp.path());
    CHECK(result.ok());
    if (!result.ok())
        return;
    const mb::ImportResult& r = result.value();
    CHECK_EQ(r.presets.size(), usize(1));
    CHECK_EQ(r.presets[0].name, std::string("구버전 펜"));

    // 이름이 다른 컬럼도 별칭으로 잡아낸다.
    CHECK_NEAR(r.presets[0].tip.diameter, 24.0f, 0.001f);
    CHECK_NEAR(r.presets[0].opacity, 0.5f, 0.001f);
    // 0.25 는 1 이하라 이미 비율로 본다(퍼센트로 착각하지 않는다).
    CHECK_NEAR(r.presets[0].spacing, 0.25f, 0.001f);

    // 없는 컬럼은 전부 리포트에 남는다.
    CHECK(hasNote(r.report, mb::ImportSeverity::Info, "경도"));
    CHECK(hasNote(r.report, mb::ImportSeverity::Info, "각도"));
    CHECK(hasNote(r.report, mb::ImportSeverity::Info, "합성 모드"));
    CHECK(hasNote(r.report, mb::ImportSeverity::Info, "필압 커브"));
    // 현재 설정 컬럼이 없어 기본 설정으로 대체했다는 사실도 말한다.
    CHECK(hasNote(r.report, mb::ImportSeverity::Degraded, "NodeVariantId"));
    // 없는 텍스처를 있다고 말하면 안 된다.
    CHECK(!r.presets[0].texture.has_value());
}

MARI_TEST(sut_two_schema_versions_produce_the_same_shape) {
    // 버전이 달라도 결과 타입은 같다 — 상위(UI/엔진)가 분기하지 않아도 된다.
    fx::TempFile a("sut_a");
    fx::TempFile b("sut_b");
    CHECK(buildSutV1(a.path()));
    CHECK(buildSutV2(b.path()));
    auto ra = importSutFile(a.path());
    auto rb = importSutFile(b.path());
    CHECK(ra.ok());
    CHECK(rb.ok());
    if (!ra.ok() || !rb.ok())
        return;
    for (const auto* r : {&ra.value(), &rb.value()}) {
        for (const auto& p : r->presets) {
            CHECK(!p.name.empty());
            CHECK_EQ(p.sourceFormat, std::string("sut"));
            CHECK(p.tip.diameter > 0.0f);
            CHECK(p.spacing > 0.0f);
            CHECK(p.opacity >= 0.0f && p.opacity <= 1.0f);
        }
    }
}

MARI_TEST(sut_garbage_file_is_rejected_not_crashed) {
    fx::TempFile tmp("sut_junk");
    std::vector<u8> junk(4096);
    u32 state = 7;
    for (usize i = 0; i < junk.size(); ++i) {
        state = state * 1103515245u + 12345u;
        junk[i] = static_cast<u8>(state >> 16);
    }
    CHECK(fx::writeFile(tmp.path(), junk));
    auto result = importSutFile(tmp.path());
    CHECK(!result.ok());
}

MARI_TEST(sut_truncated_database_is_rejected_not_crashed) {
    fx::TempFile src("sut_trunc_src");
    CHECK(buildSutV1(src.path()));

    // 정상 DB 를 여러 지점에서 잘라 본다.
    std::FILE* f = std::fopen(src.path().c_str(), "rb");
    CHECK(f != nullptr);
    if (f == nullptr)
        return;
    std::vector<u8> full;
    u8 chunk[4096];
    while (true) {
        const usize n = std::fread(chunk, 1, sizeof(chunk), f);
        if (n == 0)
            break;
        full.insert(full.end(), chunk, chunk + n);
    }
    std::fclose(f);
    CHECK(full.size() > 1024);

    for (usize denom : {usize(1), usize(2), usize(4), usize(8), usize(64)}) {
        fx::TempFile cut("sut_trunc");
        std::vector<u8> partial(full.begin(),
                                full.begin() + static_cast<long>(full.size() / denom));
        CHECK(fx::writeFile(cut.path(), partial));
        auto result = importSutFile(cut.path());
        // 성공해도 되고(앞부분만 멀쩡한 경우) 실패해도 되지만, 죽으면 안 된다.
        if (result.ok())
            CHECK(result.value().presets.size() > 0);
    }
    CHECK(true);
}

MARI_TEST(sut_empty_file_and_missing_file) {
    fx::TempFile tmp("sut_empty");
    CHECK(fx::writeFile(tmp.path(), {}));
    auto empty = importSutFile(tmp.path());
    CHECK(!empty.ok());

    auto missing = importSutFile("/존재하지/않는/파일.sut");
    CHECK(!missing.ok());
}

MARI_TEST(sut_database_without_nodename_is_rejected_clearly) {
    fx::TempFile tmp("sut_nonode");
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(tmp.path().c_str(), &db) == SQLITE_OK);
    CHECK(fx::execSql(db, "CREATE TABLE Whatever(a INTEGER); INSERT INTO Whatever VALUES(1);"));
    sqlite3_close(db);

    auto result = importSutFile(tmp.path());
    CHECK(!result.ok());
    CHECK(result.code() == ErrorCode::ParseError);
    CHECK(result.message().find("NodeName") != std::string::npos);
}

MARI_TEST(sut_import_through_format_detection) {
    fx::TempFile tmp("sut_detect");
    CHECK(buildSutV1(tmp.path()));
    auto result = importBrushFile(tmp.path());
    CHECK(result.ok());
    if (result.ok())
        CHECK_EQ(result.value().report.sourcePath, tmp.path());

    fx::TempFile junk("sut_unknown");
    CHECK(fx::writeFile(junk.path(), std::vector<u8>{'h', 'e', 'l', 'l', 'o', '!', '?', '@'}));
    auto unknown = importBrushFile(junk.path());
    CHECK(!unknown.ok());
    CHECK(unknown.code() == ErrorCode::Unsupported);
}

MARI_TEST_MAIN()
