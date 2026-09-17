// stroke 테스트용 최소 RGBA8 타일맵.
// core 의 makeTileMap() 구현과 무관하게 stroke 를 혼자 검증하려고 둔다
// (다른 에이전트의 진행 상태에 테스트가 묶이면 안 된다).
#ifndef MARI_TESTS_STROKE_FAKE_TILEMAP_HPP
#define MARI_TESTS_STROKE_FAKE_TILEMAP_HPP

#include <mari/core/tile.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace mari::test {

/// 64×64 RGBA8 타일.
class FakeTile final : public Tile {
public:
    FakeTile() : buf_(static_cast<usize>(kTileSize) * kTileSize * 4u, 0u) {}

    PixelFormat format() const override { return PixelFormat::RGBA8; }
    const u8* pixels() const override { return buf_.data(); }
    u8* mutablePixels() override { return buf_.data(); }
    usize stride() const override { return static_cast<usize>(kTileSize) * 4u; }
    bool isBlank() const override {
        for (usize i = 3; i < buf_.size(); i += 4)
            if (buf_[i] != 0)
                return false;
        return true;
    }
    std::shared_ptr<Tile> clone() const override { return std::make_shared<FakeTile>(*this); }

private:
    std::vector<u8> buf_;
};

/// 희소 타일맵. COW 로 분리한다.
class FakeTileMap final : public TileMap {
public:
    PixelFormat format() const override { return PixelFormat::RGBA8; }
    usize tileCount() const override { return tiles_.size(); }
    Rect bounds() const override {
        Rect r{};
        for (const auto& kv : tiles_)
            r = r.united(kv.first.canvasRect());
        return r;
    }
    ConstTilePtr at(TileCoord c) const override {
        auto it = tiles_.find(c);
        return it == tiles_.end() ? nullptr : it->second;
    }
    Result<TilePtr> writable(TileCoord c) override {
        auto it = tiles_.find(c);
        if (it == tiles_.end())
            it = tiles_.emplace(c, std::make_shared<FakeTile>()).first;
        else if (it->second.use_count() > 1)
            it->second = it->second->clone();
        return it->second;
    }
    void erase(TileCoord c) override { tiles_.erase(c); }
    void clear() override { tiles_.clear(); }
    void collectTiles(const Rect& area, DirtyTiles& out) const override {
        for (const auto& kv : tiles_)
            if (area.intersects(kv.first.canvasRect()))
                out.push_back(kv.first);
    }
    std::shared_ptr<TileMap> snapshot() const override {
        return std::make_shared<FakeTileMap>(*this);
    }

    /// 픽셀 하나(캔버스 좌표). 없는 타일은 완전 투명.
    [[nodiscard]] Color8 pixelAt(i32 x, i32 y) const {
        const TileCoord c{tileIndexFor(x), tileIndexFor(y)};
        auto it = tiles_.find(c);
        if (it == tiles_.end())
            return Color8{0, 0, 0, 0};
        const u8* p = it->second->pixels() +
                      static_cast<usize>(y - tileOrigin(c.ty)) * it->second->stride() +
                      static_cast<usize>(x - tileOrigin(c.tx)) * 4u;
        return Color8{p[0], p[1], p[2], p[3]};
    }

    /// 알파가 0이 아닌 픽셀이 하나라도 있는 타일 좌표(정렬됨).
    [[nodiscard]] DirtyTiles paintedTiles() const {
        DirtyTiles out;
        for (const auto& kv : tiles_)
            if (!kv.second->isBlank())
                out.push_back(kv.first);
        sortTiles(out);
        return out;
    }

    /// 실제로 잉크가 닿은 픽셀들의 경계 상자(캔버스 좌표).
    [[nodiscard]] Rect paintedBounds() const {
        Rect r{};
        for (const auto& kv : tiles_) {
            const i32 ox = tileOrigin(kv.first.tx);
            const i32 oy = tileOrigin(kv.first.ty);
            const u8* base = kv.second->pixels();
            for (i32 y = 0; y < kTileSize; ++y)
                for (i32 x = 0; x < kTileSize; ++x)
                    if (base[static_cast<usize>(y) * kv.second->stride() +
                             static_cast<usize>(x) * 4u + 3u] != 0)
                        r = r.united(Rect{ox + x, oy + y, 1, 1});
        }
        return r;
    }

    static void sortTiles(DirtyTiles& v) {
        std::sort(v.begin(), v.end(), [](const TileCoord& a, const TileCoord& b) {
            return a.ty != b.ty ? a.ty < b.ty : a.tx < b.tx;
        });
    }

private:
    std::unordered_map<TileCoord, TilePtr, TileCoordHash> tiles_;
};

} // namespace mari::test

#endif // MARI_TESTS_STROKE_FAKE_TILEMAP_HPP
