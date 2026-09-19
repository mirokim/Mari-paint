// Mari Paint — Clip Studio Paint .sut 임포터.
//
// **.sut 파일은 통째로 SQLite 데이터베이스다**(docs/01 3.3 — 이 프로젝트의 핵심 발견).
//   · 브러시 이름      : NodeName 컬럼
//   · 현재 설정/기본값 : NodeVariantId / NodeInitVariantId 가 가리키는 variant 행
//   · 파라미터         : variant 테이블에 행 1개 = 브러시 1개, 열 1개 = 파라미터 1개 (수백 개)
//   · 텍스처           : FileData 컬럼 안의 **비압축 tar** 속 PNG
//   · 동적 커브        : *effector 계열 컬럼의 **빅엔디안 blob**
//
// 🔴 컬럼 집합이 CSP 버전마다 다르다. 컬럼 이름을 **하드코딩하지 않는다.**
//    PRAGMA table_info 로 런타임 조회 후 "있는 것만" 매핑하고, 없는 것은 리포트에 남긴다.
//    effector blob 은 리버싱이 불확실하다 — 파싱이 실패해도 팁·기본 파라미터는 살린다.
#include <mari/io/brush/importer.hpp>

#include <mari/io/brush/byte_reader.hpp>
#include <mari/io/brush/png_gray.hpp>
#include <mari/io/brush/tar.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mari::io::brush {

using mari::brush::BrushTexture;
using mari::brush::DualBrush;
using mari::brush::TipKind;
using mari::brush::DynamicInput;
using mari::brush::DynamicLink;
using mari::brush::DynamicOutput;
using mari::brush::GrayImage;
using mari::brush::ImportReport;
using mari::brush::ImportSeverity;
using mari::brush::MariBrushPreset;
using mari::brush::ResponseCurve;

namespace {

/// 브러시 하나당 "번역 못 한 컬럼" 노트 상한. CSP variant 는 컬럼이 수백 개라 상한이 필요하다.
constexpr usize kMaxUnmappedNotes = 24;
/// 한 번에 읽을 브러시 수 상한(깨진 DB 방어).
constexpr int kMaxBrushes = 4096;

std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// SQL 식별자를 따옴표로 감싼다. 테이블 이름이 어떤 글자든 안전하게 쓰기 위해서다.
std::string quoteIdent(const std::string& name) {
    std::string out = "\"";
    for (char c : name) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    out += "\"";
    return out;
}

/// sqlite3_stmt 자동 해제.
struct StmtDeleter {
    void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

StmtPtr prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return nullptr;
    return StmtPtr(raw);
}

/// 컬럼 하나의 메타데이터(PRAGMA table_info).
struct ColumnInfo {
    std::string name;
    std::string lower;
    bool primaryKey = false;
};

/// 테이블의 컬럼 목록. **하드코딩 금지 규칙의 실체가 이 함수다.**
std::vector<ColumnInfo> tableColumns(sqlite3* db, const std::string& table) {
    std::vector<ColumnInfo> out;
    auto stmt = prepare(db, "PRAGMA table_info(" + quoteIdent(table) + ")");
    if (!stmt)
        return out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt.get(), 1);
        if (name == nullptr)
            continue;
        ColumnInfo c;
        c.name = reinterpret_cast<const char*>(name);
        c.lower = toLower(c.name);
        c.primaryKey = sqlite3_column_int(stmt.get(), 5) != 0;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::string> listTables(sqlite3* db) {
    std::vector<std::string> out;
    auto stmt = prepare(db, "SELECT name FROM sqlite_master WHERE type='table'");
    if (!stmt)
        return out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt.get(), 0);
        if (name != nullptr)
            out.emplace_back(reinterpret_cast<const char*>(name));
    }
    return out;
}

/// 컬럼 목록에서 이름을 대소문자 무시로 찾는다. 없으면 nullptr.
const ColumnInfo* findColumn(const std::vector<ColumnInfo>& cols, const std::string& lowerName) {
    for (const ColumnInfo& c : cols)
        if (c.lower == lowerName)
            return &c;
    return nullptr;
}

// ── effector blob ────────────────────────────────────────────────────────

/// effector blob 해석 가설. 리버싱이 불확실해서 여러 배치를 시도하고
/// **어떤 가설로 읽었는지 리포트에 적는다.** 전부 실패해도 브러시 자체는 살린다.
enum class EffectorLayout {
    None = 0,
    /// A: u32 version | u32 count | count × (u32 x, u32 y), 1/10000 고정소수점
    VersionCountFixed,
    /// B: u32 count | count × (f32 x, f32 y) 빅엔디안
    CountFloat,
    /// C: u32 count | count × (u16 x, u16 y), 1/65535 고정소수점
    CountShort,
};

const char* effectorLayoutName(EffectorLayout l) {
    switch (l) {
    case EffectorLayout::None:              return "none";
    case EffectorLayout::VersionCountFixed: return "가설 A (버전+개수+고정소수점 32비트)";
    case EffectorLayout::CountFloat:        return "가설 B (개수+32비트 부동소수점)";
    case EffectorLayout::CountShort:        return "가설 C (개수+16비트 고정소수점)";
    }
    return "none";
}

/// 점들이 커브로 말이 되는가. x 오름차순 + 값 범위 확인.
bool curveIsSane(const ResponseCurve& c) {
    if (c.points.empty() || c.points.size() > 64)
        return false;
    f32 prevX = -1.0f;
    for (const auto& p : c.points) {
        if (!(p.x >= -0.001f && p.x <= 1.001f) || !(p.y >= -0.001f && p.y <= 4.001f))
            return false;
        if (p.x < prevX - 0.001f)
            return false;
        prevX = p.x;
    }
    return true;
}

/// 실물 effector blob(CSP 1.x~3.x, 실제 .sut 5개로 확인):
///   머리말 11×u32 BE: [0]=44(머리말 길이) [1]=플래그 [3]=최솟값% [6]=랜덤% …
///   이어서 (12, n, 16) 블록이 0개 이상: 점 n개, 점마다 BE double x, y — 첫 블록이 필압 커브.
///   두 번째 이후 블록(속도·기울기로 추정)은 확신이 없어 적용하지 않고 리포트에만 남긴다.
struct RealEffector {
    f32 minimum = 0.0f;     ///< 0..1
    f32 randomAmount = 0.0f;///< 0..1
    std::vector<ResponseCurve> curves;
};

bool parseRealEffector(const u8* data, usize size, RealEffector& out) {
    if (data == nullptr || size < 44)
        return false;
    ByteReader r(data, size);
    u32 head[11];
    for (u32& h : head)
        h = r.u32be();
    if (r.failed() || head[0] != 44)
        return false;
    out.minimum = std::clamp(static_cast<f32>(head[3]) / 100.0f, 0.0f, 1.0f);
    out.randomAmount = std::clamp(static_cast<f32>(head[6]) / 100.0f, 0.0f, 1.0f);
    // 블록 스캔: (12, n, 16) 을 찾는다. 사이의 미지 u32 는 건너뛴다.
    usize p = 44;
    while (p + 12 <= size) {
        ByteReader b(data + p, size - p);
        const u32 a = b.u32be();
        const u32 n = b.u32be();
        const u32 stride = b.u32be();
        if (a == 12 && stride == 16 && n >= 2 && n <= 64 && p + 12 + static_cast<usize>(n) * 16 <= size) {
            ResponseCurve c;
            for (u32 i = 0; i < n; ++i) {
                const f64 x = b.f64be();
                const f64 y = b.f64be();
                c.points.push_back({static_cast<f32>(x), static_cast<f32>(y)});
            }
            if (!b.failed() && curveIsSane(c))
                out.curves.push_back(std::move(c));
            p += 12 + static_cast<usize>(n) * 16;
        } else {
            p += 4;
        }
    }
    return true;
}

/// blob 하나를 커브로 해석한다. 성공하면 사용한 가설을 돌려준다.
EffectorLayout parseEffectorCurve(const u8* data, usize size, ResponseCurve& out) {
    if (data == nullptr || size < 8)
        return EffectorLayout::None;

    // 가설 A
    {
        ByteReader r(data, size);
        (void)r.u32be();
        const u32 count = r.u32be();
        if (!r.failed() && count >= 1 && count <= 64 &&
            size == 8 + static_cast<usize>(count) * 8) {
            ResponseCurve c;
            c.points.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                const f32 x = static_cast<f32>(r.u32be()) / 10000.0f;
                const f32 y = static_cast<f32>(r.u32be()) / 10000.0f;
                c.points.push_back({x, y});
            }
            if (!r.failed() && curveIsSane(c)) {
                out = std::move(c);
                return EffectorLayout::VersionCountFixed;
            }
        }
    }
    // 가설 B
    {
        ByteReader r(data, size);
        const u32 count = r.u32be();
        if (!r.failed() && count >= 1 && count <= 64 &&
            size == 4 + static_cast<usize>(count) * 8) {
            ResponseCurve c;
            c.points.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                const f32 x = r.f32be();
                const f32 y = r.f32be();
                c.points.push_back({x, y});
            }
            if (!r.failed() && curveIsSane(c)) {
                out = std::move(c);
                return EffectorLayout::CountFloat;
            }
        }
    }
    // 가설 C
    {
        ByteReader r(data, size);
        const u32 count = r.u32be();
        if (!r.failed() && count >= 1 && count <= 64 &&
            size == 4 + static_cast<usize>(count) * 4) {
            ResponseCurve c;
            c.points.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                const f32 x = static_cast<f32>(r.u16be()) / 65535.0f;
                const f32 y = static_cast<f32>(r.u16be()) / 65535.0f;
                c.points.push_back({x, y});
            }
            if (!r.failed() && curveIsSane(c)) {
                out = std::move(c);
                return EffectorLayout::CountShort;
            }
        }
    }
    return EffectorLayout::None;
}

/// effector 컬럼 이름에서 출력(무엇이 변하는가)을 읽어낸다.
bool outputFromColumnName(const std::string& lower, DynamicOutput& out) {
    struct Row {
        const char* needle;
        DynamicOutput output;
    };
    static const Row kRows[] = {
        {"size", DynamicOutput::Size},          {"thick", DynamicOutput::Roundness},
        {"opacity", DynamicOutput::Opacity},    {"alpha", DynamicOutput::Opacity},
        {"density", DynamicOutput::Flow},       {"flow", DynamicOutput::Flow},
        {"flat", DynamicOutput::Roundness},     {"round", DynamicOutput::Roundness},
        {"angle", DynamicOutput::Rotation},     {"rotat", DynamicOutput::Rotation},
        {"direction", DynamicOutput::Rotation}, {"scatter", DynamicOutput::Scatter},
        {"spray", DynamicOutput::Scatter},
    };
    for (const Row& row : kRows) {
        if (lower.find(row.needle) != std::string::npos) {
            out = row.output;
            return true;
        }
    }
    return false;
}

/// effector 컬럼 이름에서 입력(무엇이 미는가)을 읽어낸다. 못 찾으면 필압으로 가정한다.
bool inputFromColumnName(const std::string& lower, DynamicInput& out) {
    struct Row {
        const char* needle;
        DynamicInput input;
    };
    static const Row kRows[] = {
        {"pressure", DynamicInput::Pressure}, {"pen", DynamicInput::Pressure},
        {"tilt", DynamicInput::TiltX},        {"incline", DynamicInput::TiltX},
        {"speed", DynamicInput::Velocity},    {"velocity", DynamicInput::Velocity},
        {"random", DynamicInput::Random},     {"azimuth", DynamicInput::Azimuth},
        {"rotation", DynamicInput::Azimuth},  {"fade", DynamicInput::Fade},
    };
    for (const Row& row : kRows) {
        if (lower.find(row.needle) != std::string::npos) {
            out = row.input;
            return true;
        }
    }
    return false;
}

/// 색 변화 컬럼 모음(실물 컬럼 이름 기준).
struct ColorDynamicsAccumulator {
    f32 hue = 0.0f, sat = 0.0f, val = 0.0f, sub = 0.0f;
    bool perStroke = false;
};

// ── 값 매핑 ──────────────────────────────────────────────────────────────

/// 프리셋의 어느 칸에 넣을 것인가.
enum class SutField {
    Diameter,
    Opacity,
    Flow,
    Spacing,
    Hardness,
    Angle,
    Roundness,
    BlendMode,
    TextureScale,
    TextureDepth,
    TextureBlendMode,
};

/// 논리 필드 하나와, CSP 버전마다 흔들리는 컬럼 이름 후보들.
struct FieldMapping {
    SutField field;
    const char* label;          ///< 리포트에 쓸 한국어 이름
    const char* aliases[4];     ///< 소문자 컬럼 이름 후보(빈 칸은 nullptr)
};

const FieldMapping kFieldMappings[] = {
    {SutField::Diameter, "브러시 크기", {"brushsize", "brushdiameter", "brush_size", nullptr}},
    {SutField::Opacity, "불투명도", {"brushopacity", "opacity", nullptr, nullptr}},
    {SutField::Flow, "잉크 농도", {"brushdensity", "density", "brushflow", nullptr}},
    {SutField::Spacing, "간격", {"brushinterval", "interval", "brushspacing", nullptr}},
    {SutField::Hardness, "경도", {"brushhardness", "hardness", "brushedgehardness", nullptr}},
    {SutField::Angle, "각도", {"brushangle", "brushrotation", nullptr, nullptr}},
    {SutField::Roundness, "원형도", {"brushthickness", "brushflatrate", "brushroundness", "flatness"}},
    {SutField::BlendMode, "합성 모드", {"compositemode", "brushblendmode", "blendmode", "combinemode"}},
    {SutField::TextureScale, "텍스처 배율", {"texturescale2", "texturescale", "materialscale", nullptr}},
    {SutField::TextureDepth, "텍스처 세기", {"texturedensity", "texturedepth", nullptr, nullptr}},
    {SutField::TextureBlendMode,
     "텍스처 합성 모드",
     {"texturecompositemode", "textureblendmode", "texturecombinemode", nullptr}},
};

/// CSP 의 합성 모드 번호 → Mari BlendMode.
/// **이 대응은 검증되지 않은 추정이다.** 0(보통)이 아니면 리포트에 근사했다고 적는다.
bool mapSutBlendMode(int value, BlendMode& out) {
    static const BlendMode kTable[] = {
        BlendMode::Normal,   BlendMode::Multiply,  BlendMode::Add,      BlendMode::Subtract,
        BlendMode::Screen,   BlendMode::Overlay,   BlendMode::Darken,   BlendMode::Lighten,
        BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::HardLight, BlendMode::SoftLight,
        BlendMode::Difference, BlendMode::Exclusion, BlendMode::Hue,     BlendMode::Saturation,
        BlendMode::Color,    BlendMode::Luminosity,
    };
    if (value < 0 || static_cast<usize>(value) >= sizeof(kTable) / sizeof(kTable[0]))
        return false;
    out = kTable[value];
    return true;
}

/// CSP 는 0..100 으로 쓰는 값과 0..1 로 쓰는 값이 섞여 있다.
/// 1 보다 크면 퍼센트로 본다. 판단을 바꿨으면 `guessed` 를 세워 리포트에 적게 한다.
f32 normalizeRatio(f64 raw, bool& guessed) {
    guessed = raw > 1.0;
    const f64 v = guessed ? raw / 100.0 : raw;
    return static_cast<f32>(v);
}

// ── 텍스처 ───────────────────────────────────────────────────────────────

/// FileData blob 하나에서 PNG 를 꺼내 그레이스케일로 디코드한다.
bool textureFromFileData(const u8* blob, usize size, GrayImage& out, std::string& how) {
    if (blob == nullptr || size == 0)
        return false;

    // 흔한 경우: 비압축 tar 안에 PNG 가 들어 있다.
    if (looksLikeTar(blob, size)) {
        auto entries = readTar(blob, size);
        if (entries) {
            // 썸네일(thumbnail/thumbnail.png)이 소재의 실제 그림이다(.layer 는 독점 C2F). 먼저 찾는다.
            for (int pass = 0; pass < 2; ++pass) {
                for (const TarEntry& e : entries.value()) {
                    if (!e.isFile() || e.size == 0)
                        continue;
                    const bool isThumb = e.name.find("thumbnail") != std::string::npos;
                    if ((pass == 0) != isThumb)
                        continue;
                    if (!looksLikePng(blob + e.offset, e.size))
                        continue;
                    auto img = decodePngGray(blob + e.offset, e.size, false, nullptr);
                    if (img) {
                        out = std::move(img).value();
                        how = "tar 안의 " + e.name;
                        return true;
                    }
                }
            }
        }
    }
    // 드물게 PNG 가 통째로 들어 있는 경우도 있다.
    if (looksLikePng(blob, size)) {
        auto img = decodePngGray(blob, size, false, nullptr);
        if (img) {
            out = std::move(img).value();
            how = "PNG 원본";
            return true;
        }
    }
    return false;
}

/// FileData 컬럼을 가진 테이블을 전부 뒤져 텍스처 이미지를 모은다.
std::vector<GrayImage> collectTextures(sqlite3* db, const std::vector<std::string>& tables,
                                       ImportReport& report) {
    std::vector<GrayImage> out;
    for (const std::string& table : tables) {
        const auto cols = tableColumns(db, table);
        const ColumnInfo* fileData = findColumn(cols, "filedata");
        if (fileData == nullptr)
            continue;

        auto stmt = prepare(db, "SELECT " + quoteIdent(fileData->name) + " FROM " +
                                    quoteIdent(table));
        if (!stmt) {
            report.add(ImportSeverity::Dropped, table + "/FileData",
                       "텍스처 테이블 '" + table + "' 을 읽지 못했다.");
            continue;
        }
        int failures = 0;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(stmt.get(), 0);
            const int bytes = sqlite3_column_bytes(stmt.get(), 0);
            if (blob == nullptr || bytes <= 0)
                continue;
            GrayImage img;
            std::string how;
            if (textureFromFileData(static_cast<const u8*>(blob), static_cast<usize>(bytes), img,
                                    how)) {
                out.push_back(std::move(img));
            } else {
                ++failures;
            }
        }
        if (failures > 0)
            report.add(ImportSeverity::Dropped, table + "/FileData",
                       "텍스처 데이터 " + std::to_string(failures) +
                           " 건에서 PNG 를 꺼내지 못했다(tar 구조가 다르거나 다른 포맷이다).");
    }
    return out;
}

// ── 브러시 한 개 번역 ────────────────────────────────────────────────────

/// variant 행 하나에서 읽어낸 원시 값.
struct CellValue {
    int type = SQLITE_NULL;
    f64 number = 0.0;
    std::string text;
    const void* blob = nullptr;
    usize blobSize = 0;
};

/// 값이 "설정된 것"으로 볼 만한가. 0 과 NULL 은 기본값으로 보고 리포트를 아낀다.
bool isMeaningful(const CellValue& v) {
    switch (v.type) {
    case SQLITE_NULL:
        return false;
    case SQLITE_INTEGER:
    case SQLITE_FLOAT:
        return v.number != 0.0;
    case SQLITE_TEXT:
        return !v.text.empty();
    case SQLITE_BLOB:
        return v.blobSize > 0;
    default:
        return false;
    }
}

} // namespace

Result<ImportResult> importSutFile(const std::string& path) {
    sqlite3* rawDb = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &rawDb, SQLITE_OPEN_READONLY, nullptr);
    struct DbCloser {
        sqlite3* db;
        ~DbCloser() { sqlite3_close(db); }
    } closer{rawDb};
    if (rc != SQLITE_OK || rawDb == nullptr)
        return Err("sut(SQLite) 파일을 열지 못했다: " + path, ErrorCode::IoError);
    sqlite3* db = rawDb;

    ImportResult result;
    result.report.sourcePath = path;

    const std::vector<std::string> tables = listTables(db);
    if (tables.empty())
        return Err("sut 안에 테이블이 없다(깨졌거나 SQLite 가 아니다): " + path,
                   ErrorCode::ParseError);

    // ── 노드 테이블 찾기 ── 이름을 하드코딩하지 않는다.
    std::string nodeTable;
    std::vector<ColumnInfo> nodeCols;
    for (const std::string& t : tables) {
        auto cols = tableColumns(db, t);
        if (findColumn(cols, "nodename") == nullptr)
            continue;
        const bool preferred = toLower(t) == "node";
        if (nodeTable.empty() || preferred) {
            nodeTable = t;
            nodeCols = std::move(cols);
            if (preferred)
                break;
        }
    }
    if (nodeTable.empty())
        return Err("sut 안에서 NodeName 컬럼을 가진 테이블을 찾지 못했다: " + path,
                   ErrorCode::ParseError);

    const ColumnInfo* variantIdCol = findColumn(nodeCols, "nodevariantid");
    const ColumnInfo* initVariantIdCol = findColumn(nodeCols, "nodeinitvariantid");
    if (variantIdCol == nullptr && initVariantIdCol != nullptr)
        result.report.add(ImportSeverity::Degraded, "NodeVariantId",
                          "현재 설정(NodeVariantId) 컬럼이 없어 기본값(NodeInitVariantId)을 썼다.");
    if (variantIdCol == nullptr && initVariantIdCol == nullptr)
        result.report.add(ImportSeverity::Degraded, "NodeVariantId",
                          "브러시 파라미터를 가리키는 컬럼이 없다. 이름만 가져온다.");

    // ── variant 테이블 찾기 ──
    std::string variantTable;
    std::vector<ColumnInfo> variantCols;
    for (const std::string& t : tables) {
        if (t == nodeTable)
            continue;
        const std::string lower = toLower(t);
        if (lower.find("variant") == std::string::npos)
            continue;
        auto cols = tableColumns(db, t);
        if (variantTable.empty() || cols.size() > variantCols.size()) {
            variantTable = t;
            variantCols = std::move(cols);
        }
    }
    if (variantTable.empty() && (variantIdCol != nullptr || initVariantIdCol != nullptr))
        result.report.add(ImportSeverity::Degraded, "Variant",
                          "variant 테이블을 찾지 못했다. 브러시 이름만 가져온다.");

    // variant 행을 집어낼 키 컬럼. INTEGER PRIMARY KEY 가 없으면 _rowid_ 로 간다.
    std::string variantKeyExpr = "_rowid_";
    std::string variantKeyColumn;
    for (const ColumnInfo& c : variantCols) {
        if (c.primaryKey) {
            variantKeyExpr = quoteIdent(c.name);
            variantKeyColumn = c.lower;
            break;
        }
    }
    // 🔴 실물(.sut, CSP 1.x~3.x)에서 확인: Node.NodeVariantID 는 Variant._PW_ID 가 아니라
    //    Variant.VariantID 를 가리킨다. 그 컬럼이 있으면 그걸로 찾는다.
    if (const ColumnInfo* vid = findColumn(variantCols, "variantid")) {
        variantKeyExpr = quoteIdent(vid->name);
        variantKeyColumn = vid->lower;
    }

    // ── 매핑 가능한 컬럼 확인. 없는 것은 전부 리포트에 남긴다. ──
    std::unordered_map<int, const ColumnInfo*> mapped; // SutField → 컬럼
    std::unordered_set<std::string> consumed;          // 우리가 읽은 컬럼(소문자)
    if (!variantKeyColumn.empty())
        consumed.insert(variantKeyColumn); // 기본키는 파라미터가 아니다
    for (const FieldMapping& fm : kFieldMappings) {
        const ColumnInfo* found = nullptr;
        for (const char* alias : fm.aliases) {
            if (alias == nullptr)
                continue;
            found = findColumn(variantCols, alias);
            if (found != nullptr)
                break;
        }
        if (found != nullptr) {
            mapped[static_cast<int>(fm.field)] = found;
            consumed.insert(found->lower);
        } else if (!variantCols.empty()) {
            result.report.add(ImportSeverity::Info, fm.aliases[0],
                              std::string("이 CSP 버전의 .sut 에는 ") + fm.label +
                                  " 컬럼이 없다. Mari 기본값을 썼다.");
        }
    }

    // effector 컬럼은 이름으로 훑는다(버전마다 이름이 달라 목록을 못 박는다).
    std::vector<const ColumnInfo*> effectorCols;
    for (const ColumnInfo& c : variantCols) {
        if (c.lower.find("effector") == std::string::npos)
            continue;
        consumed.insert(c.lower);
        // 실물 스키마에서 주 브러시 팁에 걸리는 것만: BrushSize/Opacity/Flow/Thickness.
        // Dual*/Texture*/Spray*/Rotation*(정수 열거)/Interval/MixColor/Blur/색 변화 effector 는 여기서 빼고
        // 각자 자리에서 읽거나(회전 랜덤·스프레이) 조용히 넘긴다(그릴 수 없는 것).
        if (c.lower.rfind("dual", 0) == 0 || c.lower.rfind("texture", 0) == 0 ||
            c.lower.find("spray") != std::string::npos || c.lower.find("rotation") != std::string::npos ||
            c.lower.find("interval") != std::string::npos || c.lower.find("mixcolor") != std::string::npos ||
            c.lower.find("mixalpha") != std::string::npos || c.lower.find("blur") != std::string::npos ||
            c.lower.find("huechange") != std::string::npos || c.lower.find("saturationchange") != std::string::npos ||
            c.lower.find("valuechange") != std::string::npos || c.lower.find("subcolor") != std::string::npos)
            continue;
        effectorCols.push_back(&c);
    }
    if (effectorCols.empty() && !variantCols.empty())
        result.report.add(ImportSeverity::Info, "effector",
                          "필압 커브(*effector) 컬럼이 없다. 동적 반응은 기본값이다.");

    const std::vector<GrayImage> textures = collectTextures(db, tables, result.report);

    // ── 노드를 돌며 브러시를 만든다 ──
    std::string nodeSql = "SELECT " + quoteIdent(findColumn(nodeCols, "nodename")->name);
    nodeSql += ", " + (variantIdCol != nullptr ? quoteIdent(variantIdCol->name)
                                               : std::string("NULL"));
    nodeSql += ", " + (initVariantIdCol != nullptr ? quoteIdent(initVariantIdCol->name)
                                                   : std::string("NULL"));
    nodeSql += " FROM " + quoteIdent(nodeTable);

    auto nodeStmt = prepare(db, nodeSql);
    if (!nodeStmt)
        return Err("노드 테이블을 읽지 못했다: " + std::string(sqlite3_errmsg(db)),
                   ErrorCode::ParseError);

    StmtPtr variantStmt;
    if (!variantTable.empty()) {
        variantStmt = prepare(db, "SELECT * FROM " + quoteIdent(variantTable) + " WHERE " +
                                      variantKeyExpr + " = ?");
        if (!variantStmt)
            result.report.add(ImportSeverity::Degraded, variantTable,
                              "variant 테이블을 질의하지 못했다. 브러시 이름만 가져온다.");
    }

    int index = 0;
    while (sqlite3_step(nodeStmt.get()) == SQLITE_ROW && index < kMaxBrushes) {
        MariBrushPreset preset;
        preset.sourceFormat = "sut";

        const unsigned char* nameText = sqlite3_column_text(nodeStmt.get(), 0);
        preset.name = nameText != nullptr ? reinterpret_cast<const char*>(nameText)
                                          : ("sut 브러시 " + std::to_string(index + 1));
        if (preset.name.empty())
            preset.name = "sut 브러시 " + std::to_string(index + 1);

        i64 variantId = 0;
        bool haveVariantId = false;
        if (sqlite3_column_type(nodeStmt.get(), 1) != SQLITE_NULL) {
            variantId = sqlite3_column_int64(nodeStmt.get(), 1);
            haveVariantId = true;
        } else if (sqlite3_column_type(nodeStmt.get(), 2) != SQLITE_NULL) {
            variantId = sqlite3_column_int64(nodeStmt.get(), 2);
            haveVariantId = true;
            result.report.add(ImportSeverity::Info, preset.name,
                              "'" + preset.name +
                                  "' 은 현재 설정이 비어 있어 기본 설정(NodeInitVariantId)을 읽었다.");
        }
        preset.sourceId = haveVariantId ? std::to_string(variantId) : std::to_string(index);

        std::vector<std::pair<std::string, CellValue>> row; // 컬럼 소문자 이름 → 값
        if (haveVariantId && variantStmt) {
            sqlite3_reset(variantStmt.get());
            sqlite3_clear_bindings(variantStmt.get());
            sqlite3_bind_int64(variantStmt.get(), 1, variantId);
            if (sqlite3_step(variantStmt.get()) == SQLITE_ROW) {
                const int n = sqlite3_column_count(variantStmt.get());
                row.reserve(static_cast<usize>(n));
                for (int i = 0; i < n; ++i) {
                    CellValue v;
                    v.type = sqlite3_column_type(variantStmt.get(), i);
                    switch (v.type) {
                    case SQLITE_INTEGER:
                    case SQLITE_FLOAT:
                        v.number = sqlite3_column_double(variantStmt.get(), i);
                        break;
                    case SQLITE_TEXT: {
                        const unsigned char* t = sqlite3_column_text(variantStmt.get(), i);
                        if (t != nullptr)
                            v.text = reinterpret_cast<const char*>(t);
                        break;
                    }
                    case SQLITE_BLOB: {
                        v.blob = sqlite3_column_blob(variantStmt.get(), i);
                        const int bytes = sqlite3_column_bytes(variantStmt.get(), i);
                        v.blobSize = bytes > 0 ? static_cast<usize>(bytes) : 0;
                        break;
                    }
                    default:
                        break;
                    }
                    const char* cname = sqlite3_column_name(variantStmt.get(), i);
                    row.emplace_back(toLower(cname != nullptr ? cname : ""), std::move(v));
                }
            } else {
                result.report.add(ImportSeverity::Degraded, "NodeVariantId",
                                  "'" + preset.name + "' 이 가리키는 파라미터 행(" +
                                      std::to_string(variantId) + ")이 없다. 기본값을 썼다.");
            }
        }

        auto cell = [&row](const std::string& lowerName) -> const CellValue* {
            for (const auto& kv : row)
                if (kv.first == lowerName)
                    return &kv.second;
            return nullptr;
        };
        auto mappedCell = [&](SutField f) -> const CellValue* {
            const auto it = mapped.find(static_cast<int>(f));
            if (it == mapped.end())
                return nullptr;
            return cell(it->second->lower);
        };

        // ── 스칼라 파라미터 ──
        if (const CellValue* v = mappedCell(SutField::Diameter)) {
            if (v->number > 0.0)
                preset.tip.diameter = static_cast<f32>(v->number);
        }
        if (const CellValue* v = mappedCell(SutField::Opacity)) {
            bool guessed = false;
            preset.opacity = std::clamp(normalizeRatio(v->number, guessed), 0.0f, 1.0f);
        }
        if (const CellValue* v = mappedCell(SutField::Flow)) {
            bool guessed = false;
            preset.flow = std::clamp(normalizeRatio(v->number, guessed), 0.0f, 1.0f);
        }
        if (const CellValue* v = mappedCell(SutField::Spacing)) {
            bool guessed = false;
            const f32 s = normalizeRatio(v->number, guessed);
            preset.spacing = std::clamp(s, 0.01f, 10.0f);
            if (guessed)
                result.report.add(ImportSeverity::Degraded, "BrushInterval",
                                  "'" + preset.name + "' 의 간격 값 " +
                                      std::to_string(v->number) +
                                      " 을 퍼센트로 보고 " + std::to_string(preset.spacing) +
                                      " (지름 대비 비율)로 변환했다. CSP 버전에 따라 단위가 "
                                      "다를 수 있다.");
        }
        if (const CellValue* v = mappedCell(SutField::Hardness)) {
            bool guessed = false;
            preset.tip.hardness = std::clamp(normalizeRatio(v->number, guessed), 0.0f, 1.0f);
        }
        if (const CellValue* v = mappedCell(SutField::Angle)) {
            // 실물 관찰: CSP 의 BrushRotation 90 이 "똑바로 선" 스탬프다(세로축 기준 각). Mari 는 가로축 기준.
            const ColumnInfo* col = mapped[static_cast<int>(SutField::Angle)];
            const bool csp = col != nullptr && col->lower == "brushrotation";
            preset.tip.angle = static_cast<f32>(v->number) - (csp ? 90.0f : 0.0f);
        }
        if (const CellValue* v = mappedCell(SutField::Roundness)) {
            bool guessed = false;
            const f32 r = std::clamp(normalizeRatio(v->number, guessed), 0.0f, 1.0f);
            const ColumnInfo* col = mapped[static_cast<int>(SutField::Roundness)];
            // BrushThickness(실물): 100 = 원. 옛 이름 "flat/flatness" 는 반대(클수록 납작).
            const bool thickness = col != nullptr && col->lower.find("thick") != std::string::npos;
            preset.tip.aspectRatio = std::clamp(thickness ? r : 1.0f - r, 0.01f, 1.0f);
        }
        if (const CellValue* v = mappedCell(SutField::BlendMode)) {
            const int mode = static_cast<int>(v->number);
            BlendMode blend = BlendMode::Normal;
            if (mapSutBlendMode(mode, blend)) {
                preset.blendMode = blend;
                if (mode != 0)
                    result.report.add(ImportSeverity::Degraded, "BrushBlendMode",
                                      "'" + preset.name + "' 의 합성 모드 번호 " +
                                          std::to_string(mode) + " 을 '" + blendModeName(blend) +
                                          "' 로 봤다. CSP 의 번호 대응은 검증되지 않았다.");
            } else {
                result.report.add(ImportSeverity::Dropped, "BrushBlendMode",
                                  "'" + preset.name + "' 의 합성 모드 번호 " +
                                      std::to_string(mode) + " 을 모른다. 보통으로 뒀다.");
            }
        }

        // ── 동적 반응(effector blob) ──
        for (const ColumnInfo* col : effectorCols) {
            const CellValue* v = cell(col->lower);
            if (v == nullptr || !isMeaningful(*v))
                continue;

            DynamicOutput output = DynamicOutput::Size;
            if (!outputFromColumnName(col->lower, output)) {
                result.report.add(ImportSeverity::Dropped, col->name,
                                  "'" + preset.name + "' 의 " + col->name +
                                      " 이 무엇을 바꾸는 값인지 몰라 반영하지 못했다.");
                continue;
            }
            DynamicInput input = DynamicInput::Pressure;
            const bool namedInput = inputFromColumnName(col->lower, input);

            if (v->type != SQLITE_BLOB) {
                // 커브가 아니라 단일 세기 값인 경우. 선형 커브로 근사한다.
                bool guessed = false;
                const f32 amount = std::clamp(normalizeRatio(v->number, guessed), 0.0f, 1.0f);
                DynamicLink link;
                link.input = input;
                link.output = output;
                link.curve.points = {{0.0f, 1.0f - amount}, {1.0f, 1.0f}};
                preset.dynamics.push_back(std::move(link));
                result.report.add(ImportSeverity::Degraded, col->name,
                                  "'" + preset.name + "' 의 " + col->name +
                                      " 은 커브가 아니라 단일 값이라 직선 반응으로 근사했다.");
                continue;
            }

            RealEffector real;
            if (parseRealEffector(static_cast<const u8*>(v->blob), v->blobSize, real)) {
                const bool additive = output == DynamicOutput::Rotation || output == DynamicOutput::Scatter;
                if (!real.curves.empty()) {
                    DynamicLink link;
                    link.input = DynamicInput::Pressure;
                    link.output = output;
                    link.curve = real.curves.front();
                    if (!additive) {
                        // CSP 최솟값: 출력 = min + (1 − min)·curve(p)
                        for (auto& pt : link.curve.points)
                            pt.y = real.minimum + (1.0f - real.minimum) * pt.y;
                    }
                    preset.dynamics.push_back(std::move(link));
                }
                if (real.randomAmount > 0.0f) {
                    DynamicLink link;
                    link.input = DynamicInput::Random;
                    link.output = output;
                    if (additive)
                        link.curve.points = {{0.0f, 0.0f}, {1.0f, real.randomAmount}};
                    else
                        link.curve.points = {{0.0f, 1.0f - real.randomAmount}, {1.0f, 1.0f}};
                    preset.dynamics.push_back(std::move(link));
                }
                if (real.curves.size() > 1)
                    result.report.add(ImportSeverity::Degraded, col->name,
                                      "'" + preset.name + "' 의 " + col->name + " 에 커브가 " +
                                          std::to_string(real.curves.size()) +
                                          " 개 있다. 첫 커브(필압)만 반영했고 나머지(속도·기울기로 추정)는 뺐다.");
                continue;
            }

            ResponseCurve curve;
            const EffectorLayout layout =
                parseEffectorCurve(static_cast<const u8*>(v->blob), v->blobSize, curve);
            if (layout == EffectorLayout::None) {
                // **여기서 죽지 않는 게 요구사항이다.** 커브만 버리고 브러시는 살린다.
                result.report.add(ImportSeverity::Dropped, col->name,
                                  "'" + preset.name + "' 의 필압 커브(" + col->name + ", " +
                                      std::to_string(v->blobSize) +
                                      "바이트)를 해석하지 못했다. 팁과 기본 파라미터는 그대로 "
                                      "가져왔고, 커브는 기본값이다.");
                continue;
            }

            DynamicLink link;
            link.input = input;
            link.output = output;
            link.curve = std::move(curve);
            preset.dynamics.push_back(std::move(link));
            result.report.add(ImportSeverity::Info, col->name,
                              "'" + preset.name + "' 의 " + col->name + " 커브를 " +
                                  effectorLayoutName(layout) + " 로 해석했다.");
            if (!namedInput)
                result.report.add(ImportSeverity::Degraded, col->name,
                                  "'" + preset.name + "' 의 " + col->name +
                                      " 은 어떤 입력이 미는 값인지 이름으로 알 수 없어 "
                                      "필압으로 가정했다.");
        }

        // ── 텍스처 ──
        // 실물 .sut: MaterialFile.FileData 는 **팁 패턴 소재**다(BrushUsePatternImage=1).
        // TextureImage 가 비어 있지 않을 때만 텍스처로 본다.
        bool usePattern = false;
        if (const CellValue* v = cell("brushusepatternimage")) {
            usePattern = v->number != 0.0;
            consumed.insert("brushusepatternimage");
        }
        consumed.insert("brushpatternimagearray");
        consumed.insert("brushpatternnameversion");
        bool hasTextureImage = false;
        if (const CellValue* v = cell("textureimage")) {
            hasTextureImage = v->type == SQLITE_BLOB ? v->blobSize > 0 : !v->text.empty();
            consumed.insert("textureimage");
        }
        // 옛/합성 스키마(플래그 컬럼이 아예 없다)면 예전처럼 FileData 를 텍스처로 본다.
        const bool legacySchema = cell("brushusepatternimage") == nullptr && cell("textureimage") == nullptr;
        if (usePattern && !textures.empty()) {
            const usize pick = static_cast<usize>(index) < textures.size() ? static_cast<usize>(index) : 0;
            preset.tip.kind = TipKind::Bitmap;
            preset.tip.bitmap = textures[pick];
            // 소재 그림은 "흰 바탕에 어두운 잉크"(투명은 흰색으로 깔림) → 잉크 = 255 − 밝기.
            for (u8& px : preset.tip.bitmap.pixels)
                px = static_cast<u8>(255u - px);
            result.report.add(ImportSeverity::Degraded, "MaterialFile",
                              "'" + preset.name + "' 의 팁은 소재 썸네일(PNG)에서 가져왔다. 원본 .layer(C2F)는 "
                                                "독점 포맷이라 읽지 않는다 — 해상도가 썸네일(보통 300px)로 제한된다.");
        }
        const bool wantsTexture = hasTextureImage || (legacySchema && (mapped.count(static_cast<int>(SutField::TextureScale)) != 0 ||
                                                                       mapped.count(static_cast<int>(SutField::TextureDepth)) != 0));
        if (wantsTexture && !textures.empty() && !usePattern) {
            BrushTexture tex;
            if (const CellValue* v = mappedCell(SutField::TextureScale)) {
                bool guessed = false;
                const f32 s = normalizeRatio(v->number, guessed);
                if (s > 0.0f)
                    tex.scale = s;
            }
            if (const CellValue* v = mappedCell(SutField::TextureDepth)) {
                bool guessed = false;
                tex.depth = std::clamp(normalizeRatio(v->number, guessed), 0.0f, 1.0f);
            }
            if (const CellValue* v = mappedCell(SutField::TextureBlendMode)) {
                BlendMode blend = BlendMode::Multiply;
                if (mapSutBlendMode(static_cast<int>(v->number), blend))
                    tex.blendMode = blend;
            }
            const usize pick = static_cast<usize>(index) < textures.size()
                                   ? static_cast<usize>(index)
                                   : 0;
            tex.image = textures[pick];
            preset.texture = std::move(tex);
            if (textures.size() != 1 && static_cast<usize>(index) >= textures.size())
                result.report.add(ImportSeverity::Degraded, "FileData",
                                  "'" + preset.name +
                                      "' 에 붙일 텍스처를 특정하지 못해 파일의 첫 텍스처를 썼다.");
        } else if (wantsTexture) {
            result.report.add(ImportSeverity::Dropped, "FileData",
                              "'" + preset.name +
                                  "' 은 텍스처 설정이 있지만 텍스처 이미지를 꺼내지 못했다.");
        }

        // ── 흩뿌림(스프레이) ──
        if (const CellValue* v = cell("brushusespray")) {
            consumed.insert("brushusespray");
            if (v->number != 0.0) {
                f32 sprayPx = preset.tip.diameter;
                if (const CellValue* sz = cell("brushspraysize")) sprayPx = static_cast<f32>(sz->number);
                bool sync = false;
                if (const CellValue* sy = cell("brushspraysizesyncbrushsize")) sync = sy->number != 0.0;
                // 스프레이 반경(지름 대비 비율). 동기화면 브러시 크기와 같이 커진다 — 비율은 그대로.
                const f32 ratio = preset.tip.diameter > 0.0f ? sprayPx / preset.tip.diameter : 1.0f;
                DynamicLink link;
                link.input = DynamicInput::Random;
                link.output = DynamicOutput::Scatter;
                link.curve.points = {{0.0f, 0.0f}, {1.0f, std::clamp(ratio, 0.0f, 8.0f)}};
                preset.dynamics.push_back(std::move(link));
                if (const CellValue* d = cell("brushspraydensity"))
                    preset.scatterCount = std::clamp(static_cast<i32>(d->number), 1, 16);
                (void)sync;
            }
        }
        for (const char* k : {"brushspraysize", "brushspraysizeunit", "brushspraysizesyncbrushsize", "brushspraydensity",
                              "brushspraybias", "brushsprayusefixedpoint", "brushsprayfixedpointarray",
                              "brushspraysizeeffector", "brushspraydensityeffector"})
            consumed.insert(k);
        // BrushRotationEffector(정수 열거)·RandomScale 은 실물 5개가 전부 기본값(3, 100)이라 의미를 못 박지 못했다.
        // 랜덤 회전으로 넘겨짚지 않는다 — 조용히 넘긴다.
        consumed.insert("brushrotationrandomscale");
        consumed.insert("brushrotationeffector");

        // ── 물 가장자리 → 젖은 가장자리 ──
        if (const CellValue* v = cell("brushusewateredge")) {
            preset.wetEdges = v->number != 0.0;
            consumed.insert("brushusewateredge");
        }

        // ── 색 변화 ──
        {
            ColorDynamicsAccumulator cd;
            if (const CellValue* v = cell("brushhuechange")) { cd.hue = static_cast<f32>(v->number) / 100.0f; consumed.insert("brushhuechange"); }
            if (const CellValue* v = cell("brushsaturationchange")) { cd.sat = static_cast<f32>(v->number) / 100.0f; consumed.insert("brushsaturationchange"); }
            if (const CellValue* v = cell("brushvaluechange")) { cd.val = static_cast<f32>(v->number) / 100.0f; consumed.insert("brushvaluechange"); }
            if (const CellValue* v = cell("brushsubcolor")) { cd.sub = static_cast<f32>(v->number) / 100.0f; consumed.insert("brushsubcolor"); }
            if (const CellValue* v = cell("brushchangestrokecolor")) { cd.perStroke = v->number != 0.0; consumed.insert("brushchangestrokecolor"); }
            preset.colorDynamics.hueJitter = std::clamp(cd.hue, 0.0f, 1.0f);
            preset.colorDynamics.saturationJitter = std::clamp(cd.sat, 0.0f, 1.0f);
            preset.colorDynamics.brightnessJitter = std::clamp(cd.val, 0.0f, 1.0f);
            preset.colorDynamics.fgBgJitter = std::clamp(cd.sub, 0.0f, 1.0f);
            preset.colorDynamics.perTip = true;
            for (const char* k : {"brushstrokehuechange", "brushstrokesaturationchange", "brushstrokevaluechange",
                                  "brushstrokesubcolor", "brushchangecolortarget", "brushchangepatterncolor"})
                consumed.insert(k);
        }

        // ── 듀얼 브러시 ──
        if (const CellValue* v = cell("usedualbrush")) {
            consumed.insert("usedualbrush");
            if (v->number != 0.0) {
                DualBrush dual;
                if (const CellValue* c = cell("dualsize")) dual.tip.diameter = static_cast<f32>(c->number);
                if (const CellValue* c = cell("dualhardness")) dual.tip.hardness = std::clamp(static_cast<f32>(c->number) / 100.0f, 0.0f, 1.0f);
                if (const CellValue* c = cell("dualthickness")) dual.tip.aspectRatio = std::clamp(static_cast<f32>(c->number) / 100.0f, 0.01f, 1.0f);
                if (const CellValue* c = cell("dualrotation")) dual.tip.angle = static_cast<f32>(c->number);
                if (const CellValue* c = cell("dualinterval")) dual.spacing = std::clamp(static_cast<f32>(c->number) / 100.0f, 0.01f, 10.0f);
                if (const CellValue* c = cell("dualbrushcompositemode")) {
                    BlendMode m = BlendMode::Multiply;
                    if (mapSutBlendMode(static_cast<int>(c->number), m)) dual.blendMode = m;
                }
                preset.dual = std::move(dual);
            }
        }
        for (const auto& [lowerName, value] : row)
            if (lowerName.rfind("dual", 0) == 0 || lowerName.rfind("fill", 0) == 0)
                consumed.insert(lowerName);

        // ── 그릴 것과 무관한 UI/입력 설정 — 조용히 넘긴다 ──
        for (const char* k : {"_pw_id", "changergbbydual", "syncdualbrushsize", "dualantialias",
                              "variantshowseparator", "variantshowparam", "antialias", "flickerreduction",
                              "flickerreductionbyspeed", "flickerreductionbyspeedtype", "stickness",
                              "brushsizeunit", "brushsizesyncviewscale", "brushatleast1pixel", "brushadjustflowbyinterval",
                              "brushautointervaltype", "brushcontinuousplot", "brushverticalthicknes", "brushrotationinspray",
                              "brushrotationeffectorinspray", "brushrotationrandominspray", "brushpatternordertype",
                              "brushpatternordertype2", "brushpatternreversehorizontal", "brushpatternreversevertical",
                              "textureforplot", "texturereversedensity", "texturestressdensity", "texturerotate",
                              "texturebrightness", "texturecontrast", "brushusewatercolor", "brushusewatercolor2",
                              "brushwatercolor", "brushmixcolor", "brushmixalpha", "brushmixcolorextension",
                              "brushblurlinksize", "brushblur", "brushblurunit", "brushribbon", "brushblendpatternbydarken",
                              "brushuserevision", "brushrevision", "brushrevisionbyspeed", "brushrevisionbyviewscale",
                              "brushrevisionbezier", "brushinouttarget", "brushinouttype", "brushinoutbyspeed",
                              "brushusein", "brushinlength", "brushinlengthunit", "brushinratio", "brushuseout",
                              "brushoutlength", "brushoutlengthunit", "brushoutratio", "brushsharpencorner",
                              "brushwateredgeradius", "brushwateredgeradiusunit", "brushwateredgealphapower",
                              "brushwateredgevaluepower", "brushwateredgeafterdrag", "brushwateredgeblur",
                              "brushwateredgeblurunit", "brushusevectoreraser", "brushvectorerasertype",
                              "brushvectoreraserreferalllayer", "brusheraselllayer", "brusheraseallayer",
                              "brusheraseall_layer", "brusheraseallayer", "brushenablesnap", "brushusevectormagnet",
                              "brushvectormagnetpower", "brushusereferlayer", "brushadjustvelocity"})
            consumed.insert(k);

        // ── 번역 못 한 컬럼 보고 ── 조용히 버리지 않는다.
        usize emitted = 0;
        usize skipped = 0;
        for (const auto& [lowerName, value] : row) {
            if (consumed.count(lowerName) != 0 || !isMeaningful(value))
                continue;
            if (lowerName == "_rowid_")
                continue;
            if (emitted >= kMaxUnmappedNotes) {
                ++skipped;
                continue;
            }
            result.report.add(ImportSeverity::Dropped, lowerName,
                              "'" + preset.name + "' 의 설정 " + lowerName +
                                  " 은 Mari 로 번역하지 못해 반영되지 않았다.");
            ++emitted;
        }
        if (skipped > 0)
            result.report.add(ImportSeverity::Dropped, variantTable,
                              "'" + preset.name + "' 에서 위 외에 " + std::to_string(skipped) +
                                  " 개의 설정을 더 번역하지 못했다.");

        result.presets.push_back(std::move(preset));
        ++index;
    }

    if (result.presets.empty())
        return Err("sut 에서 브러시를 하나도 읽지 못했다: " + path, ErrorCode::ParseError);

    result.report.presetCount = result.presets.size();
    return Ok(std::move(result));
}

} // namespace mari::io::brush
