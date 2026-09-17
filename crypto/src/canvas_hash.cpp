// Mari Paint — 픽셀 스냅샷 해시 구현. 규약은 canvas_hash.hpp 주석이 정본이다.
#include <mari/crypto/canvas_hash.hpp>

#include <mari/core/compositor.hpp>

#include <algorithm>
#include <vector>

namespace mari::crypto {
namespace {

/// 리틀엔디안 u32 를 먹인다.
void feedLe32(Sha256& h, u32 v) noexcept {
    const u8 b[4] = {static_cast<u8>(v & 0xFFu), static_cast<u8>((v >> 8) & 0xFFu),
                     static_cast<u8>((v >> 16) & 0xFFu), static_cast<u8>((v >> 24) & 0xFFu)};
    h.update(b, 4);
}

/// 리틀엔디안 u64 를 먹인다.
void feedLe64(Sha256& h, u64 v) noexcept {
    u8 b[8];
    for (usize i = 0; i < 8; ++i) {
        b[i] = static_cast<u8>((v >> (8u * i)) & 0xFFu);
    }
    h.update(b, 8);
}

/// i32 는 2의 보수 비트열을 그대로 리틀엔디안으로 싣는다.
void feedLeI32(Sha256& h, i32 v) noexcept { feedLe32(h, static_cast<u32>(v)); }

/// 규약의 "알파 0 정규화". 제자리에서 고친다.
void normalizeTransparent(u8* rgba, usize pixelCount) noexcept {
    for (usize i = 0; i < pixelCount; ++i) {
        u8* px = rgba + i * 4u;
        if (px[3] == 0) {
            px[0] = 0;
            px[1] = 0;
            px[2] = 0;
        }
    }
}

/// 문자열 리터럴 도메인 접두사를 먹인다(널 종단 제외).
void feedDomain(Sha256& h, const char* domain) noexcept {
    usize n = 0;
    while (domain[n] != '\0') {
        ++n;
    }
    h.update(domain, n);
}

} // namespace

Result<std::string> canvasHashHexOfPixels(i32 width, i32 height, const u8* rgba, usize stride) {
    if (width < 0 || height < 0) {
        return Err("캔버스 크기가 음수다", ErrorCode::InvalidArgument);
    }
    const usize rowBytes = static_cast<usize>(width) * 4u;
    if (stride == 0) {
        stride = rowBytes;
    }
    if (stride < rowBytes) {
        return Err("stride 가 한 행보다 작다", ErrorCode::InvalidArgument);
    }
    if (rgba == nullptr && width > 0 && height > 0) {
        return Err("픽셀 버퍼가 null 이다", ErrorCode::InvalidArgument);
    }

    Sha256 h;
    feedDomain(h, kCanvasHashDomain);
    feedLe32(h, static_cast<u32>(width));
    feedLe32(h, static_cast<u32>(height));

    // 행마다 정규화 사본을 하나만 들고 간다. 통짜 버퍼를 만들지 않는다.
    std::vector<u8> row(rowBytes);
    for (i32 y = 0; y < height; ++y) {
        if (rowBytes > 0) {
            std::copy_n(rgba + static_cast<usize>(y) * stride, rowBytes, row.begin());
            normalizeTransparent(row.data(), static_cast<usize>(width));
            h.update(row.data(), rowBytes);
        }
    }
    return Ok(toHex(h.finish()));
}

Result<std::string> canvasHashHex(const LayerTree& tree) {
    const Size sz = tree.canvasSize();
    if (sz.width < 0 || sz.height < 0) {
        return Err("캔버스 크기가 음수다", ErrorCode::InvalidArgument);
    }

    Sha256 h;
    feedDomain(h, kCanvasHashDomain);
    feedLe32(h, static_cast<u32>(sz.width));
    feedLe32(h, static_cast<u32>(sz.height));
    if (sz.isEmpty()) {
        return Ok(toHex(h.finish()));
    }

    // 🔴 가로 띠 단위로 합성해서 흘려보낸다(스트리밍). 8K 캔버스를 통째로 올리지 않는다.
    //    띠 버퍼가 4MiB 를 넘지 않게 행 수를 줄인다. 최소 한 행은 보장한다.
    const usize rowBytes = static_cast<usize>(sz.width) * 4u;
    static constexpr usize kBandBudget = 4u * 1024u * 1024u;
    i32 bandRows = kTileSize; // 타일 경계에 맞추면 합성기가 타일을 두 번 안 만진다
    if (rowBytes > 0) {
        const usize fit = kBandBudget / rowBytes;
        if (fit < static_cast<usize>(bandRows)) {
            bandRows = fit == 0 ? 1 : static_cast<i32>(fit);
        }
    }

    std::vector<u8> band(rowBytes * static_cast<usize>(bandRows));
    for (i32 y = 0; y < sz.height; y += bandRows) {
        const i32 rows = std::min(bandRows, sz.height - y);
        const Rect area{0, y, sz.width, rows};
        const usize used = rowBytes * static_cast<usize>(rows);
        std::fill_n(band.begin(), used, static_cast<u8>(0));
        const Result<void> r = compositeArea(tree, area, band.data(), rowBytes);
        if (!r.ok()) {
            return r.error();
        }
        normalizeTransparent(band.data(), static_cast<usize>(sz.width) * static_cast<usize>(rows));
        h.update(band.data(), used);
    }
    return Ok(toHex(h.finish()));
}

Result<std::string> tileMapHashHex(const TileMap& map) {
    if (map.format() != PixelFormat::RGBA8) {
        return Err("레이어 픽셀 해시는 RGBA8 타일맵만 받는다 — 추측해서 해시하지 않는다",
                   ErrorCode::Unsupported);
    }

    Sha256 h;
    feedDomain(h, kLayerHashDomain);
    feedLe32(h, static_cast<u32>(kTileSize));

    // 할당된 타일 전부를 모은다. bounds() 가 할당 타일의 경계 상자라 이걸로 충분하다.
    DirtyTiles tiles;
    const Rect b = map.bounds();
    if (!b.isEmpty()) {
        map.collectTiles(b, tiles);
    }
    // 규약이 정한 정렬: ty → tx 오름차순. 중복은 제거한다.
    std::sort(tiles.begin(), tiles.end(), [](const TileCoord& l, const TileCoord& r) {
        return l.ty != r.ty ? l.ty < r.ty : l.tx < r.tx;
    });
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());

    static constexpr usize kTileBytes =
        static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize) * 4u;
    std::vector<u8> buf(kTileBytes);
    u64 contributing = 0;

    for (const TileCoord& c : tiles) {
        const ConstTilePtr t = map.at(c);
        if (!t) {
            continue; // 없는 타일 = 빈 타일. 기여하지 않는다
        }
        // 🔴 알파가 전부 0 이면 기여하지 않는다 — 할당 여부는 구현 세부다.
        if (t->isBlank()) {
            continue;
        }
        const u8* src = t->pixels();
        const usize stride = t->stride();
        if (src == nullptr || stride < static_cast<usize>(kTileSize) * 4u) {
            return Err("타일 픽셀을 읽을 수 없다", ErrorCode::Unknown);
        }
        for (i32 row = 0; row < kTileSize; ++row) {
            std::copy_n(src + static_cast<usize>(row) * stride,
                        static_cast<usize>(kTileSize) * 4u,
                        buf.begin() + static_cast<usize>(row) * static_cast<usize>(kTileSize) * 4u);
        }
        normalizeTransparent(buf.data(), static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize));

        feedLeI32(h, c.tx);
        feedLeI32(h, c.ty);
        h.update(buf.data(), kTileBytes);
        ++contributing;
    }

    feedLe64(h, contributing);
    return Ok(toHex(h.finish()));
}

Result<std::string> layerHashHex(const Layer& layer) {
    const TileMap* map = layer.tiles();
    if (map == nullptr) {
        // 그룹 레이어 — 픽셀이 없다. 규약대로 "타일 0개" 해시를 낸다.
        Sha256 h;
        feedDomain(h, kLayerHashDomain);
        feedLe32(h, static_cast<u32>(kTileSize));
        feedLe64(h, 0u);
        return Ok(toHex(h.finish()));
    }
    return tileMapHashHex(*map);
}

} // namespace mari::crypto
