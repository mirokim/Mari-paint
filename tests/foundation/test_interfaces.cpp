// 인터페이스 헤더 계약 테스트.
// 구현은 다른 에이전트가 하지만, 선언이 **실제로 구현 가능한지**는 지금 확인해야 한다.
// 여기서 순수 가상 함수를 전부 구현한 더미가 컴파일되면 시그니처에 모순이 없다는 뜻이다.
#include <mari/brush/engine.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/tile.hpp>
#include <mari/test/harness.hpp>

#include <array>

using namespace mari;

namespace {

/// 64x64 RGBA8 더미 타일.
class DummyTile final : public Tile {
public:
    PixelFormat format() const override { return PixelFormat::RGBA8; }
    const u8* pixels() const override { return buf_.data(); }
    u8* mutablePixels() override { return buf_.data(); }
    usize stride() const override { return kTileSize * 4; }
    bool isBlank() const override { return blank_; }
    std::shared_ptr<Tile> clone() const override { return std::make_shared<DummyTile>(*this); }

private:
    std::array<u8, kTileSize * kTileSize * 4> buf_{};
    bool blank_ = true;
};

/// 타일 한 장만 갖는 최소 타일맵.
class DummyTileMap final : public TileMap {
public:
    PixelFormat format() const override { return PixelFormat::RGBA8; }
    usize tileCount() const override { return tile_ ? 1u : 0u; }
    Rect bounds() const override { return tile_ ? coord_.canvasRect() : Rect{}; }
    ConstTilePtr at(TileCoord c) const override {
        return (tile_ && c == coord_) ? tile_ : nullptr;
    }
    Result<TilePtr> writable(TileCoord c) override {
        if (!tile_) {
            tile_ = std::make_shared<DummyTile>();
            coord_ = c;
        }
        if (!(c == coord_))
            return Err("더미는 타일 하나만 다룬다", ErrorCode::Unsupported);
        return tile_;
    }
    void erase(TileCoord c) override {
        if (tile_ && c == coord_)
            tile_.reset();
    }
    void clear() override { tile_.reset(); }
    void collectTiles(const Rect& area, DirtyTiles& out) const override {
        if (tile_ && area.intersects(coord_.canvasRect()))
            out.push_back(coord_);
    }
    std::shared_ptr<TileMap> snapshot() const override {
        return std::make_shared<DummyTileMap>(*this); // COW: 타일 포인터만 공유
    }

private:
    TilePtr tile_;
    TileCoord coord_{};
};

/// 아무것도 안 그리지만 계약은 지키는 엔진.
class DummyEngine final : public brush::IBrushEngine {
public:
    const char* name() const noexcept override { return "dummy"; }
    Result<void> setPreset(const brush::MariBrushPreset& p, brush::ImportReport* rep) override {
        spacing_ = p.spacing * p.tip.diameter;
        if (rep && p.texture.has_value())
            rep->add(brush::ImportSeverity::Dropped, "texture", "더미 엔진은 텍스처를 모릅니다");
        return Ok();
    }
    Result<void> beginStroke(const brush::StrokeContext& ctx) override {
        if (ctx.target == nullptr)
            return Err("대상 타일맵이 없다", ErrorCode::InvalidArgument);
        target_ = ctx.target;
        return Ok();
    }
    void stamp(const brush::StampInput& in, DirtyTiles& dirty) noexcept override {
        dirty.push_back(TileCoord{tileIndexFor(static_cast<i32>(in.pos.x)),
                                  tileIndexFor(static_cast<i32>(in.pos.y))});
    }
    void endStroke(DirtyTiles&) noexcept override { target_ = nullptr; }
    f32 spacingPx(f32 pressure) const noexcept override { return spacing_ * pressure; }

private:
    TileMap* target_ = nullptr;
    f32 spacing_ = 2.0f;
};

} // namespace

MARI_TEST(tile_coord_maps_to_canvas_rect) {
    const TileCoord c{2, 3};
    const Rect r = c.canvasRect();
    CHECK_EQ(r.x, 128);
    CHECK_EQ(r.y, 192); // y는 아래로 증가
    CHECK_EQ(r.width, kTileSize);
    CHECK_EQ(r.height, kTileSize);
    CHECK(r.contains(Point{128, 192}));
    CHECK(!r.contains(Point{192, 192}));
}

MARI_TEST(tile_coord_hash_distinguishes) {
    TileCoordHash h;
    CHECK_NE(h(TileCoord{1, 0}), h(TileCoord{0, 1}));
    CHECK_EQ(h(TileCoord{-1, 5}), h(TileCoord{-1, 5}));
}

MARI_TEST(tilemap_contract_is_implementable) {
    DummyTileMap map;
    CHECK_EQ(map.tileCount(), 0u);
    CHECK(map.at(TileCoord{0, 0}) == nullptr); // 없는 타일 = 완전 투명

    auto w = map.writable(TileCoord{1, 1});
    CHECK(w.ok());
    CHECK_EQ(map.tileCount(), 1u);
    CHECK_EQ(map.bounds(), (Rect{64, 64, 64, 64}));

    DirtyTiles dirty;
    map.collectTiles(Rect{0, 0, 200, 200}, dirty);
    CHECK_EQ(dirty.size(), 1u);
    map.collectTiles(Rect{1000, 1000, 10, 10}, dirty);
    CHECK_EQ(dirty.size(), 1u); // 겹치지 않으면 안 더한다

    auto snap = map.snapshot();
    CHECK_EQ(snap->tileCount(), 1u);

    map.erase(TileCoord{1, 1});
    CHECK_EQ(map.tileCount(), 0u);
    CHECK_EQ(snap->tileCount(), 1u); // 스냅샷은 살아 있다
}

MARI_TEST(engine_contract_is_implementable) {
    DummyEngine eng;
    DummyTileMap map;

    brush::MariBrushPreset preset;
    preset.tip.diameter = 20.0f;
    preset.spacing = 0.25f;
    preset.texture = brush::BrushTexture{};

    brush::ImportReport report;
    CHECK(eng.setPreset(preset, &report).ok());
    CHECK(report.hasDropped()); // 모르는 건 정직하게 남긴다
    CHECK_NEAR(eng.spacingPx(1.0f), 5.0f, 1e-4);

    brush::StrokeContext ctx(StrokeSource::humanPen());
    CHECK(!eng.beginStroke(ctx).ok()); // target 없으면 실패한다
    ctx.target = &map;
    ctx.layerId = 7;
    CHECK(eng.beginStroke(ctx).ok());

    DirtyTiles dirty;
    brush::StampInput in;
    in.pos = PointF{130.0f, 5.0f};
    eng.stamp(in, dirty);
    eng.endStroke(dirty);
    CHECK_EQ(dirty.size(), 1u);
    CHECK(dirty[0] == (TileCoord{2, 0}));
}

MARI_TEST(layer_enums_are_stable) {
    CHECK_EQ(static_cast<int>(LayerKind::Raster), 0);
    CHECK_EQ(kInvalidLayerId, 0u);
    CHECK_EQ(kInvalidBrushId, 0u);
}

MARI_TEST_MAIN()
