// Mari Paint — 브러시 임포터 테스트 픽스처 생성기.
//
// 실제 .abr/.sut 파일은 저장소에 넣지 않는다(라이선스·용량). 대신 **바이트를 직접 조립**해
// 최소 유효 파일을 만든다. 포맷을 우리 손으로 쓰고 우리 손으로 읽으니 왕복이 검증된다.
//
// 여기서 쓰는 레이아웃은 임포터가 가정하는 레이아웃과 같다. 즉 이 파일은
// "우리가 이해했다고 주장하는 포맷"의 **실행 가능한 명세**이기도 하다.
#ifndef MARI_TESTS_IO_BRUSH_FIXTURE_BUILDER_HPP
#define MARI_TESTS_IO_BRUSH_FIXTURE_BUILDER_HPP

#include <mari/core/types.hpp>

#include <png.h>
#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <string>
#include <vector>

// 임시 파일 이름에 PID 를 섞는다. Windows 는 unistd.h 가 없다.
#if defined(_WIN32)
#include <process.h>
#define MARI_TESTFIX_GETPID() ::_getpid()
#else
#include <unistd.h>
#define MARI_TESTFIX_GETPID() ::getpid()
#endif

namespace mari::testfix {

using mari::f64;
using mari::i32;
using mari::u16;
using mari::u32;
using mari::u8;
using mari::usize;

// ── 빅엔디안 바이트 라이터 ───────────────────────────────────────────────

struct ByteWriter {
    std::vector<u8> bytes;

    void raw(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void raw(const std::vector<u8>& v) { bytes.insert(bytes.end(), v.begin(), v.end()); }
    void u8v(u8 v) { bytes.push_back(v); }
    void u16be(u16 v) {
        bytes.push_back(static_cast<u8>(v >> 8));
        bytes.push_back(static_cast<u8>(v & 0xFF));
    }
    void i16be(mari::i16 v) { u16be(static_cast<u16>(v)); }
    void u32be(u32 v) {
        bytes.push_back(static_cast<u8>(v >> 24));
        bytes.push_back(static_cast<u8>((v >> 16) & 0xFF));
        bytes.push_back(static_cast<u8>((v >> 8) & 0xFF));
        bytes.push_back(static_cast<u8>(v & 0xFF));
    }
    void i32be(i32 v) { u32be(static_cast<u32>(v)); }
    void f64be(f64 v) {
        mari::u64 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u32be(static_cast<u32>(bits >> 32));
        u32be(static_cast<u32>(bits & 0xFFFFFFFFu));
    }
    void ascii(const std::string& s) { raw(s.data(), s.size()); }
    /// Adobe 유니코드 문자열: u32 문자 수(NUL 포함) + UTF-16BE.
    /// 입력은 UTF-8(소스 파일 인코딩) 이라 UTF-16 으로 제대로 접어 준다 —
    /// 여기서 대충 하면 임포터의 한글 이름 처리를 검증하지 못한다.
    void unicodeString(const std::string& s) {
        std::vector<u16> units;
        usize i = 0;
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            u32 cp = 0;
            usize extra = 0;
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1Fu;
                extra = 1;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0Fu;
                extra = 2;
            } else {
                cp = c & 0x07u;
                extra = 3;
            }
            ++i;
            for (usize k = 0; k < extra && i < s.size(); ++k, ++i)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
            if (cp >= 0x10000u) {
                cp -= 0x10000u;
                units.push_back(static_cast<u16>(0xD800u + (cp >> 10)));
                units.push_back(static_cast<u16>(0xDC00u + (cp & 0x3FFu)));
            } else {
                units.push_back(static_cast<u16>(cp));
            }
        }
        u32be(static_cast<u32>(units.size() + 1));
        for (u16 unit : units)
            u16be(unit);
        u16be(0);
    }
    /// Adobe 키: 길이 4 면 u32 0 + 4글자, 아니면 u32 길이 + 글자.
    void key(const std::string& s) {
        if (s.size() == 4)
            u32be(0);
        else
            u32be(static_cast<u32>(s.size()));
        ascii(s);
    }
    void align4() {
        while (bytes.size() % 4 != 0)
            bytes.push_back(0);
    }
};

// ── ActionDescriptor 값 ─────────────────────────────────────────────────

using DescBytes = std::vector<u8>;
using DescItem = std::pair<std::string, DescBytes>;

inline DescBytes dvUnitFloat(const std::string& unit, f64 v) {
    ByteWriter w;
    w.ascii("UntF");
    w.ascii(unit);
    w.f64be(v);
    return w.bytes;
}
inline DescBytes dvDouble(f64 v) {
    ByteWriter w;
    w.ascii("doub");
    w.f64be(v);
    return w.bytes;
}
inline DescBytes dvLong(i32 v) {
    ByteWriter w;
    w.ascii("long");
    w.i32be(v);
    return w.bytes;
}
inline DescBytes dvBool(bool v) {
    ByteWriter w;
    w.ascii("bool");
    w.u8v(v ? 1 : 0);
    return w.bytes;
}
inline DescBytes dvText(const std::string& v) {
    ByteWriter w;
    w.ascii("TEXT");
    w.unicodeString(v);
    return w.bytes;
}
inline DescBytes dvEnum(const std::string& type, const std::string& value) {
    ByteWriter w;
    w.ascii("enum");
    w.key(type);
    w.key(value);
    return w.bytes;
}
inline DescBytes dvRaw(const std::vector<u8>& v) {
    ByteWriter w;
    w.ascii("tdta");
    w.u32be(static_cast<u32>(v.size()));
    w.raw(v);
    return w.bytes;
}

/// 디스크립터 본문(className | classId | count | items) 을 쓴다. OSType 은 붙이지 않는다.
inline DescBytes descriptorBody(const std::string& className, const std::string& classId,
                                const std::vector<DescItem>& items) {
    ByteWriter w;
    w.unicodeString(className);
    w.key(classId);
    w.u32be(static_cast<u32>(items.size()));
    for (const auto& [k, v] : items) {
        w.key(k);
        w.raw(v);
    }
    return w.bytes;
}

inline DescBytes dvObjc(const std::string& className, const std::string& classId,
                        const std::vector<DescItem>& items) {
    ByteWriter w;
    w.ascii("Objc");
    w.raw(descriptorBody(className, classId, items));
    return w.bytes;
}

inline DescBytes dvList(const std::vector<DescBytes>& values) {
    ByteWriter w;
    w.ascii("VlLs");
    w.u32be(static_cast<u32>(values.size()));
    for (const auto& v : values)
        w.raw(v);
    return w.bytes;
}

/// `desc` 섹션 본문: u32 버전(16) + 루트 디스크립터.
inline std::vector<u8> descSection(const std::vector<DescItem>& rootItems) {
    ByteWriter w;
    w.u32be(16);
    w.raw(descriptorBody("", "null", rootItems));
    return w.bytes;
}

// ── samp 섹션 ───────────────────────────────────────────────────────────

/// 팁 하나의 원본 픽셀. 실물 abr 규약대로 **255 = 잉크 가득, 0 = 빈 곳**이다.
struct SampSpec {
    std::string name;
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> pixels; ///< width*height
    u16 spacing = 25;
    bool rle = false;
};

/// PackBits 로 한 행을 압축한다(가장 단순한 형태: 그대로 복사 런만 쓴다).
inline std::vector<u8> packBitsRow(const u8* row, usize width) {
    std::vector<u8> out;
    usize x = 0;
    while (x < width) {
        usize runEnd = x + 1;
        while (runEnd < width && row[runEnd] == row[x] && runEnd - x < 128)
            ++runEnd;
        const usize runLen = runEnd - x;
        if (runLen >= 2) {
            out.push_back(static_cast<u8>(static_cast<mari::i8>(-static_cast<int>(runLen) + 1)));
            out.push_back(row[x]);
            x = runEnd;
        } else {
            usize litEnd = x;
            while (litEnd < width && litEnd - x < 128) {
                if (litEnd + 1 < width && row[litEnd + 1] == row[litEnd])
                    break;
                ++litEnd;
            }
            const usize litLen = litEnd - x;
            out.push_back(static_cast<u8>(litLen - 1));
            for (usize i = 0; i < litLen; ++i)
                out.push_back(row[x + i]);
            x = litEnd;
        }
    }
    return out;
}

/// v6.2 머리말(실물 .abr 과 같다): 37바이트 키("$"+이름, NUL 채움) · 264바이트 건너뜀 · bounds · depth · compression.
/// spacing 은 v6 머리말에 없다 — desc 의 Spcn 이 맡는다(SampSpec::spacing 은 옛 v1/2 규약용으로 남겨 둔다).
inline std::vector<u8> sampSection(const std::vector<SampSpec>& tips) {
    ByteWriter out;
    for (const SampSpec& t : tips) {
        ByteWriter b;
        {
            std::string key = "$" + t.name;
            key.resize(37, static_cast<char>(0));
            b.raw(std::vector<u8>(key.begin(), key.end()));
        }
        b.raw(std::vector<u8>(264, 0));
        b.i32be(0);           // top
        b.i32be(0);           // left
        b.i32be(t.height);    // bottom
        b.i32be(t.width);     // right
        b.u16be(8);           // depth
        b.u8v(t.rle ? 1 : 0); // compression
        if (t.rle) {
            std::vector<std::vector<u8>> rows;
            rows.reserve(static_cast<usize>(t.height));
            for (i32 y = 0; y < t.height; ++y)
                rows.push_back(packBitsRow(t.pixels.data() + static_cast<usize>(y) * t.width,
                                           static_cast<usize>(t.width)));
            for (const auto& r : rows)
                b.u16be(static_cast<u16>(r.size()));
            for (const auto& r : rows)
                b.raw(r);
        } else {
            b.raw(t.pixels);
        }
        out.u32be(static_cast<u32>(b.bytes.size()));
        out.raw(b.bytes);
        out.align4();
    }
    return out.bytes;
}

// ── patt 섹션 ───────────────────────────────────────────────────────────

struct PattSpec {
    std::string id;
    std::string name;
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> pixels;
};

inline std::vector<u8> pattSection(const std::vector<PattSpec>& pats) {
    ByteWriter out;
    for (const PattSpec& p : pats) {
        ByteWriter b;
        b.u32be(1); // 패턴 버전
        b.u32be(1); // 이미지 모드 = 그레이스케일
        b.u16be(static_cast<u16>(p.height));
        b.u16be(static_cast<u16>(p.width));
        b.unicodeString(p.name);
        b.u8v(static_cast<u8>(p.id.size()));
        b.ascii(p.id);

        ByteWriter chan;
        chan.u32be(8); // pixel depth(32비트 필드)
        chan.i32be(0);
        chan.i32be(0);
        chan.i32be(p.height);
        chan.i32be(p.width);
        chan.u16be(8);
        chan.u8v(0); // 압축 없음
        chan.raw(p.pixels);

        b.u32be(3); // 가상 메모리 배열 리스트 버전
        b.u32be(static_cast<u32>(chan.bytes.size() + 24));
        b.i32be(0);
        b.i32be(0);
        b.i32be(p.height);
        b.i32be(p.width);
        b.u32be(1); // 채널 1개
        b.u32be(1); // written
        b.u32be(static_cast<u32>(chan.bytes.size()));
        b.raw(chan.bytes);

        out.u32be(static_cast<u32>(b.bytes.size()));
        out.raw(b.bytes);
        out.align4();
    }
    return out.bytes;
}

// ── .abr 전체 ───────────────────────────────────────────────────────────

struct AbrSection {
    std::string key;
    std::vector<u8> data;
};

inline std::vector<u8> buildAbr(u16 version, u16 subversion,
                                const std::vector<AbrSection>& sections) {
    ByteWriter w;
    w.u16be(version);
    w.u16be(subversion);
    for (const AbrSection& s : sections) {
        w.ascii("8BIM");
        w.ascii(s.key);
        w.u32be(static_cast<u32>(s.data.size()));
        w.raw(s.data);
        w.align4();
    }
    return w.bytes;
}

// ── PNG 쓰기(테스트 텍스처용) ────────────────────────────────────────────

inline void pngWriteToVector(png_structp png, png_bytep data, png_size_t length) {
    auto* out = static_cast<std::vector<u8>*>(png_get_io_ptr(png));
    out->insert(out->end(), data, data + length);
}
inline void pngFlushNoop(png_structp) {}

/// 그레이스케일 8비트 PNG 를 메모리에 쓴다.
inline std::vector<u8> writeGrayPng(i32 width, i32 height, const std::vector<u8>& pixels) {
    std::vector<u8> out;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr)
        return out;
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return out;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return out;
    }
    png_set_write_fn(png, &out, pngWriteToVector, pngFlushNoop);
    png_set_IHDR(png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 8,
                 PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_bytep> rows(static_cast<usize>(height));
    for (i32 y = 0; y < height; ++y)
        rows[static_cast<usize>(y)] =
            const_cast<png_bytep>(pixels.data() + static_cast<usize>(y) * width);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return out;
}

// ── tar 쓰기 ────────────────────────────────────────────────────────────

struct TarFile {
    std::string name;
    std::vector<u8> data;
};

/// 비압축 ustar 아카이브를 만든다(.sut 의 FileData 컬럼 모사).
inline std::vector<u8> buildTar(const std::vector<TarFile>& files) {
    std::vector<u8> out;
    for (const TarFile& f : files) {
        u8 header[512] = {};
        const usize nameLen = f.name.size() < 100 ? f.name.size() : 99;
        std::memcpy(header, f.name.data(), nameLen);
        std::snprintf(reinterpret_cast<char*>(header) + 100, 8, "%07o", 0644);
        std::snprintf(reinterpret_cast<char*>(header) + 108, 8, "%07o", 0);
        std::snprintf(reinterpret_cast<char*>(header) + 116, 8, "%07o", 0);
        std::snprintf(reinterpret_cast<char*>(header) + 124, 12, "%011o",
                      static_cast<unsigned>(f.data.size()));
        std::snprintf(reinterpret_cast<char*>(header) + 136, 12, "%011o", 0u);
        std::memset(header + 148, ' ', 8); // 체크섬 자리는 일단 공백
        header[156] = '0';
        std::memcpy(header + 257, "ustar", 5);
        std::memcpy(header + 263, "00", 2);

        unsigned sum = 0;
        for (usize i = 0; i < 512; ++i)
            sum += header[i];
        std::snprintf(reinterpret_cast<char*>(header) + 148, 8, "%06o", sum);
        header[154] = 0;
        header[155] = ' ';

        out.insert(out.end(), header, header + 512);
        out.insert(out.end(), f.data.begin(), f.data.end());
        const usize pad = (512 - f.data.size() % 512) % 512;
        out.insert(out.end(), pad, 0);
    }
    out.insert(out.end(), 1024, 0); // 끝 표식
    return out;
}

// ── effector blob (가설 A: u32 버전 | u32 개수 | (u32 x, u32 y) × 개수) ──

inline std::vector<u8> effectorBlobA(const std::vector<std::pair<double, double>>& points) {
    ByteWriter w;
    w.u32be(1);
    w.u32be(static_cast<u32>(points.size()));
    for (const auto& [x, y] : points) {
        w.u32be(static_cast<u32>(x * 10000.0));
        w.u32be(static_cast<u32>(y * 10000.0));
    }
    return w.bytes;
}

// ── .sut (SQLite) ───────────────────────────────────────────────────────

/// 임시 파일 경로 하나. 소멸자가 파일을 지운다.
class TempFile {
public:
    explicit TempFile(const std::string& tag) {
        static int counter = 0;
        path_ = "/tmp/mari_brush_fixture_" + tag + "_" + std::to_string(++counter) + "_" +
                std::to_string(static_cast<long>(MARI_TESTFIX_GETPID())) + ".bin";
        std::remove(path_.c_str());
    }
    ~TempFile() { std::remove(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

inline bool writeFile(const std::string& path, const std::vector<u8>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const usize n = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

/// SQL 여러 줄을 실행한다. 실패하면 false.
inline bool execSql(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (err != nullptr)
        sqlite3_free(err);
    return rc == SQLITE_OK;
}

/// blob 을 한 칸에 넣는다.
inline bool bindBlobRow(sqlite3* db, const std::string& sql, const std::vector<u8>& blob) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

} // namespace mari::testfix

#endif // MARI_TESTS_IO_BRUSH_FIXTURE_BUILDER_HPP
