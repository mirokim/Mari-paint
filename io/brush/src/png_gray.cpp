// Mari Paint — PNG → GrayImage 디코더 구현 (libpng).
#include <mari/io/brush/png_gray.hpp>

#include <png.h>

#include <csetjmp>
#include <memory>
#include <cstring>
#include <vector>

namespace mari::io::brush {
namespace {

/// 캔버스 폭·높이 상한. 브러시 텍스처가 이보다 크면 뭔가 잘못된 파일이다.
constexpr png_uint_32 kMaxDim = 16384;

struct MemSource {
    const u8* data;
    usize size;
    usize pos;
};

void readFromMemory(png_structp png, png_bytep out, png_size_t want) {
    auto* src = static_cast<MemSource*>(png_get_io_ptr(png));
    if (src == nullptr || src->size - src->pos < want) {
        png_error(png, "PNG 데이터가 잘렸다");
        return;
    }
    std::memcpy(out, src->data + src->pos, want);
    src->pos += want;
}

void onError(png_structp png, png_const_charp) { longjmp(png_jmpbuf(png), 1); }
void onWarning(png_structp, png_const_charp) {} // 경고는 무시한다

} // namespace

bool looksLikePng(const u8* data, usize size) {
    static constexpr u8 kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data != nullptr && size >= 8 && std::memcmp(data, kSig, 8) == 0;
}

Result<mari::brush::GrayImage> decodePngGray(const u8* data, usize size, bool invert,
                                             PngGrayInfo* info) {
    if (!looksLikePng(data, size))
        return Err("PNG 시그니처가 아니다", ErrorCode::ParseError);

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, onError, onWarning);
    if (png == nullptr)
        return Err("libpng 읽기 구조체를 만들지 못했다", ErrorCode::OutOfMemory);
    png_infop pngInfo = png_create_info_struct(png);
    if (pngInfo == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return Err("libpng 정보 구조체를 만들지 못했다", ErrorCode::OutOfMemory);
    }

    // setjmp 이후에 바뀌는 지역 변수는 longjmp 뒤 값이 미정의다(그리고 소멸자도 안 돈다).
    // 그래서 작업 버퍼를 setjmp **전에** 힙에 잡아 두고, 포인터 변수는 이후 건드리지 않는다.
    struct Workspace {
        mari::brush::GrayImage out;
        std::vector<png_bytep> rows;
        std::vector<u8> buffer;
        PngGrayInfo local;
    };
    const auto ws = std::make_unique<Workspace>();
    auto& out = ws->out;
    auto& rows = ws->rows;
    auto& buffer = ws->buffer;
    auto& local = ws->local;

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &pngInfo, nullptr);
        return Err("PNG 디코드에 실패했다", ErrorCode::ParseError);
    }

    MemSource src{data, size, 0};
    png_set_read_fn(png, &src, readFromMemory);
    png_read_info(png, pngInfo);

    const png_uint_32 w = png_get_image_width(png, pngInfo);
    const png_uint_32 h = png_get_image_height(png, pngInfo);
    const int bitDepth = png_get_bit_depth(png, pngInfo);
    const int colorType = png_get_color_type(png, pngInfo);

    if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim) {
        png_destroy_read_struct(&png, &pngInfo, nullptr);
        return Err("PNG 크기가 비정상이다", ErrorCode::ParseError);
    }

    local.sourceBitDepth = bitDepth;
    local.hadColor = (colorType & PNG_COLOR_MASK_COLOR) != 0;
    local.hadAlpha = (colorType & PNG_COLOR_MASK_ALPHA) != 0 ||
                     png_get_valid(png, pngInfo, PNG_INFO_tRNS) != 0;

    // 무엇이 오든 8비트 그레이(+알파) 로 접는다.
    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, pngInfo, PNG_INFO_tRNS) != 0)
        png_set_tRNS_to_alpha(png);
    if (bitDepth == 16)
        png_set_strip_16(png);
    if ((png_get_color_type(png, pngInfo) & PNG_COLOR_MASK_COLOR) != 0)
        png_set_rgb_to_gray(png, PNG_ERROR_ACTION_NONE, -1.0, -1.0);
    png_read_update_info(png, pngInfo);

    const png_size_t stride = png_get_rowbytes(png, pngInfo);
    const int channels = png_get_channels(png, pngInfo);
    buffer.assign(static_cast<usize>(stride) * h, 0);
    rows.resize(h);
    for (png_uint_32 y = 0; y < h; ++y)
        rows[y] = buffer.data() + static_cast<usize>(stride) * y;
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &pngInfo, nullptr);

    out.width = static_cast<i32>(w);
    out.height = static_cast<i32>(h);
    out.pixels.resize(static_cast<usize>(w) * h);
    for (png_uint_32 y = 0; y < h; ++y) {
        const u8* row = buffer.data() + static_cast<usize>(stride) * y;
        for (png_uint_32 x = 0; x < w; ++x) {
            u32 v = row[static_cast<usize>(x) * static_cast<usize>(channels)];
            if (channels >= 2) {
                // 투명한 곳은 "잉크 없음" 쪽으로 합성한다. 뒤집기 전 기준은 흰색이다.
                const u32 a = row[static_cast<usize>(x) * static_cast<usize>(channels) + 1];
                v = (v * a + 255u * (255u - a)) / 255u;
            }
            if (invert)
                v = 255u - v;
            out.pixels[static_cast<usize>(y) * w + x] = static_cast<u8>(v);
        }
    }

    if (info != nullptr)
        *info = local;
    return Ok(std::move(out));
}

} // namespace mari::io::brush
