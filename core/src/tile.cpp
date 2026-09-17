// Mari Paint — 타일 캔버스 구현. 선언은 include/mari/core/tile.hpp.
//
// 두 겹의 Copy-on-Write 를 쓴다. docs/02 3절의 "O(1) 복제"가 여기서 나온다.
//   1) 타일 COW  — 타일 포인터가 공유 중이면 writable() 이 clone 해서 분리한다.
//   2) 맵 COW    — snapshot() 은 희소 맵 자체를 공유한다(진짜 O(1)).
//                  이후 첫 쓰기에서만 포인터 맵을 한 번 복사한다(픽셀 복사는 없다).
#include <mari/core/tile.hpp>

#include <cstring>
#include <unordered_map>

namespace mari {
namespace {

/// 임의 PixelFormat 을 담는 64×64 타일. 픽셀은 연속 버퍼 하나다.
/// 새로 만들면 전부 0 — 즉 완전 투명이다.
class BufferTile final : public Tile {
public:
    explicit BufferTile(PixelFormat f)
        : format_(f), stride_(static_cast<usize>(kTileSize) * bytesPerPixel(f)),
          buf_(stride_ * static_cast<usize>(kTileSize), u8{0}) {}

    PixelFormat format() const override { return format_; }
    const u8* pixels() const override { return buf_.data(); }
    u8* mutablePixels() override {
        blankKnown_ = false; // 내용이 바뀔 수 있다 — 캐시를 버린다
        return buf_.data();
    }
    usize stride() const override { return stride_; }

    bool isBlank() const override {
        if (!blankKnown_) {
            blank_ = computeBlank();
            blankKnown_ = true;
        }
        return blank_;
    }

    std::shared_ptr<Tile> clone() const override { return std::make_shared<BufferTile>(*this); }

private:
    /// 알파가 전부 0이면 비었다고 본다. 알파가 없는 포맷(Gray)은 값이 전부 0일 때다.
    [[nodiscard]] bool computeBlank() const {
        const usize count = static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize);
        switch (format_) {
        case PixelFormat::RGBA8:
            for (usize i = 0; i < count; ++i)
                if (buf_[i * 4u + 3u] != 0)
                    return false;
            return true;
        case PixelFormat::RGBA16:
            for (usize i = 0; i < count; ++i)
                if (buf_[i * 8u + 6u] != 0 || buf_[i * 8u + 7u] != 0)
                    return false;
            return true;
        case PixelFormat::RGBA32F:
            for (usize i = 0; i < count; ++i) {
                f32 a = 0.0f;
                std::memcpy(&a, buf_.data() + i * 16u + 12u, sizeof(f32));
                if (a != 0.0f)
                    return false;
            }
            return true;
        case PixelFormat::Gray8:
        case PixelFormat::Gray16:
        case PixelFormat::Unknown:
            break;
        }
        for (u8 v : buf_)
            if (v != 0)
                return false;
        return true;
    }

    PixelFormat format_;
    usize stride_;
    std::vector<u8> buf_;
    mutable bool blank_ = true;
    mutable bool blankKnown_ = true;
};

/// 희소 타일 맵. 없는 좌표는 nullptr = 완전 투명 — 메모리를 쓰지 않는다.
class SparseTileMap final : public TileMap {
public:
    explicit SparseTileMap(PixelFormat f)
        : format_(f), storage_(std::make_shared<Storage>()) {}

    PixelFormat format() const override { return format_; }
    usize tileCount() const override { return storage_->tiles.size(); }

    Rect bounds() const override {
        Rect r{};
        for (const auto& [c, t] : storage_->tiles)
            r = r.united(c.canvasRect());
        return r;
    }

    ConstTilePtr at(TileCoord c) const override {
        const auto it = storage_->tiles.find(c);
        return it == storage_->tiles.end() ? nullptr : it->second;
    }

    Result<TilePtr> writable(TileCoord c) override {
        if (format_ == PixelFormat::Unknown)
            return Err("Unknown 포맷 타일맵에는 쓸 수 없다", ErrorCode::InvalidArgument);
        detach();
        auto it = storage_->tiles.find(c);
        if (it == storage_->tiles.end()) {
            auto t = std::make_shared<BufferTile>(format_);
            if (!t)
                return Err("타일 할당 실패", ErrorCode::OutOfMemory);
            it = storage_->tiles.emplace(c, std::move(t)).first;
            return it->second;
        }
        if (it->second.use_count() > 1) {
            // 다른 스냅샷/레이어와 공유 중이다 — 여기서 갈라선다.
            auto copy = it->second->clone();
            if (!copy)
                return Err("타일 복제 실패", ErrorCode::OutOfMemory);
            it->second = std::move(copy);
        }
        return it->second;
    }

    void erase(TileCoord c) override {
        if (storage_->tiles.find(c) == storage_->tiles.end())
            return;
        detach();
        storage_->tiles.erase(c);
    }

    void clear() override {
        if (storage_->tiles.empty())
            return;
        storage_ = std::make_shared<Storage>(); // 공유본은 건드리지 않는다
    }

    void collectTiles(const Rect& canvasArea, DirtyTiles& out) const override {
        if (canvasArea.isEmpty() || storage_->tiles.empty())
            return;
        const i32 tx0 = tileIndexFor(canvasArea.x);
        const i32 ty0 = tileIndexFor(canvasArea.y);
        const i32 tx1 = tileIndexFor(canvasArea.right() - 1);
        const i32 ty1 = tileIndexFor(canvasArea.bottom() - 1);
        const i64 span = static_cast<i64>(tx1 - tx0 + 1) * static_cast<i64>(ty1 - ty0 + 1);
        if (span <= static_cast<i64>(storage_->tiles.size())) {
            // 영역이 좁다 — 좌표를 훑으며 조회한다.
            for (i32 ty = ty0; ty <= ty1; ++ty)
                for (i32 tx = tx0; tx <= tx1; ++tx) {
                    const TileCoord c{tx, ty};
                    if (storage_->tiles.find(c) != storage_->tiles.end())
                        out.push_back(c);
                }
        } else {
            // 할당된 타일이 적다 — 맵을 훑는다.
            for (const auto& [c, t] : storage_->tiles) {
                (void)t;
                if (canvasArea.intersects(c.canvasRect()))
                    out.push_back(c);
            }
        }
    }

    std::shared_ptr<TileMap> snapshot() const override {
        auto s = std::make_shared<SparseTileMap>(format_);
        s->storage_ = storage_; // 맵째로 공유한다 — O(1)
        return s;
    }

private:
    struct Storage {
        std::unordered_map<TileCoord, TilePtr, TileCoordHash> tiles;
    };

    /// 맵이 공유 중이면 포인터만 복사해 갈라선다(픽셀은 복사하지 않는다).
    void detach() {
        if (storage_.use_count() > 1)
            storage_ = std::make_shared<Storage>(*storage_);
    }

    PixelFormat format_;
    std::shared_ptr<Storage> storage_;
};

} // namespace

Result<TileMapPtr> makeTileMap(PixelFormat format) {
    if (format == PixelFormat::Unknown || bytesPerPixel(format) == 0)
        return Err("알 수 없는 픽셀 포맷이다", ErrorCode::InvalidArgument);
    auto map = std::make_shared<SparseTileMap>(format);
    if (!map)
        return Err("타일맵 할당 실패", ErrorCode::OutOfMemory);
    return TileMapPtr{std::move(map)};
}

} // namespace mari
