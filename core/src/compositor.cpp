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
/// extraMask 는 area 크기의 Gray8 버퍼(클리핑 기준 알파). nullptr 이면 없음.
Result<void> compositeRaster(const Layer& layer, const Rect& area, u8* dst, usize stride,
                             const u8* extraMask = nullptr, usize extraStride = 0) {
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
                if (extraMask != nullptr) {
                    const u8* erow = extraMask + static_cast<usize>(y - area.y) * extraStride +
                                     static_cast<usize>(r.x - area.x);
                    if (mrow == nullptr) {
                        mrow = erow;
                    } else {
                        // 레이어 마스크 × 클리핑 알파. 한 줄짜리 작업 버퍼(타일 폭 이하).
                        u8 combined[kTileSize];
                        for (i32 i = 0; i < r.width; ++i)
                            combined[i] = static_cast<u8>((static_cast<u32>(mrow[i]) * erow[i] + 127u) / 255u);
                        mrow = combined;
                    }
                }
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

/// 레이어 하나(그룹 포함)를 **격리 버퍼**에 불투명도 1·Normal 로 그린다. 클리핑 묶음의 기준용.
Result<void> compositeIsolated(const Layer& layer, const Rect& area, u8* tmp, usize tmpStride) {
    if (layer.kind() == LayerKind::Group) {
        return compositeList(layer.children(), area, tmp, tmpStride);
    }
    // 기준 레이어의 마스크는 존중하되 불투명도·블렌드는 묶음 전체에 나중에 적용한다.
    struct Plain final : Layer {
        const Layer& l;
        explicit Plain(const Layer& x) : l(x) {}
        LayerId id() const override { return l.id(); }
        LayerKind kind() const override { return l.kind(); }
        const std::string& name() const override { return l.name(); }
        void setName(std::string) override {}
        f32 opacity() const override { return 1.0f; }
        void setOpacity(f32) override {}
        BlendMode blendMode() const override { return BlendMode::Normal; }
        void setBlendMode(BlendMode) override {}
        bool visible() const override { return true; }
        void setVisible(bool) override {}
        bool locked() const override { return l.locked(); }
        void setLocked(bool) override {}
        bool alphaLocked() const override { return l.alphaLocked(); }
        void setAlphaLocked(bool) override {}
        bool clipToBelow() const override { return false; }
        void setClipToBelow(bool) override {}
        TileMap* tiles() override { return nullptr; }
        const TileMap* tiles() const override { return l.tiles(); }
        const TileMap* mask() const override { return l.mask(); }
        void setMask(TileMapPtr) override {}
        Rect bounds() const override { return l.bounds(); }
        const std::vector<LayerPtr>& children() const override { return l.children(); }
    };
    return compositeRaster(Plain(layer), area, tmp, tmpStride);
}

Result<void> compositeList(const std::vector<LayerPtr>& layers, const Rect& area, u8* dst,
                           usize stride) {
    // 인덱스 0 = 가장 아래. 아래에서 위로 올라간다.
    for (usize idx = 0; idx < layers.size(); ++idx) {
        const LayerPtr& layer = layers[idx];
        if (!layer || !layer->visible() || layer->opacity() <= 0.0f)
            continue;
        if (layer->clipToBelow())
            continue; // 기준 레이어가 처리한다(기준이 없거나 숨겨졌으면 안 보이는 게 맞다)

        // 🔴 클리핑 묶음: 이 레이어 위에 연속된 clipToBelow 레이어가 있으면
        //    ① 기준을 격리 버퍼에 그리고 ② 그 알파를 마스크로 클립 레이어들을 격리 버퍼에 올린 뒤
        //    ③ 버퍼 전체를 기준의 불투명도·블렌드로 dst 에 올린다(CSP 규약).
        usize clipEnd = idx + 1;
        while (clipEnd < layers.size() && layers[clipEnd] && layers[clipEnd]->clipToBelow())
            ++clipEnd;
        if (clipEnd > idx + 1) {
            const usize tmpStride = static_cast<usize>(area.width) * kRgbaBpp;
            std::vector<u8> tmp(tmpStride * static_cast<usize>(area.height), u8{0});
            auto r = compositeIsolated(*layer, area, tmp.data(), tmpStride);
            if (!r.ok())
                return r;
            // 기준 알파 → Gray8 마스크
            std::vector<u8> alpha(static_cast<usize>(area.width) * static_cast<usize>(area.height));
            for (i32 y = 0; y < area.height; ++y)
                for (i32 x = 0; x < area.width; ++x)
                    alpha[static_cast<usize>(y) * static_cast<usize>(area.width) + static_cast<usize>(x)] =
                        tmp[static_cast<usize>(y) * tmpStride + static_cast<usize>(x) * kRgbaBpp + 3];
            for (usize k = idx + 1; k < clipEnd; ++k) {
                const LayerPtr& c = layers[k];
                if (!c->visible() || c->opacity() <= 0.0f)
                    continue;
                if (c->kind() == LayerKind::Group) {
                    std::vector<u8> g(tmpStride * static_cast<usize>(area.height), u8{0});
                    auto gr = compositeList(c->children(), area, g.data(), tmpStride);
                    if (!gr.ok())
                        return gr;
                    for (i32 y = 0; y < area.height; ++y)
                        blendRowRgba8(c->blendMode(), tmp.data() + static_cast<usize>(y) * tmpStride,
                                      g.data() + static_cast<usize>(y) * tmpStride, area.width, c->opacity(),
                                      alpha.data() + static_cast<usize>(y) * static_cast<usize>(area.width));
                } else {
                    auto cr = compositeRaster(*c, area, tmp.data(), tmpStride, alpha.data(),
                                              static_cast<usize>(area.width));
                    if (!cr.ok())
                        return cr;
                }
            }
            compositeBuffer(layer->blendMode(), layer->opacity(), area, dst, stride, tmp.data(), tmpStride);
            idx = clipEnd - 1;
            continue;
        }

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
