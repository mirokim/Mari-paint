// Mari Paint — 합성기 구현. 선언은 include/mari/core/compositor.hpp.
#include <mari/core/blend.hpp>
#include <mari/core/compositor.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace mari {
namespace {

constexpr usize kRgbaBpp = 4u;

/// dst 를 투명(0)으로 채운다.
void clearArea(u8* dst, const Rect& area, usize stride) {
    const usize rowBytes = static_cast<usize>(area.width) * kRgbaBpp;
    for (i32 y = 0; y < area.height; ++y)
        std::memset(dst + static_cast<usize>(y) * stride, 0, rowBytes);
}

Result<void> compositeList(const std::vector<LayerPtr>& layers, const Rect& area, u8* dst,
                           usize stride);

/// 래스터 레이어 한 장을 dst 위에 올린다. 할당된 타일만 훑는다.
Result<void> compositeRaster(const Layer& layer, const Rect& area, u8* dst, usize stride) {
    const TileMap* tm = layer.tiles();
    if (tm == nullptr)
        return Ok();
    if (tm->format() != PixelFormat::RGBA8)
        return Err("합성기는 RGBA8 레이어만 다룬다", ErrorCode::Unsupported);

    const TileMap* mask = layer.mask();
    if (mask != nullptr && mask->format() != PixelFormat::Gray8)
        return Err("레이어 마스크는 Gray8 이어야 한다", ErrorCode::Unsupported);

    const BlendMode mode = layer.blendMode();
    const f32 opacity = layer.opacity();

    const i32 tx0 = tileIndexFor(area.x);
    const i32 ty0 = tileIndexFor(area.y);
    const i32 tx1 = tileIndexFor(area.right() - 1);
    const i32 ty1 = tileIndexFor(area.bottom() - 1);

    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const TileCoord c{tx, ty};
            const ConstTilePtr tile = tm->at(c);
            if (!tile || tile->isBlank())
                continue; // 없는 타일 = 완전 투명 — 합성에서 통째로 건너뛴다

            // 마스크가 붙었는데 이 타일에 마스크가 없으면 전부 가려진 것으로 본다.
            ConstTilePtr maskTile;
            if (mask != nullptr) {
                maskTile = mask->at(c);
                if (!maskTile)
                    continue;
            }

            const Rect r = c.canvasRect().intersected(area);
            if (r.isEmpty())
                continue;
            const i32 ox = tileOrigin(tx);
            const i32 oy = tileOrigin(ty);

            for (i32 y = r.y; y < r.bottom(); ++y) {
                const u8* srow = tile->pixels() + static_cast<usize>(y - oy) * tile->stride() +
                                 static_cast<usize>(r.x - ox) * kRgbaBpp;
                u8* drow = dst + static_cast<usize>(y - area.y) * stride +
                           static_cast<usize>(r.x - area.x) * kRgbaBpp;
                const u8* mrow = nullptr;
                if (maskTile)
                    mrow = maskTile->pixels() + static_cast<usize>(y - oy) * maskTile->stride() +
                           static_cast<usize>(r.x - ox);
                blendRowRgba8(mode, drow, srow, r.width, opacity, mrow);
            }
        }
    }
    return Ok();
}

/// 임시 버퍼(그룹 합성 결과)를 dst 위에 한 덩어리로 올린다.
void compositeBuffer(BlendMode mode, f32 opacity, const Rect& area, u8* dst, usize dstStride,
                     const u8* src, usize srcStride) {
    for (i32 y = 0; y < area.height; ++y) {
        blendRowRgba8(mode, dst + static_cast<usize>(y) * dstStride,
                      src + static_cast<usize>(y) * srcStride, area.width, opacity, nullptr);
    }
}

Result<void> compositeList(const std::vector<LayerPtr>& layers, const Rect& area, u8* dst,
                           usize stride) {
    // 인덱스 0 = 가장 아래. 아래에서 위로 올라간다.
    for (const LayerPtr& layer : layers) {
        if (!layer || !layer->visible() || layer->opacity() <= 0.0f)
            continue;

        if (layer->kind() == LayerKind::Group) {
            if (layer->children().empty())
                continue;
            // 그룹은 격리해서 먼저 합친 뒤, 그 결과를 한 덩어리로 올린다.
            const usize tmpStride = static_cast<usize>(area.width) * kRgbaBpp;
            std::vector<u8> tmp(tmpStride * static_cast<usize>(area.height), u8{0});
            auto r = compositeList(layer->children(), area, tmp.data(), tmpStride);
            if (!r.ok())
                return r;
            compositeBuffer(layer->blendMode(), layer->opacity(), area, dst, stride, tmp.data(),
                            tmpStride);
        } else {
            auto r = compositeRaster(*layer, area, dst, stride);
            if (!r.ok())
                return r;
        }
    }
    return Ok();
}

} // namespace

void dedupDirtyTiles(DirtyTiles& tiles) {
    std::sort(tiles.begin(), tiles.end(), [](const TileCoord& a, const TileCoord& b) {
        return a.ty != b.ty ? a.ty < b.ty : a.tx < b.tx;
    });
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
}

Rect dirtyTilesBounds(const DirtyTiles& tiles) {
    Rect r{};
    for (const TileCoord& c : tiles)
        r = r.united(c.canvasRect());
    return r;
}

Result<void> compositeArea(const LayerTree& tree, const Rect& area, u8* dst, usize dstStride) {
    if (area.isEmpty())
        return Ok();
    if (dst == nullptr)
        return Err("출력 버퍼가 없다", ErrorCode::InvalidArgument);
    if (dstStride < static_cast<usize>(area.width) * kRgbaBpp)
        return Err("dstStride 가 영역 너비보다 작다", ErrorCode::InvalidArgument);

    clearArea(dst, area, dstStride);
    return compositeList(tree.roots(), area, dst, dstStride);
}

Result<void> compositeDirty(const LayerTree& tree, const DirtyTiles& dirty, const Rect& dstArea,
                            u8* dst, usize dstStride) {
    if (dirty.empty() || dstArea.isEmpty())
        return Ok();
    if (dst == nullptr)
        return Err("출력 버퍼가 없다", ErrorCode::InvalidArgument);
    if (dstStride < static_cast<usize>(dstArea.width) * kRgbaBpp)
        return Err("dstStride 가 영역 너비보다 작다", ErrorCode::InvalidArgument);

    DirtyTiles unique(dirty);
    dedupDirtyTiles(unique);

    for (const TileCoord& c : unique) {
        const Rect r = c.canvasRect().intersected(dstArea);
        if (r.isEmpty())
            continue; // 화면 밖 더티 타일은 그냥 버린다
        u8* sub = dst + static_cast<usize>(r.y - dstArea.y) * dstStride +
                  static_cast<usize>(r.x - dstArea.x) * kRgbaBpp;
        clearArea(sub, r, dstStride);
        auto res = compositeList(tree.roots(), r, sub, dstStride);
        if (!res.ok())
            return res;
    }
    return Ok();
}

} // namespace mari
