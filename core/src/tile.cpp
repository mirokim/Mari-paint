// Mari Paint — 타일 캔버스 구현. 선언은 include/mari/core/tile.hpp.
//
// docs/02 3절의 "복제가 O(1)" 은 Copy-on-Write 에서 나온다. 실체는 이렇다.
//   1) 픽셀 버퍼 COW — BufferTile 은 16KB 픽셀 버퍼를 shared_ptr 로 공유한다.
//                      mutablePixels() 가 불릴 때 비로소 갈라선다. clone() 은 O(1).
//   2) 타일 객체 단독 소유 — 한 TileMap 안의 Tile 객체는 그 맵만 갖는다.
//                      snapshot() 은 타일 **핸들만** 복제한다(픽셀 복사는 0 바이트).
//
// 왜 맵 자체를 공유(진짜 O(1))하지 않는가:
//   writable() 이 돌려준 TilePtr 를 붙잡은 채 snapshot() 을 뜨면, 맵을 공유한 경우
//   그 포인터로 쓴 내용이 스냅샷까지 오염시킨다(조용한 버그다). 타일 객체를
//   미리 갈라 두면 그 경로가 막힌다. 스냅샷 비용은 "할당된 타일 수만큼의 작은
//   핸들 복제" 이고 픽셀은 한 바이트도 복사하지 않는다 — 8K 레이어 기준 수백 KB가
//   아니라 수백 μs 수준이다.
#include <mari/core/tile.hpp>

#include <cstring>
#include <unordered_map>

namespace mari {
namespace {

/// 임의 PixelFormat 을 담는 64×64 타일. 새로 만들면 전부 0 = 완전 투명이다.
/// 픽셀 버퍼는 COW 로 공유하므로 clone() 이 O(1) 이다.
class BufferTile final : public Tile {
public:
    explicit BufferTile(PixelFormat f)
        : format_(f), stride_(static_cast<usize>(kTileSize) * bytesPerPixel(f)),
          data_(std::make_shared<std::vector<u8>>(stride_ * static_cast<usize>(kTileSize), u8{0})) {
    }

    PixelFormat format() const override { return format_; }
    const u8* pixels() const override { return data_->data(); }

    u8* mutablePixels() override {
        ensureUnique();
        blankKnown_ = false; // 내용이 바뀔 수 있다 — 캐시를 버린다
        return data_->data();
    }

    usize stride() const override { return stride_; }

    bool isBlank() const override {
        if (!blankKnown_) {
            blank_ = computeBlank();
            blankKnown_ = true;
        }
        return blank_;
    }

    /// 픽셀을 복사하지 않는다 — 버퍼를 공유하고 쓸 때 갈라선다.
    std::shared_ptr<Tile> clone() const override { return std::make_shared<BufferTile>(*this); }

private:
    /// 버퍼가 남과 공유 중이면 여기서 진짜 복사가 일어난다.
    void ensureUnique() {
        if (data_.use_count() > 1)
            data_ = std::make_shared<std::vector<u8>>(*data_);
    }

    /// 알파가 전부 0이면 비었다고 본다. 알파가 없는 포맷(Gray)은 값이 전부 0일 때다.
    [[nodiscard]] bool computeBlank() const {
        const std::vector<u8>& b = *data_;
        const usize count = static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize);
        switch (format_) {
        case PixelFormat::RGBA8:
            for (usize i = 0; i < count; ++i)
                if (b[i * 4u + 3u] != 0)
                    return false;
            return true;
        case PixelFormat::RGBA16:
            for (usize i = 0; i < count; ++i)
                if (b[i * 8u + 6u] != 0 || b[i * 8u + 7u] != 0)
                    return false;
            return true;
        case PixelFormat::RGBA32F:
            for (usize i = 0; i < count; ++i) {
                f32 a = 0.0f;
                std::memcpy(&a, b.data() + i * 16u + 12u, sizeof(f32));
                if (a != 0.0f)
                    return false;
            }
            return true;
        case PixelFormat::Gray8:
        case PixelFormat::Gray16:
        case PixelFormat::Unknown:
            break;
        }
        for (u8 v : b)
            if (v != 0)
                return false;
        return true;
    }

    PixelFormat format_;
    usize stride_;
    std::shared_ptr<std::vector<u8>> data_;
    mutable bool blank_ = true;
    mutable bool blankKnown_ = true;
};

/// 희소 타일 맵. 없는 좌표는 nullptr = 완전 투명 — 메모리를 쓰지 않는다.
class SparseTileMap final : public TileMap {
public:
    explicit SparseTileMap(PixelFormat f) : format_(f) {}

    PixelFormat format() const override { return format_; }
    usize tileCount() const override { return tiles_.size(); }

    Rect bounds() const override {
        Rect r{};
        for (const auto& [c, t] : tiles_) {
            (void)t;
            r = r.united(c.canvasRect());
        }
        return r;
    }

    ConstTilePtr at(TileCoord c) const override {
        const auto it = tiles_.find(c);
        return it == tiles_.end() ? nullptr : it->second;
    }

    Result<TilePtr> writable(TileCoord c) override {
        if (format_ == PixelFormat::Unknown)
            return Err("Unknown 포맷 타일맵에는 쓸 수 없다", ErrorCode::InvalidArgument);
        auto it = tiles_.find(c);
        if (it == tiles_.end())
            it = tiles_.emplace(c, std::make_shared<BufferTile>(format_)).first;
        return it->second;
    }

    void erase(TileCoord c) override { tiles_.erase(c); }

    void clear() override { tiles_.clear(); }

    void collectTiles(const Rect& canvasArea, DirtyTiles& out) const override {
        if (canvasArea.isEmpty() || tiles_.empty())
            return;
        const i32 tx0 = tileIndexFor(canvasArea.x);
        const i32 ty0 = tileIndexFor(canvasArea.y);
        const i32 tx1 = tileIndexFor(canvasArea.right() - 1);
        const i32 ty1 = tileIndexFor(canvasArea.bottom() - 1);
        const i64 span = static_cast<i64>(tx1 - tx0 + 1) * static_cast<i64>(ty1 - ty0 + 1);
        if (span <= static_cast<i64>(tiles_.size())) {
            // 영역이 좁다 — 좌표를 훑으며 조회한다.
            for (i32 ty = ty0; ty <= ty1; ++ty)
                for (i32 tx = tx0; tx <= tx1; ++tx) {
                    const TileCoord c{tx, ty};
                    if (tiles_.find(c) != tiles_.end())
                        out.push_back(c);
                }
        } else {
            // 할당된 타일이 더 적다 — 맵을 훑는다.
            for (const auto& [c, t] : tiles_) {
                (void)t;
                if (canvasArea.intersects(c.canvasRect()))
                    out.push_back(c);
            }
        }
    }

    /// 타일 핸들만 복제한다. 픽셀 버퍼는 공유되고, 어느 쪽이든 쓸 때 갈라선다.
    std::shared_ptr<TileMap> snapshot() const override {
        auto s = std::make_shared<SparseTileMap>(format_);
        s->tiles_.reserve(tiles_.size());
        for (const auto& [c, t] : tiles_)
            s->tiles_.emplace(c, t->clone());
        return s;
    }

private:
    PixelFormat format_;
    std::unordered_map<TileCoord, TilePtr, TileCoordHash> tiles_;
};

} // namespace

Result<TileMapPtr> makeTileMap(PixelFormat format) {
    if (format == PixelFormat::Unknown || bytesPerPixel(format) == 0)
        return Err("알 수 없는 픽셀 포맷이다", ErrorCode::InvalidArgument);
    return TileMapPtr{std::make_shared<SparseTileMap>(format)};
}

} // namespace mari
