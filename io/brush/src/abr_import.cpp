// Mari Paint — Photoshop .abr 임포터.
//
// 레이아웃(CS2 이후 6.x):
//     u16 version | u16 subversion | ("8BIM" + 4바이트 key + u32 length + data)*
//   key = "samp" 팁 비트맵 / "desc" 동작 파라미터(ActionDescriptor) / "patt" 텍스처 패턴
//
// 공식 스펙이 없다. 그래서 두 가지를 지킨다.
//   1) **관대하게 읽는다** — 모르는 섹션·모르는 키를 만나도 멈추지 않는다.
//   2) **정직하게 실패한다** — 읽었지만 번역하지 못한 키를 전부 ImportReport 에 남긴다.
//      (Krita 가 텍스처만 가져오고 아무 말 없이 나머지를 버려서 욕먹는다 — docs/01 2.1)
#include <mari/io/brush/importer.hpp>

#include <mari/io/brush/abr_descriptor.hpp>
#include <mari/io/brush/byte_reader.hpp>
#include <mari/io/brush/png_gray.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace mari::io::brush {

using mari::brush::BrushTexture;
using mari::brush::DynamicInput;
using mari::brush::DynamicLink;
using mari::brush::DynamicOutput;
using mari::brush::GrayImage;
using mari::brush::ImportReport;
using mari::brush::ImportSeverity;
using mari::brush::MariBrushPreset;
using mari::brush::TipKind;

namespace {

/// 브러시 하나당 남길 "번역 못 한 키" 노트 상한. 넘치면 요약 한 줄로 접는다.
constexpr usize kMaxUnmappedNotes = 40;
/// 팁 비트맵 한 변의 상한. 이보다 크면 깨진 파일로 본다.
constexpr i32 kMaxTipDim = 8192;

// ── samp 섹션: 팁 비트맵 ──────────────────────────────────────────────────

/// samp 섹션에서 읽어낸 팁 하나.
struct SampBrush {
    std::string name; ///< 보통 UUID 꼴. desc 의 sampledData 와 이걸로 잇는다
    GrayImage mask;
    u16 spacing = 25; ///< samp 가 직접 들고 있는 간격(%)
    bool antialias = false;
};

/// PackBits(RLE) 한 행을 푼다. 성공하면 true.
bool unpackBits(ByteReader& r, u8* dst, usize width, usize packedLen) {
    const usize end = r.pos() + packedLen;
    if (!r.has(packedLen))
        return false;
    usize x = 0;
    while (r.pos() < end && x < width) {
        const i8 n = static_cast<i8>(r.u8v());
        if (n >= 0) {
            const usize count = static_cast<usize>(n) + 1;
            if (x + count > width || !r.has(count))
                return false;
            for (usize i = 0; i < count; ++i)
                dst[x++] = r.u8v();
        } else if (n != -128) {
            const usize count = static_cast<usize>(-static_cast<int>(n)) + 1;
            if (x + count > width || !r.has(1))
                return false;
            const u8 v = r.u8v();
            for (usize i = 0; i < count; ++i)
                dst[x++] = v;
        }
    }
    // 행이 덜 찼으면 남는 부분은 "잉크 없음"으로 둔다. 잘린 파일에서도 그림은 나온다.
    while (x < width)
        dst[x++] = 0;
    r.seek(end);
    return !r.failed();
}

/// samp 섹션 하나를 파싱한다. 실패한 브러시는 건너뛰고 리포트에 남긴다.
std::vector<SampBrush> parseSampSection(ByteReader& r, ImportReport& report) {
    std::vector<SampBrush> out;
    while (r.remaining() >= 4) {
        const u32 brushLen = r.u32be();
        const usize brushStart = r.pos();
        if (brushLen == 0 || !r.has(brushLen)) {
            report.add(ImportSeverity::Dropped, "samp",
                       "팁 비트맵 항목의 길이가 파일 밖을 가리켜 나머지를 읽지 못했다.");
            break;
        }
        usize brushEnd = brushStart + brushLen;
        if (brushEnd % 4 != 0)
            brushEnd += 4 - brushEnd % 4;
        if (brushEnd > r.size())
            brushEnd = r.size();

        SampBrush b;
        (void)r.u32be(); // 용도 불명 필드. 값을 쓰지 않는다
        b.spacing = r.u16be();
        b.name = r.unicodeString();
        b.antialias = r.u8v() != 0;
        (void)r.i16be(); // 짧은 bounds(top)
        (void)r.i16be();
        (void)r.i16be();
        (void)r.i16be();
        const i32 top = r.i32be();
        const i32 left = r.i32be();
        const i32 bottom = r.i32be();
        const i32 right = r.i32be();
        const u16 depth = r.u16be();
        const u8 compression = r.u8v();

        if (r.failed()) {
            report.add(ImportSeverity::Dropped, "samp", "팁 비트맵 머리말이 잘렸다.");
            break;
        }

        const i64 w = static_cast<i64>(right) - left;
        const i64 h = static_cast<i64>(bottom) - top;
        if (w <= 0 || h <= 0 || w > kMaxTipDim || h > kMaxTipDim) {
            report.add(ImportSeverity::Dropped, "samp/bounds",
                       "팁 '" + b.name + "' 의 크기가 비정상이라 건너뛴다.");
            r.seek(brushEnd);
            continue;
        }
        if (depth != 8) {
            report.add(ImportSeverity::Dropped, "samp/depth",
                       "팁 '" + b.name + "' 의 비트 깊이 " + std::to_string(depth) +
                           " 는 아직 지원하지 않는다(8비트만 읽는다).");
            r.seek(brushEnd);
            continue;
        }

        const usize width = static_cast<usize>(w);
        const usize height = static_cast<usize>(h);
        std::vector<u8> raw(width * height, 0);
        bool ok = true;
        if (compression == 0) {
            if (!r.has(width * height)) {
                ok = false;
            } else {
                auto bytes = r.bytes(width * height);
                raw.swap(bytes);
            }
        } else if (compression == 1) {
            std::vector<u16> rowLens(height, 0);
            for (usize y = 0; y < height && ok; ++y) {
                rowLens[y] = r.u16be();
                ok = !r.failed();
            }
            for (usize y = 0; y < height && ok; ++y)
                ok = unpackBits(r, raw.data() + y * width, width, rowLens[y]);
        } else {
            report.add(ImportSeverity::Dropped, "samp/compression",
                       "팁 '" + b.name + "' 의 압축 방식 " + std::to_string(compression) +
                           " 을 모른다.");
            r.seek(brushEnd);
            continue;
        }

        if (!ok) {
            report.add(ImportSeverity::Dropped, "samp/data",
                       "팁 '" + b.name + "' 의 픽셀이 잘려 건너뛴다.");
            r.seek(brushEnd);
            if (r.failed())
                break;
            continue;
        }

        // .abr 의 팁은 "검을수록 잉크가 많다". Mari 의 GrayImage 는 반대 규약이라 뒤집는다.
        b.mask.width = static_cast<i32>(width);
        b.mask.height = static_cast<i32>(height);
        b.mask.pixels.resize(width * height);
        for (usize i = 0; i < raw.size(); ++i)
            b.mask.pixels[i] = static_cast<u8>(255u - raw[i]);

        out.push_back(std::move(b));
        r.seek(brushEnd);
        if (r.failed())
            break;
    }
    return out;
}

// ── patt 섹션: 텍스처 패턴 ────────────────────────────────────────────────

/// patt 섹션에서 읽어낸 패턴 하나.
struct PattEntry {
    std::string id;   ///< 파스칼 문자열 ID. desc 의 Txtr/Idnt 와 잇는다
    std::string name;
    GrayImage image;
};

/// 패턴 채널 하나를 읽는다(raw 또는 RLE).
bool readPatternChannel(ByteReader& r, usize width, usize height, std::vector<u8>& out) {
    const u32 written = r.u32be();
    const u32 length = r.u32be();
    if (r.failed())
        return false;
    const usize channelEnd = r.pos() + length;
    if (written == 0 || length == 0) {
        r.seek(channelEnd);
        return false;
    }
    if (!r.has(length))
        return false;

    (void)r.u32be(); // pixel depth(32비트 필드)
    (void)r.i32be(); // rect top
    (void)r.i32be(); // left
    (void)r.i32be(); // bottom
    (void)r.i32be(); // right
    const u16 depth = r.u16be();
    const u8 compression = r.u8v();
    if (r.failed() || depth != 8) {
        r.seek(channelEnd);
        return false;
    }

    out.assign(width * height, 0);
    bool ok = true;
    if (compression == 0) {
        if (!r.has(width * height))
            ok = false;
        else {
            auto bytes = r.bytes(width * height);
            out.swap(bytes);
        }
    } else if (compression == 1) {
        std::vector<u16> rowLens(height, 0);
        for (usize y = 0; y < height && ok; ++y) {
            rowLens[y] = r.u16be();
            ok = !r.failed();
        }
        for (usize y = 0; y < height && ok; ++y)
            ok = unpackBits(r, out.data() + y * width, width, rowLens[y]);
    } else {
        ok = false;
    }
    r.seek(channelEnd);
    return ok && !r.failed();
}

/// 패턴 하나를 파싱한다.
bool parseOnePattern(ByteReader& r, PattEntry& out) {
    const u32 version = r.u32be();
    const u32 imageMode = r.u32be();
    const u16 pointV = r.u16be();
    const u16 pointH = r.u16be();
    out.name = r.unicodeString();
    const u8 idLen = r.u8v();
    out.id = r.ascii(idLen);
    if (r.failed() || version != 1)
        return false;
    if (imageMode == 2) // 인덱스 컬러 — 컬러 테이블 길이가 가변이라 손대지 않는다
        return false;
    if (pointV == 0 || pointH == 0)
        return false;

    (void)r.u32be(); // 가상 메모리 배열 리스트 버전
    (void)r.u32be(); // 길이
    const i32 top = r.i32be();
    const i32 left = r.i32be();
    const i32 bottom = r.i32be();
    const i32 right = r.i32be();
    const u32 channelCount = r.u32be();
    if (r.failed())
        return false;

    const i64 w = static_cast<i64>(right) - left;
    const i64 h = static_cast<i64>(bottom) - top;
    if (w <= 0 || h <= 0 || w > kMaxTipDim || h > kMaxTipDim || channelCount == 0 ||
        channelCount > 8)
        return false;

    const usize width = static_cast<usize>(w);
    const usize height = static_cast<usize>(h);
    std::vector<u8> first;
    bool got = false;
    for (u32 c = 0; c < channelCount; ++c) {
        std::vector<u8> chan;
        const bool ok = readPatternChannel(r, width, height, chan);
        if (ok && !got) {
            first.swap(chan);
            got = true;
        }
        if (r.failed())
            break;
    }
    if (!got)
        return false;

    out.image.width = static_cast<i32>(width);
    out.image.height = static_cast<i32>(height);
    out.image.pixels.swap(first);
    return true;
}

std::vector<PattEntry> parsePattSection(ByteReader& r, ImportReport& report) {
    std::vector<PattEntry> out;
    while (r.remaining() >= 4) {
        const u32 len = r.u32be();
        if (len == 0 || !r.has(len))
            break;
        const usize start = r.pos();
        usize end = start + len;
        if (end % 4 != 0)
            end += 4 - end % 4;
        if (end > r.size())
            end = r.size();

        ByteReader sub(r.data() + start, len);
        PattEntry e;
        if (parseOnePattern(sub, e))
            out.push_back(std::move(e));
        else
            report.add(ImportSeverity::Dropped, "patt",
                       "텍스처 패턴 하나를 해석하지 못했다. 이 브러시의 텍스처는 비어 있다.");
        r.seek(end);
        if (r.failed())
            break;
    }
    return out;
}

// ── desc 섹션: 동작 번역 ──────────────────────────────────────────────────

/// 디스크립터를 읽으면서 "우리가 실제로 쓴 키"를 기록한다.
/// 끝나고 안 쓴 키를 리포트에 남기기 위한 장치다 — 조용히 버리지 않으려면 이게 필요하다.
class TrackedDescriptor {
public:
    TrackedDescriptor(const Descriptor& d, std::string prefix, std::set<std::string>& used)
        : d_(d), prefix_(std::move(prefix)), used_(used) {}

    [[nodiscard]] std::string path(std::string_view key) const {
        return prefix_.empty() ? std::string(key) : prefix_ + "/" + std::string(key);
    }

    /// 키를 읽고 "사용함"으로 표시한다.
    [[nodiscard]] const DescValue* get(std::string_view key) {
        const DescValue* v = d_.find(key);
        if (v != nullptr)
            used_.insert(path(key));
        return v;
    }

    /// 여러 별칭 중 먼저 걸리는 것. 포맷 버전마다 키 이름이 흔들려서 필요하다.
    [[nodiscard]] const DescValue* getAny(std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (const DescValue* v = get(k))
                return v;
        return nullptr;
    }

    /// 하위 디스크립터로 내려간다. 컨테이너 키도 "사용함"으로 표시된다.
    [[nodiscard]] const Descriptor* child(std::string_view key) {
        const DescValue* v = get(key);
        if (v == nullptr || v->type != DescType::Descriptor || !v->descriptor)
            return nullptr;
        return v->descriptor.get();
    }

    [[nodiscard]] const Descriptor& raw() const { return d_; }
    [[nodiscard]] const std::string& prefix() const { return prefix_; }
    [[nodiscard]] std::set<std::string>& used() const { return used_; }

private:
    const Descriptor& d_;
    std::string prefix_;
    std::set<std::string>& used_;
};

/// UnitFloat/Double 을 퍼센트로 읽어 0..1 비율로 돌려준다.
/// 단위가 '#Prc' 가 아니면 approximate 를 세워 호출자가 리포트하게 한다.
f32 percentToRatio(const DescValue& v, bool& approximate) {
    approximate = (v.type == DescType::UnitFloat && v.unit != "#Prc");
    return static_cast<f32>(v.asNumber() / 100.0);
}

/// 포토샵 블렌드 키 → Mari BlendMode.
bool mapBlendMode(const std::string& key, BlendMode& out) {
    static const std::unordered_map<std::string, BlendMode> kMap = {
        {"Nrml", BlendMode::Normal},     {"Mltp", BlendMode::Multiply},
        {"Scrn", BlendMode::Screen},     {"Ovrl", BlendMode::Overlay},
        {"Drkn", BlendMode::Darken},     {"Lghn", BlendMode::Lighten},
        {"CDdg", BlendMode::ColorDodge}, {"CBrn", BlendMode::ColorBurn},
        {"HrdL", BlendMode::HardLight},  {"SftL", BlendMode::SoftLight},
        {"Dfrn", BlendMode::Difference}, {"Xclu", BlendMode::Exclusion},
        {"H   ", BlendMode::Hue},        {"Strt", BlendMode::Saturation},
        {"Clr ", BlendMode::Color},      {"Lmns", BlendMode::Luminosity},
        {"Add ", BlendMode::Add},        {"lddg", BlendMode::Add},
        {"Sbtr", BlendMode::Subtract},
    };
    const auto it = kMap.find(key);
    if (it == kMap.end())
        return false;
    out = it->second;
    return true;
}

/// 동적 반응의 제어원(bVTy enum) → Mari DynamicInput.
/// second 가 true 면 "비슷한 것으로 근사했다"는 뜻이다.
struct MappedInput {
    bool found = false;
    bool off = false;
    bool approximate = false;
    DynamicInput input = DynamicInput::Pressure;
};

MappedInput mapControlSource(const std::string& key) {
    struct Row {
        const char* key;
        DynamicInput input;
        bool approximate;
    };
    static const Row kRows[] = {
        {"Prs ", DynamicInput::Pressure, false},  {"PrsP", DynamicInput::Pressure, false},
        {"Pres", DynamicInput::Pressure, false},  {"fade", DynamicInput::Fade, false},
        {"Fade", DynamicInput::Fade, false},      {"Tlt ", DynamicInput::TiltX, true},
        {"tilt", DynamicInput::TiltX, true},      {"PnTl", DynamicInput::TiltX, true},
        {"WhlS", DynamicInput::Azimuth, true},    {"Rtn ", DynamicInput::Azimuth, true},
        {"InRt", DynamicInput::Azimuth, true},    {"Drct", DynamicInput::Direction, false},
        {"InDr", DynamicInput::Direction, true},  {"Rndm", DynamicInput::Random, false},
    };
    MappedInput m;
    if (key == "Off " || key == "off " || key == "None" || key.empty()) {
        m.found = true;
        m.off = true;
        return m;
    }
    for (const Row& row : kRows) {
        if (key == row.key) {
            m.found = true;
            m.input = row.input;
            m.approximate = row.approximate;
            return m;
        }
    }
    return m;
}

/// 출력이 가산식(회전·흩뿌림)인가, 곱셈식(크기·불투명도·유량·원형도)인가.
bool isAdditiveOutput(DynamicOutput o) {
    return o == DynamicOutput::Rotation || o == DynamicOutput::Scatter;
}

/// 변화(variance) 하위 디스크립터 하나를 DynamicLink 로 번역한다.
/// PS 의 variance 는 {제어원, 최솟값, 지터} 세 조각이라 링크가 최대 두 개 나온다.
void translateVariance(const Descriptor& varDesc, const std::string& prefix, DynamicOutput output,
                       MariBrushPreset& preset, ImportReport& report,
                       std::set<std::string>& used) {
    TrackedDescriptor t(varDesc, prefix, used);

    f32 minimum = 0.0f;
    bool hasMinimum = false;
    if (const DescValue* v = t.getAny({"minimum", "minimumV", "Mnm "})) {
        bool approx = false;
        minimum = percentToRatio(*v, approx);
        hasMinimum = true;
    }

    f32 jitter = 0.0f;
    if (const DescValue* v = t.getAny({"jitter", "Jttr"})) {
        bool approx = false;
        jitter = percentToRatio(*v, approx);
    }

    std::string control;
    if (const DescValue* v = t.getAny({"bVTy", "useTipDynamics", "control"})) {
        if (v->type == DescType::Enumerated)
            control = v->enumValue;
        else if (v->type == DescType::Integer)
            control = std::to_string(static_cast<int>(v->asNumber()));
    }

    if (!control.empty()) {
        const MappedInput m = mapControlSource(control);
        if (!m.found) {
            report.add(ImportSeverity::Dropped, t.path("bVTy"),
                       std::string("모르는 동적 제어원 '") + control + "' — " +
                           mari::brush::dynamicOutputName(output) +
                           " 의 필압/틸트 반응을 반영하지 못했다.");
        } else if (!m.off) {
            DynamicLink link;
            link.input = m.input;
            link.output = output;
            if (isAdditiveOutput(output)) {
                // 가산 출력: 0 입력에서 0, 1 입력에서 최대치.
                link.curve.points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
            } else {
                link.curve.points = {{0.0f, hasMinimum ? minimum : 0.0f}, {1.0f, 1.0f}};
            }
            preset.dynamics.push_back(std::move(link));
            if (m.approximate)
                report.add(ImportSeverity::Degraded, t.path("bVTy"),
                           std::string("동적 제어원 '") + control + "' 은 Mari 의 '" +
                               mari::brush::dynamicInputName(m.input) + "' 로 근사했다.");
            if (m.input == DynamicInput::Fade) {
                if (const DescValue* fs = t.getAny({"fadeStep", "Fdtp"})) {
                    preset.extraParams.emplace_back(prefix + "/fadeStep",
                                                    static_cast<f32>(fs->asNumber()));
                    report.add(ImportSeverity::Degraded, t.path("fadeStep"),
                               "페이드 길이(" + std::to_string(static_cast<int>(fs->asNumber())) +
                                   " 스텝)는 Mari 의 0..1 진행도로 정규화된다. "
                                   "실제 길이는 엔진 설정에 따라 달라진다.");
                }
            }
        }
    }

    if (jitter > 0.0f) {
        DynamicLink link;
        link.input = DynamicInput::Random;
        link.output = output;
        if (isAdditiveOutput(output))
            link.curve.points = {{0.0f, 0.0f}, {1.0f, jitter}};
        else
            link.curve.points = {{0.0f, std::max(0.0f, 1.0f - jitter)}, {1.0f, 1.0f}};
        preset.dynamics.push_back(std::move(link));
    }
}

/// 브러시 팁 디스크립터를 BrushTip 으로 번역한다. sampledData 는 out 으로 넘긴다.
void translateTip(TrackedDescriptor& t, MariBrushPreset& preset, ImportReport& report,
                  std::string& sampledData) {
    if (const DescValue* v = t.getAny({"Dmtr", "Dmtr "})) {
        const f32 d = static_cast<f32>(v->asNumber());
        if (d > 0.0f)
            preset.tip.diameter = d;
        if (v->type == DescType::UnitFloat && v->unit != "#Pxl")
            report.add(ImportSeverity::Degraded, t.path("Dmtr"),
                       "팁 지름의 단위가 픽셀이 아니다(" + v->unit + "). 값을 그대로 썼다.");
    }
    if (const DescValue* v = t.get("Angl"))
        preset.tip.angle = static_cast<f32>(v->asNumber());
    if (const DescValue* v = t.get("Rndn")) {
        bool approx = false;
        preset.tip.aspectRatio = std::clamp(percentToRatio(*v, approx), 0.01f, 1.0f);
    }
    if (const DescValue* v = t.get("Hrdn")) {
        bool approx = false;
        preset.tip.hardness = std::clamp(percentToRatio(*v, approx), 0.0f, 1.0f);
    }
    if (const DescValue* v = t.get("Spcn")) {
        bool approx = false;
        preset.spacing = std::max(0.01f, percentToRatio(*v, approx));
    }
    if (const DescValue* v = t.get("Intr")) {
        if (!v->asBool(true)) {
            preset.spacing = 0.01f;
            report.add(ImportSeverity::Degraded, t.path("Intr"),
                       "간격이 꺼져 있는 브러시다. Mari 는 간격 0 을 지원하지 않아 "
                       "지름의 1% 로 대신했다.");
        }
    }
    if (const DescValue* v = t.get("sampledData"))
        sampledData = v->text;

    // flipX/flipY 는 우리가 직접 뒤집을 수 있다. 버리지 않는다.
    bool flipX = false;
    bool flipY = false;
    if (const DescValue* v = t.getAny({"flipX", "FlpX"}))
        flipX = v->asBool();
    if (const DescValue* v = t.getAny({"flipY", "FlpY"}))
        flipY = v->asBool();
    if (flipX || flipY)
        preset.extraParams.emplace_back("abr/flip", (flipX ? 1.0f : 0.0f) + (flipY ? 2.0f : 0.0f));

    if (const DescValue* v = t.get("brushType")) {
        const int kind = static_cast<int>(v->asNumber());
        if (kind == 1)
            preset.tip.shape = mari::brush::ProceduralShape::Circle;
        else if (kind != 0)
            report.add(ImportSeverity::Degraded, t.path("brushType"),
                       "팁 종류 " + std::to_string(kind) + " 은 원형으로 근사했다.");
    }
}

/// 팁 비트맵을 좌우/상하로 뒤집는다(abr 의 flipX/flipY).
void applyFlip(GrayImage& img, bool flipX, bool flipY) {
    if (img.empty() || (!flipX && !flipY))
        return;
    const usize w = static_cast<usize>(img.width);
    const usize h = static_cast<usize>(img.height);
    std::vector<u8> out(img.pixels.size());
    for (usize y = 0; y < h; ++y) {
        const usize sy = flipY ? h - 1 - y : y;
        for (usize x = 0; x < w; ++x) {
            const usize sx = flipX ? w - 1 - x : x;
            out[y * w + x] = img.pixels[sy * w + sx];
        }
    }
    img.pixels.swap(out);
}

/// 사람이 읽을 키 설명. 없으면 키를 그대로 쓴다.
const char* describeKey(const std::string& key) {
    static const std::unordered_map<std::string, const char*> kDesc = {
        {"Nose", "노이즈"},
        {"Wtdg", "젖은 가장자리(wet edges)"},
        {"useBrushSize", "브러시 크기 고정 옵션"},
        {"protectTexture", "텍스처 보호"},
        {"dualBrush", "듀얼 브러시"},
        {"clrVr", "색상 변화(색조/채도/명도 지터)"},
        {"brushGroup", "브러시 그룹"},
        {"toolOptions", "도구 옵션"},
        {"Rpt ", "airbrush 반복"},
        {"useTipDynamics", "팁 다이내믹 사용 여부"},
        {"interfaceIconFrameDimmed", "UI 아이콘(그릴 것 없음)"},
    };
    const auto it = kDesc.find(key);
    return it == kDesc.end() ? nullptr : it->second;
}

/// 번역하지 못하고 남은 키를 전부 리포트에 적는다. **이게 이 임포터의 핵심 약속이다.**
void reportUnmapped(const Descriptor& brushDesc, const std::string& prefix,
                    const std::set<std::string>& used, const std::string& brushName,
                    ImportReport& report) {
    std::vector<std::string> all;
    collectKeyPaths(brushDesc, prefix, all);

    usize emitted = 0;
    usize skipped = 0;
    for (const std::string& path : all) {
        if (used.count(path) != 0)
            continue;
        // 이미 보고한 컨테이너의 하위 키는 한 번만 말한다.
        bool underReported = false;
        for (const std::string& p : all) {
            if (p.size() < path.size() && path.compare(0, p.size(), p) == 0 &&
                path[p.size()] == '/' && used.count(p) == 0) {
                underReported = true;
                break;
            }
        }
        if (underReported)
            continue;

        if (emitted >= kMaxUnmappedNotes) {
            ++skipped;
            continue;
        }
        const std::string leaf = path.substr(path.rfind('/') == std::string::npos
                                                 ? 0
                                                 : path.rfind('/') + 1);
        const char* human = describeKey(leaf);
        std::string msg = "'" + brushName + "' 의 설정 " + path;
        if (human != nullptr)
            msg += " (" + std::string(human) + ")";
        msg += " 은 Mari 로 번역하지 못해 반영되지 않았다.";
        report.add(ImportSeverity::Dropped, path, msg);
        ++emitted;
    }
    if (skipped > 0)
        report.add(ImportSeverity::Dropped, prefix,
                   "'" + brushName + "' 에서 위 외에 " + std::to_string(skipped) +
                       " 개의 설정을 더 번역하지 못했다.");
}

/// 브러시 디스크립터 하나 → MariBrushPreset 하나.
MariBrushPreset translateBrush(const Descriptor& brushDesc, const std::string& prefix,
                               const std::vector<SampBrush>& samples,
                               const std::vector<PattEntry>& patterns, usize index,
                               ImportReport& report) {
    std::set<std::string> used;
    TrackedDescriptor t(brushDesc, prefix, used);

    MariBrushPreset preset;
    preset.sourceFormat = "abr";
    preset.sourceId = std::to_string(index);

    if (const DescValue* v = t.getAny({"Nm  ", "Name"}))
        preset.name = v->text;
    if (preset.name.empty())
        preset.name = "abr 브러시 " + std::to_string(index + 1);

    std::string sampledData;
    if (const Descriptor* tip = t.child("Brsh")) {
        TrackedDescriptor tt(*tip, t.path("Brsh"), used);
        translateTip(tt, preset, report, sampledData);
    } else {
        report.add(ImportSeverity::Degraded, t.path("Brsh"),
                   "'" + preset.name + "' 에 팁 디스크립터가 없어 기본 원형 팁을 썼다.");
    }

    if (const DescValue* v = t.getAny({"Opct", "Opacity"})) {
        bool approx = false;
        preset.opacity = std::clamp(percentToRatio(*v, approx), 0.0f, 1.0f);
    }
    if (const DescValue* v = t.getAny({"Flw ", "flow"})) {
        bool approx = false;
        preset.flow = std::clamp(percentToRatio(*v, approx), 0.0f, 1.0f);
    }
    if (const DescValue* v = t.getAny({"Bld ", "blendMode"})) {
        if (v->type == DescType::Enumerated) {
            BlendMode mode = BlendMode::Normal;
            if (mapBlendMode(v->enumValue, mode))
                preset.blendMode = mode;
            else
                report.add(ImportSeverity::Dropped, t.path("Bld "),
                           "모르는 합성 모드 '" + v->enumValue + "' — 보통(normal)으로 뒀다.");
        }
    }

    // ── 동적 반응 ──
    struct VarRow {
        const char* key;
        DynamicOutput output;
    };
    static const VarRow kVariances[] = {
        {"szVr", DynamicOutput::Size},      {"opVr", DynamicOutput::Opacity},
        {"flVr", DynamicOutput::Flow},      {"rndVr", DynamicOutput::Roundness},
        {"wtVr", DynamicOutput::Roundness}, {"angVr", DynamicOutput::Rotation},
    };
    for (const VarRow& row : kVariances) {
        if (const Descriptor* var = t.child(row.key))
            translateVariance(*var, t.path(row.key), row.output, preset, report, used);
    }

    // 흩뿌림: {Scat 세기, Cnt 개수} 가 따로 논다.
    if (const DescValue* v = t.getAny({"Scat", "Spct", "scatter"})) {
        bool approx = false;
        const f32 amount = percentToRatio(*v, approx);
        if (amount > 0.0f) {
            DynamicLink link;
            link.input = DynamicInput::Random;
            link.output = DynamicOutput::Scatter;
            link.curve.points = {{0.0f, 0.0f}, {1.0f, amount}};
            preset.dynamics.push_back(std::move(link));
        }
    }
    if (const Descriptor* var = t.child("scatterDynamics"))
        translateVariance(*var, t.path("scatterDynamics"), DynamicOutput::Scatter, preset, report,
                          used);
    if (const DescValue* v = t.getAny({"Cnt ", "Count"})) {
        preset.extraParams.emplace_back("abr/scatterCount", static_cast<f32>(v->asNumber()));
        report.add(ImportSeverity::Degraded, t.path("Cnt "),
                   "'" + preset.name + "' 의 스탬프 개수(" +
                       std::to_string(static_cast<int>(v->asNumber())) +
                       ")는 엔진이 지원하면 쓰이고, 아니면 1 로 동작한다.");
    }

    // ── 텍스처 ──
    if (const Descriptor* tex = t.child("Txtr")) {
        TrackedDescriptor tt(*tex, t.path("Txtr"), used);
        BrushTexture texture;
        if (const DescValue* v = tt.getAny({"Scl ", "Scale"})) {
            bool approx = false;
            texture.scale = std::max(0.01f, percentToRatio(*v, approx));
        }
        if (const DescValue* v = tt.getAny({"textureDepth", "Dpth"})) {
            bool approx = false;
            texture.depth = std::clamp(percentToRatio(*v, approx), 0.0f, 1.0f);
        }
        if (const DescValue* v = tt.getAny({"textureBlendMode", "BlnM"})) {
            if (v->type == DescType::Enumerated) {
                BlendMode mode = BlendMode::Multiply;
                if (mapBlendMode(v->enumValue, mode))
                    texture.blendMode = mode;
                else
                    report.add(ImportSeverity::Dropped, tt.path("textureBlendMode"),
                               "모르는 텍스처 합성 모드 '" + v->enumValue + "' — 곱하기로 뒀다.");
            }
        }
        std::string patternId;
        if (const Descriptor* pat = tt.child("Ptrn")) {
            TrackedDescriptor pt(*pat, tt.path("Ptrn"), used);
            if (const DescValue* v = pt.getAny({"Idnt", "ID"}))
                patternId = v->text;
            (void)pt.getAny({"Nm  ", "Name"});
        }
        if (patternId.empty()) {
            if (const DescValue* v = tt.getAny({"Idnt", "ID"}))
                patternId = v->text;
        }

        const PattEntry* match = nullptr;
        for (const PattEntry& p : patterns) {
            if (!patternId.empty() && p.id == patternId) {
                match = &p;
                break;
            }
        }
        if (match == nullptr && !patterns.empty()) {
            match = &patterns.front();
            report.add(ImportSeverity::Degraded, tt.path("Idnt"),
                       "'" + preset.name +
                           "' 의 텍스처 패턴 ID 를 찾지 못해 파일의 첫 패턴을 썼다.");
        }
        if (match != nullptr) {
            texture.image = match->image;
            preset.texture = std::move(texture);
        } else {
            report.add(ImportSeverity::Dropped, tt.path("Ptrn"),
                       "'" + preset.name +
                           "' 의 텍스처 패턴 픽셀이 파일에 없어 텍스처를 가져오지 못했다. "
                           "설정(배율·깊이·합성 모드)만 읽었다.");
        }
    }

    // ── 팁 비트맵 연결 ──
    const SampBrush* sample = nullptr;
    if (!sampledData.empty()) {
        for (const SampBrush& s : samples) {
            if (s.name == sampledData) {
                sample = &s;
                break;
            }
        }
        if (sample == nullptr && !samples.empty()) {
            sample = &samples[std::min(index, samples.size() - 1)];
            report.add(ImportSeverity::Degraded, prefix + "/sampledData",
                       "'" + preset.name + "' 의 팁 ID(" + sampledData +
                           ")를 파일에서 찾지 못해 순서로 맞췄다.");
        }
    }
    if (sample != nullptr) {
        preset.tip.kind = TipKind::Bitmap;
        preset.tip.bitmap = sample->mask;
        bool flipX = false;
        bool flipY = false;
        for (const auto& [k, v] : preset.extraParams) {
            if (k == "abr/flip") {
                flipX = (static_cast<int>(v) & 1) != 0;
                flipY = (static_cast<int>(v) & 2) != 0;
            }
        }
        applyFlip(preset.tip.bitmap, flipX, flipY);
        if (preset.tip.diameter <= 0.0f)
            preset.tip.diameter = static_cast<f32>(std::max(sample->mask.width, sample->mask.height));
    }

    reportUnmapped(brushDesc, prefix, used, preset.name, report);
    return preset;
}

/// desc 가 없을 때(팁만 있는 .abr) samp 만으로 프리셋을 만든다.
MariBrushPreset presetFromSample(const SampBrush& s, usize index) {
    MariBrushPreset p;
    p.sourceFormat = "abr";
    p.sourceId = s.name.empty() ? std::to_string(index) : s.name;
    p.name = s.name.empty() ? ("abr 팁 " + std::to_string(index + 1)) : s.name;
    p.tip.kind = TipKind::Bitmap;
    p.tip.bitmap = s.mask;
    p.tip.diameter = static_cast<f32>(std::max(s.mask.width, s.mask.height));
    p.spacing = std::max(0.01f, static_cast<f32>(s.spacing) / 100.0f);
    return p;
}

} // namespace

BrushFormat detectFormat(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return BrushFormat::Unknown;
    u8 head[16] = {};
    const usize n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);

    if (n >= 16 && std::string(reinterpret_cast<const char*>(head), 15) == "SQLite format 3")
        return BrushFormat::Sut;
    if (n >= 4) {
        // .abr 은 매직이 없다. 버전 워드(1,2,6,7,10) + 8BIM 여부로 본다.
        const u16 version = static_cast<u16>((head[0] << 8) | head[1]);
        if (version == 1 || version == 2 || version == 6 || version == 7 || version == 10)
            return BrushFormat::Abr;
    }
    // 매직으로 못 가리면 확장자로 마지막 판단을 한다.
    const auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot + 1);
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == "abr")
            return BrushFormat::Abr;
        if (ext == "sut")
            return BrushFormat::Sut;
    }
    return BrushFormat::Unknown;
}

Result<ImportResult> importAbrBytes(const u8* data, usize size, const std::string& sourceName) {
    if (data == nullptr || size < 4)
        return Err("abr 파일이 너무 짧다", ErrorCode::ParseError);

    ImportResult result;
    result.report.sourcePath = sourceName;

    ByteReader r(data, size);
    const u16 version = r.u16be();
    const u16 subversion = r.u16be();
    if (version == 1 || version == 2)
        return Err("abr 버전 " + std::to_string(version) +
                       " 은 팁 비트맵만 담고 동작 정보가 없다. 아직 지원하지 않는다.",
                   ErrorCode::Unsupported);
    if (version != 6 && version != 7 && version != 10)
        return Err("모르는 abr 버전: " + std::to_string(version), ErrorCode::Unsupported);
    if (subversion != 1 && subversion != 2)
        result.report.add(ImportSeverity::Info, "header",
                          "처음 보는 abr 하위 버전 " + std::to_string(subversion) +
                              " 이다. 아는 범위에서 읽었다.");

    std::vector<SampBrush> samples;
    std::vector<PattEntry> patterns;
    DescriptorPtr rootDesc;

    while (r.remaining() >= 12) {
        const usize sigPos = r.pos();
        std::string sig = r.ascii(4);
        if (sig != "8BIM") {
            // 섹션 패딩이 4바이트 경계일 수 있다. 한 번만 맞춰 보고, 그래도 아니면 멈춘다.
            r.seek(sigPos);
            r.align(4);
            if (r.failed() || r.remaining() < 12)
                break;
            sig = r.ascii(4);
            if (sig != "8BIM") {
                result.report.add(ImportSeverity::Info, "section",
                                  "오프셋 " + std::to_string(sigPos) +
                                      " 에서 8BIM 서명을 찾지 못해 읽기를 멈췄다.");
                break;
            }
        }
        const std::string key = r.ascii(4);
        const u32 len = r.u32be();
        if (r.failed())
            break;
        const usize start = r.pos();
        if (!r.has(len)) {
            result.report.add(ImportSeverity::Dropped, "section/" + key,
                              "'" + key + "' 섹션의 길이가 파일 밖을 가리킨다. 여기서 멈춘다.");
            break;
        }

        ByteReader sub(data + start, len);
        if (key == "samp") {
            samples = parseSampSection(sub, result.report);
        } else if (key == "desc") {
            auto d = parseDescriptorSection(sub);
            if (d)
                rootDesc = std::move(d).value();
            else
                result.report.add(ImportSeverity::Dropped, "section/desc",
                                  "동작 파라미터(desc)를 읽지 못했다: " + d.error().message +
                                      " — 팁 비트맵만 가져온다.");
        } else if (key == "patt") {
            patterns = parsePattSection(sub, result.report);
        } else if (key == "phry") {
            result.report.add(ImportSeverity::Info, "section/phry",
                              "브러시 계층(phry) 섹션은 Mari 에 대응이 없어 읽지 않았다.");
        } else {
            result.report.add(ImportSeverity::Info, "section/" + key,
                              "모르는 섹션 '" + key + "' 을 건너뛰었다.");
        }

        r.seek(start + len);
        if (r.failed())
            break;
    }

    // desc 가 있으면 desc 를 정본으로 삼는다(동작이 거기 있다).
    if (rootDesc) {
        const DescValue* list = rootDesc->find("Brsh");
        if (list != nullptr && list->type == DescType::List) {
            for (usize i = 0; i < list->list.size(); ++i) {
                const DescValue& item = list->list[i];
                if (item.type != DescType::Descriptor || !item.descriptor) {
                    result.report.add(ImportSeverity::Dropped, "Brsh[" + std::to_string(i) + "]",
                                      "브러시 항목이 디스크립터가 아니라 건너뛰었다.");
                    continue;
                }
                result.presets.push_back(translateBrush(*item.descriptor,
                                                        "Brsh[" + std::to_string(i) + "]", samples,
                                                        patterns, i, result.report));
            }
        } else {
            // 루트 자체가 브러시 하나인 경우도 있다.
            result.presets.push_back(
                translateBrush(*rootDesc, "", samples, patterns, 0, result.report));
        }
    }

    if (result.presets.empty()) {
        if (samples.empty())
            return Err("abr 에서 브러시를 하나도 읽지 못했다", ErrorCode::ParseError);
        result.report.add(ImportSeverity::Degraded, "desc",
                          "동작 파라미터(desc 섹션)가 없어 팁 비트맵과 간격만 가져왔다. "
                          "필압 반응·흩뿌림·텍스처는 기본값이다.");
        for (usize i = 0; i < samples.size(); ++i)
            result.presets.push_back(presetFromSample(samples[i], i));
    }

    result.report.presetCount = result.presets.size();
    return Ok(std::move(result));
}

Result<ImportResult> importAbrFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return Err("파일을 열지 못했다: " + path, ErrorCode::IoError);
    std::vector<u8> buf;
    u8 chunk[64 * 1024];
    while (true) {
        const usize n = std::fread(chunk, 1, sizeof(chunk), f);
        if (n == 0)
            break;
        buf.insert(buf.end(), chunk, chunk + n);
        if (buf.size() > (256u << 20)) {
            std::fclose(f);
            return Err("abr 파일이 비정상적으로 크다: " + path, ErrorCode::ParseError);
        }
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad)
        return Err("파일을 읽는 중 오류가 났다: " + path, ErrorCode::IoError);
    return importAbrBytes(buf.data(), buf.size(), path);
}

Result<ImportResult> importBrushFile(const std::string& path) {
    switch (detectFormat(path)) {
    case BrushFormat::Abr:
        return importAbrFile(path);
    case BrushFormat::Sut:
        return importSutFile(path);
    case BrushFormat::Unknown:
        break;
    }
    return Err("브러시 포맷을 알아보지 못했다: " + path, ErrorCode::Unsupported);
}

} // namespace mari::io::brush
