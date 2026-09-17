// Mari Paint — RGBA8 이미지 · PNG 코덱 · 타일맵 변환. 선언은 include/mari/ora/image.hpp.
//
// libpng 의 오류 처리는 setjmp/longjmp 다. C++ 객체의 소멸자를 건너뛸 수 있으므로
// longjmp 가 가능한 구간에서는 **지역 객체를 만들지 않는다.** 버퍼는 구간 밖에 둔다.
#include <mari/ora/image.hpp>

#include <png.h>

#include <cstring>

namespace mari::ora {
namespace {

/// png_set_write_fn 콜백이 채우는 출력 버퍼.
struct WriteSink {
    std::vector<u8>* out = nullptr;
};

void pngWrite(png_structp png, png_bytep data, png_size_t length) {
    auto* sink = static_cast<WriteSink*>(png_get_io_ptr(png));
    sink->out->insert(sink->out->end(), data, data + length);
}

void pngFlush(png_structp) {}

/// png_set_read_fn 콜백이 읽는 입력 커서.
struct ReadSource {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;
};

void pngRead(png_structp png, png_bytep out, png_size_t length) {
    auto* src = static_cast<ReadSource*>(png_get_io_ptr(png));
    if (src->pos + length > src->size) {
        png_error(png, "PNG stream truncated");
        return;
    }
    std::memcpy(out, src->data + src->pos, length);
    src->pos += length;
}

/// 오류를 문자열로 받아 두고 longjmp 로 빠져나간다.
struct ErrorSink {
    std::string message;
};

void pngError(png_structp png, png_const_charp msg) {
    auto* sink = static_cast<ErrorSink*>(png_get_error_ptr(png));
    if (sink != nullptr)
        sink->message = msg != nullptr ? msg : "unknown";
    png_longjmp(png, 1);
}

void pngWarning(png_structp, png_const_charp) {
    // 경고는 무시한다. .ora 안에는 온갖 툴이 쓴 PNG 가 들어온다.
}

} // namespace

Image8 Image8::make(i32 w, i32 h) {
    Image8 img;
    if (w <= 0 || h <= 0)
        return img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<usize>(w) * static_cast<usize>(h) * 4u, u8{0});
    return img;
}

// ── 인코딩 ───────────────────────────────────────────────────────────────

Result<std::vector<u8>> encodePng(const Image8& img, int level) {
    if (img.empty())
        return Err("encodePng: 빈 이미지", ErrorCode::InvalidArgument);
    if (img.pixels.size() < img.stride() * static_cast<usize>(img.height))
        return Err("encodePng: 픽셀 버퍼가 작다", ErrorCode::InvalidArgument);
    // setjmp 구간을 넘나드는 값이라 volatile 로 둔다(-Wclobbered 회피).
    // 파라미터 level 은 이 아래로 쓰지 않는다.
    const volatile int compressionLevel = (level < 0 || level > 9) ? 6 : level;

    std::vector<u8> out;
    ErrorSink errSink;
    WriteSink sink{&out};
    std::vector<png_bytep> rows(static_cast<usize>(img.height));

    png_structp png =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, &errSink, pngError, pngWarning);
    if (png == nullptr)
        return Err("encodePng: png_create_write_struct 실패", ErrorCode::OutOfMemory);
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return Err("encodePng: png_create_info_struct 실패", ErrorCode::OutOfMemory);
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return Err("encodePng 실패: " + errSink.message, ErrorCode::IoError);
    }

    png_set_write_fn(png, &sink, pngWrite, pngFlush);
    png_set_compression_level(png, compressionLevel);
    png_set_IHDR(png, info, static_cast<png_uint_32>(img.width),
                 static_cast<png_uint_32>(img.height), 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    u8* base = const_cast<u8*>(img.pixels.data());
    for (usize y = 0; y < rows.size(); ++y)
        rows[y] = base + y * img.stride();
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return out;
}

// ── 디코딩 ───────────────────────────────────────────────────────────────

Result<Image8> decodePng(const u8* data, usize size) {
    if (data == nullptr || size < 8)
        return Err("decodePng: 입력이 너무 짧다", ErrorCode::ParseError);
    if (png_sig_cmp(static_cast<png_const_bytep>(data), 0, 8) != 0)
        return Err("decodePng: PNG 서명이 아니다", ErrorCode::ParseError);

    Image8 img;
    ErrorSink errSink;
    ReadSource src{data, size, 0};
    std::vector<png_bytep> rows;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &errSink, pngError, pngWarning);
    if (png == nullptr)
        return Err("decodePng: png_create_read_struct 실패", ErrorCode::OutOfMemory);
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return Err("decodePng: png_create_info_struct 실패", ErrorCode::OutOfMemory);
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return Err("decodePng 실패: " + errSink.message, ErrorCode::ParseError);
    }

    png_set_read_fn(png, &src, pngRead);
    png_read_info(png, info);

    const png_uint_32 w = png_get_image_width(png, info);
    const png_uint_32 h = png_get_image_height(png, info);
    const int depth = png_get_bit_depth(png, info);
    const int colorType = png_get_color_type(png, info);

    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu)
        png_error(png, "unreasonable image size");

    // 무엇이 들어오든 8비트 RGBA 로 펼친다.
    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0)
        png_set_tRNS_to_alpha(png);
    if (depth == 16)
        png_set_strip_16(png);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    if (png_get_rowbytes(png, info) != static_cast<png_size_t>(w) * 4u)
        png_error(png, "unexpected row size after transforms");

    img = Image8::make(static_cast<i32>(w), static_cast<i32>(h));
    if (img.pixels.empty())
        png_error(png, "out of memory");
    rows.resize(h);
    for (png_uint_32 y = 0; y < h; ++y)
        rows[y] = img.pixels.data() + static_cast<usize>(y) * img.stride();
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    return img;
}

// ── 타일맵 ↔ 이미지 ──────────────────────────────────────────────────────

Result<Image8> readRegion(const TileMap& map, const Rect& area) {
    if (map.format() != PixelFormat::RGBA8)
        return Err("readRegion: RGBA8 타일맵만 지원한다", ErrorCode::Unsupported);
    if (area.isEmpty())
        return Err("readRegion: 빈 영역", ErrorCode::InvalidArgument);

    Image8 img = Image8::make(area.width, area.height);
    if (img.pixels.empty())
        return Err("readRegion: 메모리 부족", ErrorCode::OutOfMemory);

    const i32 tx0 = tileIndexFor(area.x);
    const i32 ty0 = tileIndexFor(area.y);
    const i32 tx1 = tileIndexFor(area.right() - 1);
    const i32 ty1 = tileIndexFor(area.bottom() - 1);

    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const ConstTilePtr tile = map.at(TileCoord{tx, ty});
            if (!tile)
                continue; // 없는 타일 = 완전 투명. 이미 0으로 채워져 있다
            const Rect tr = TileCoord{tx, ty}.canvasRect();
            const Rect hit = tr.intersected(area);
            if (hit.isEmpty())
                continue;
            const u8* srcBase = tile->pixels();
            const usize srcStride = tile->stride();
            for (i32 y = hit.y; y < hit.bottom(); ++y) {
                const u8* s = srcBase + static_cast<usize>(y - tr.y) * srcStride +
                              static_cast<usize>(hit.x - tr.x) * 4u;
                u8* d = img.pixels.data() + static_cast<usize>(y - area.y) * img.stride() +
                        static_cast<usize>(hit.x - area.x) * 4u;
                std::memcpy(d, s, static_cast<usize>(hit.width) * 4u);
            }
        }
    }
    return img;
}

Result<void> writeRegion(TileMap& map, const Rect& area, const Image8& img) {
    if (map.format() != PixelFormat::RGBA8)
        return Err("writeRegion: RGBA8 타일맵만 지원한다", ErrorCode::Unsupported);
    if (area.isEmpty())
        return Ok();
    if (img.width != area.width || img.height != area.height)
        return Err("writeRegion: 이미지 크기가 영역과 다르다", ErrorCode::InvalidArgument);

    const i32 tx0 = tileIndexFor(area.x);
    const i32 ty0 = tileIndexFor(area.y);
    const i32 tx1 = tileIndexFor(area.right() - 1);
    const i32 ty1 = tileIndexFor(area.bottom() - 1);

    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const Rect tr = TileCoord{tx, ty}.canvasRect();
            const Rect hit = tr.intersected(area);
            if (hit.isEmpty())
                continue;

            // 이 타일에 들어갈 조각이 전부 투명하면 타일을 만들지 않는다(희소성 유지).
            bool anyInk = false;
            for (i32 y = hit.y; y < hit.bottom() && !anyInk; ++y) {
                const u8* s = img.pixels.data() + static_cast<usize>(y - area.y) * img.stride() +
                              static_cast<usize>(hit.x - area.x) * 4u;
                for (i32 x = 0; x < hit.width; ++x)
                    if (s[static_cast<usize>(x) * 4u + 3u] != 0) {
                        anyInk = true;
                        break;
                    }
            }
            if (!anyInk && !map.at(TileCoord{tx, ty}))
                continue;

            auto w = map.writable(TileCoord{tx, ty});
            if (!w)
                return w.error();
            const TilePtr tile = std::move(w).value();
            u8* dstBase = tile->mutablePixels();
            const usize dstStride = tile->stride();
            for (i32 y = hit.y; y < hit.bottom(); ++y) {
                const u8* s = img.pixels.data() + static_cast<usize>(y - area.y) * img.stride() +
                              static_cast<usize>(hit.x - area.x) * 4u;
                u8* d = dstBase + static_cast<usize>(y - tr.y) * dstStride +
                        static_cast<usize>(hit.x - tr.x) * 4u;
                std::memcpy(d, s, static_cast<usize>(hit.width) * 4u);
            }
        }
    }
    return Ok();
}

Image8 downscaleToFit(const Image8& src, i32 maxSide) {
    if (src.empty() || maxSide <= 0)
        return src;
    if (src.width <= maxSide && src.height <= maxSide)
        return src;

    const f64 scale = static_cast<f64>(maxSide) /
                      static_cast<f64>(src.width > src.height ? src.width : src.height);
    i32 dw = static_cast<i32>(static_cast<f64>(src.width) * scale);
    i32 dh = static_cast<i32>(static_cast<f64>(src.height) * scale);
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;

    Image8 dst = Image8::make(dw, dh);
    for (i32 y = 0; y < dh; ++y) {
        const i32 sy0 = static_cast<i32>(static_cast<i64>(y) * src.height / dh);
        i32 sy1 = static_cast<i32>(static_cast<i64>(y + 1) * src.height / dh);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (i32 x = 0; x < dw; ++x) {
            const i32 sx0 = static_cast<i32>(static_cast<i64>(x) * src.width / dw);
            i32 sx1 = static_cast<i32>(static_cast<i64>(x + 1) * src.width / dw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;

            // 색은 **알파 가중 평균**으로 모은다. 그냥 평균 내면 투명 픽셀의
            // 색(보통 0)이 섞여 가장자리가 검게 뜬다.
            u64 sumA = 0;
            u64 sumR = 0, sumG = 0, sumB = 0;
            u64 n = 0;
            for (i32 sy = sy0; sy < sy1; ++sy) {
                const u8* s = src.pixels.data() + static_cast<usize>(sy) * src.stride() +
                              static_cast<usize>(sx0) * 4u;
                for (i32 sx = sx0; sx < sx1; ++sx, s += 4) {
                    const u64 a = s[3];
                    sumR += static_cast<u64>(s[0]) * a;
                    sumG += static_cast<u64>(s[1]) * a;
                    sumB += static_cast<u64>(s[2]) * a;
                    sumA += a;
                    ++n;
                }
            }
            u8* d = dst.pixels.data() + static_cast<usize>(y) * dst.stride() +
                    static_cast<usize>(x) * 4u;
            if (sumA == 0 || n == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
            } else {
                d[0] = static_cast<u8>(sumR / sumA);
                d[1] = static_cast<u8>(sumG / sumA);
                d[2] = static_cast<u8>(sumB / sumA);
                d[3] = static_cast<u8>(sumA / n);
            }
        }
    }
    return dst;
}

} // namespace mari::ora
