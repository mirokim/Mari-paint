// Mari Paint — 선택 마스크 구현. 선언은 include/mari/core/selection.hpp.
//
// 구현에서 지킨 세 가지:
//   1) **타일 0개가 두 극단을 모두 표현한다.** 모든 변경 끝에 `normalize()` 가
//      "값이 전부 outside 인 타일"을 버린다. 그래서 전체 선택도 빈 선택도 메모리 0이다.
//   2) **캔버스 밖은 선택되지 않는다.** `valueAt()` 이 캔버스 밖에서 0을 돌려주는 것이
//      반전·수축이 잘 정의되는 근거다.
//   3) **가장자리를 정직하게 만든다.** 타원·올가미는 x 방향 면적을 정확히 재고
//      y 방향만 4× 초과표본한다. "안티에일리어싱했다"고 말할 수 있는 수준으로 한다.
#include <mari/core/selection.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <climits>
#include <limits>
#include <utility>
#include <vector>

namespace mari {
namespace {

constexpr usize kTilePixels = static_cast<usize>(kTileSize) * static_cast<usize>(kTileSize);
constexpr i32 kSubSamples = 4; ///< y 방향 초과표본 수(x 는 해석적으로 정확히 잰다)

/// 두 선택 값을 결합한다. 0..255 를 "선택 정도"로 보고 곱·합으로 다룬다.
[[nodiscard]] u8 applyOp(u8 a, u8 b, SelectionOp op) noexcept {
    const i32 A = static_cast<i32>(a);
    const i32 B = static_cast<i32>(b);
    switch (op) {
    case SelectionOp::Replace:
        return b;
    case SelectionOp::Union:
        return static_cast<u8>(std::max(A, B));
    case SelectionOp::Subtract:
        return static_cast<u8>(A * (255 - B) / 255);
    case SelectionOp::Intersect:
        return static_cast<u8>(A * B / 255);
    case SelectionOp::Xor:
        return static_cast<u8>(std::abs(A - B));
    }
    return b;
}

[[nodiscard]] u8 toByte(f32 v) noexcept {
    return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

/// 한 스캔라인 구간 [sx, ex) 의 면적을 커버리지 누적기에 더한다.
/// x 방향은 **정확한 면적**이다(부분 픽셀을 그대로 더한다).
void accumulateSpan(f32* row, i32 w, i32 originX, f32 sx, f32 ex, f32 weight) noexcept {
    if (ex <= sx) {
        return;
    }
    const f32 lo = std::max(sx, static_cast<f32>(originX));
    const f32 hi = std::min(ex, static_cast<f32>(originX + w));
    if (hi <= lo) {
        return;
    }
    const auto first = static_cast<i32>(std::floor(lo)) - originX;
    const auto last = static_cast<i32>(std::ceil(hi)) - originX;
    for (i32 px = std::max(0, first); px < std::min(w, last); ++px) {
        const f32 pl = static_cast<f32>(originX + px);
        const f32 overlap = std::min(hi, pl + 1.0f) - std::max(lo, pl);
        if (overlap > 0.0f) {
            row[px] += overlap * weight;
        }
    }
}

/// Felzenszwalb 1D 제곱거리 변환. 정확한 유클리드 거리다(근사가 아니다).
void distance1d(const f32* f, f32* d, i32 n, i32* v, f32* z) noexcept {
    i32 k = 0;
    v[0] = 0;
    z[0] = -std::numeric_limits<f32>::max();
    z[1] = std::numeric_limits<f32>::max();
    for (i32 q = 1; q < n; ++q) {
        const f32 fq = f[q];
        const auto qf = static_cast<f32>(q);
        f32 s = 0.0f;
        while (true) {
            const auto vk = static_cast<f32>(v[k]);
            s = ((fq + qf * qf) - (f[v[k]] + vk * vk)) / (2.0f * qf - 2.0f * vk);
            if (s > z[k]) {
                break;
            }
            --k;
            if (k < 0) {
                k = 0;
                break;
            }
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = std::numeric_limits<f32>::max();
    }
    k = 0;
    for (i32 q = 0; q < n; ++q) {
        const auto qf = static_cast<f32>(q);
        while (z[k + 1] < qf) {
            ++k;
        }
        const f32 dx = qf - static_cast<f32>(v[k]);
        d[q] = dx * dx + f[v[k]];
    }
}

/// 2D 제곱거리 변환. `f` 는 씨앗이면 0, 아니면 큰 값. 결과는 제곱거리.
void distance2d(std::vector<f32>& f, i32 w, i32 h) {
    const i32 n = std::max(w, h);
    std::vector<f32> col(static_cast<usize>(n));
    std::vector<f32> out(static_cast<usize>(n));
    std::vector<i32> v(static_cast<usize>(n) + 1u);
    std::vector<f32> z(static_cast<usize>(n) + 2u);

    for (i32 x = 0; x < w; ++x) {
        for (i32 y = 0; y < h; ++y) {
            col[static_cast<usize>(y)] = f[static_cast<usize>(y) * static_cast<usize>(w) +
                                           static_cast<usize>(x)];
        }
        distance1d(col.data(), out.data(), h, v.data(), z.data());
        for (i32 y = 0; y < h; ++y) {
            f[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)] =
                out[static_cast<usize>(y)];
        }
    }
    for (i32 y = 0; y < h; ++y) {
        f32* row = f.data() + static_cast<usize>(y) * static_cast<usize>(w);
        distance1d(row, out.data(), w, v.data(), z.data());
        std::memcpy(row, out.data(), static_cast<usize>(w) * sizeof(f32));
    }
}

/// 가우시안 커널 하나. 합이 1 이다.
std::vector<f32> gaussianKernel(f32 sigma, i32 half) {
    std::vector<f32> k(static_cast<usize>(2 * half + 1));
    f32 sum = 0.0f;
    const f32 inv = 1.0f / (2.0f * sigma * sigma);
    for (i32 i = -half; i <= half; ++i) {
        const auto fi = static_cast<f32>(i);
        const f32 v = std::exp(-fi * fi * inv);
        k[static_cast<usize>(i + half)] = v;
        sum += v;
    }
    for (f32& v : k) {
        v /= sum;
    }
    return k;
}

} // namespace

// ── 이름 ────────────────────────────────────────────────────────────────

const char* selectionOpName(SelectionOp op) noexcept {
    switch (op) {
    case SelectionOp::Replace:
        return "replace";
    case SelectionOp::Union:
        return "add";
    case SelectionOp::Subtract:
        return "subtract";
    case SelectionOp::Intersect:
        return "intersect";
    case SelectionOp::Xor:
        return "xor";
    }
    return "replace";
}

Result<SelectionOp> selectionOpFromName(std::string_view s) noexcept {
    if (s == "replace" || s == "new" || s == "set") {
        return Ok(SelectionOp::Replace);
    }
    if (s == "add" || s == "union") {
        return Ok(SelectionOp::Union);
    }
    if (s == "subtract" || s == "sub" || s == "difference") {
        return Ok(SelectionOp::Subtract);
    }
    if (s == "intersect" || s == "and") {
        return Ok(SelectionOp::Intersect);
    }
    if (s == "xor") {
        return Ok(SelectionOp::Xor);
    }
    return Err(std::string("모르는 선택 결합 방식이다: \"") + std::string(s) +
                   "\" (replace|add|subtract|intersect|xor)",
               ErrorCode::InvalidArgument);
}

// ── 생성·복사 ────────────────────────────────────────────────────────────

SelectionMask::SelectionMask(const SelectionMask& o)
    : tiles_(o.tiles_ ? o.tiles_->snapshot() : nullptr), canvas_(o.canvas_), outside_(o.outside_),
      boundsCache_(o.boundsCache_), boundsValid_(o.boundsValid_) {}

SelectionMask& SelectionMask::operator=(const SelectionMask& o) {
    if (this != &o) {
        // 🔴 픽셀을 복사하지 않는다 — 타일 핸들만 복제하고 쓸 때 갈라선다(COW).
        tiles_ = o.tiles_ ? o.tiles_->snapshot() : nullptr;
        canvas_ = o.canvas_;
        outside_ = o.outside_;
        boundsCache_ = o.boundsCache_;
        boundsValid_ = o.boundsValid_;
    }
    return *this;
}

SelectionMask SelectionMask::all(Size canvas) noexcept {
    SelectionMask m;
    m.canvas_ = canvas;
    m.outside_ = 255;
    return m;
}

SelectionMask SelectionMask::empty(Size canvas) noexcept {
    SelectionMask m;
    m.canvas_ = canvas;
    m.outside_ = 0;
    return m;
}

void SelectionMask::setCanvasSize(Size canvas) {
    canvas_ = canvas;
    boundsValid_ = false;
    normalize();
}

// ── 읽기 ────────────────────────────────────────────────────────────────

usize SelectionMask::tileCount() const noexcept { return tiles_ ? tiles_->tileCount() : 0u; }

const u8* SelectionMask::tilePixels(TileCoord c) const noexcept {
    if (!tiles_) {
        return nullptr;
    }
    // 맵이 타일을 계속 들고 있으므로, 여기서 만든 shared_ptr 가 사라져도 픽셀은 산다.
    const ConstTilePtr t = tiles_->at(c);
    return t ? t->pixels() : nullptr;
}

u8 SelectionMask::valueAt(i32 x, i32 y) const noexcept {
    if (x < 0 || y < 0 || x >= canvas_.width || y >= canvas_.height) {
        return 0; // 🔴 캔버스 밖은 언제나 선택되지 않는다
    }
    const TileCoord c{tileIndexFor(x), tileIndexFor(y)};
    const u8* p = tilePixels(c);
    if (p == nullptr) {
        return outside_;
    }
    const auto lx = static_cast<usize>(x - tileOrigin(c.tx));
    const auto ly = static_cast<usize>(y - tileOrigin(c.ty));
    return p[ly * static_cast<usize>(kTileSize) + lx];
}

u8 SelectionMask::clampedValueAt(i32 x, i32 y) const noexcept {
    if (canvas_.isEmpty()) {
        return 0;
    }
    return valueAt(std::clamp(x, 0, canvas_.width - 1), std::clamp(y, 0, canvas_.height - 1));
}

void SelectionMask::collectTiles(DirtyTiles& out) const {
    if (!tiles_) {
        return;
    }
    tiles_->collectTiles(tiles_->bounds(), out);
}

Rect SelectionMask::bounds() const {
    if (boundsValid_) {
        return boundsCache_;
    }
    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    Rect acc{};
    if (canvas.isEmpty()) {
        boundsCache_ = acc;
        boundsValid_ = true;
        return acc;
    }
    const i32 tx1 = tileIndexFor(canvas.width - 1);
    const i32 ty1 = tileIndexFor(canvas.height - 1);
    for (i32 ty = 0; ty <= ty1; ++ty) {
        for (i32 tx = 0; tx <= tx1; ++tx) {
            const TileCoord c{tx, ty};
            const Rect tr = c.canvasRect().intersected(canvas);
            if (tr.isEmpty()) {
                continue;
            }
            const u8* p = tilePixels(c);
            if (p == nullptr) {
                if (outside_ != 0) {
                    acc = acc.united(tr);
                }
                continue;
            }
            // 할당된 타일은 픽셀 단위로 정확히 잰다.
            i32 minx = tr.right();
            i32 maxx = tr.x - 1;
            i32 miny = tr.bottom();
            i32 maxy = tr.y - 1;
            for (i32 y = tr.y; y < tr.bottom(); ++y) {
                const u8* row = p + static_cast<usize>(y - tileOrigin(ty)) *
                                        static_cast<usize>(kTileSize);
                for (i32 x = tr.x; x < tr.right(); ++x) {
                    if (row[static_cast<usize>(x - tileOrigin(tx))] == 0) {
                        continue;
                    }
                    minx = std::min(minx, x);
                    maxx = std::max(maxx, x);
                    miny = std::min(miny, y);
                    maxy = std::max(maxy, y);
                }
            }
            if (maxx >= minx && maxy >= miny) {
                acc = acc.united(Rect::fromBounds(minx, miny, maxx + 1, maxy + 1));
            }
        }
    }
    boundsCache_ = acc;
    boundsValid_ = true;
    return acc;
}

u64 SelectionMask::selectedPixels() const {
    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    if (canvas.isEmpty()) {
        return 0;
    }
    u64 sum = 0;
    const i32 tx1 = tileIndexFor(canvas.width - 1);
    const i32 ty1 = tileIndexFor(canvas.height - 1);
    for (i32 ty = 0; ty <= ty1; ++ty) {
        for (i32 tx = 0; tx <= tx1; ++tx) {
            const TileCoord c{tx, ty};
            const Rect tr = c.canvasRect().intersected(canvas);
            if (tr.isEmpty()) {
                continue;
            }
            const u8* p = tilePixels(c);
            if (p == nullptr) {
                sum += static_cast<u64>(outside_) * static_cast<u64>(tr.width) *
                       static_cast<u64>(tr.height);
                continue;
            }
            for (i32 y = tr.y; y < tr.bottom(); ++y) {
                const u8* row = p + static_cast<usize>(y - tileOrigin(ty)) *
                                        static_cast<usize>(kTileSize);
                for (i32 x = tr.x; x < tr.right(); ++x) {
                    sum += row[static_cast<usize>(x - tileOrigin(tx))];
                }
            }
        }
    }
    return sum / 255u;
}

// ── 쓰기 기반 ────────────────────────────────────────────────────────────

Result<u8*> SelectionMask::writableTile(TileCoord c) {
    if (!tiles_) {
        Result<TileMapPtr> m = makeTileMap(PixelFormat::Gray8);
        if (!m.ok()) {
            return m.error();
        }
        tiles_ = m.value();
    }
    const bool existed = tiles_->at(c) != nullptr;
    Result<TilePtr> t = tiles_->writable(c);
    if (!t.ok()) {
        return t.error();
    }
    Tile* tile = t.value().get();
    if (tile == nullptr) {
        return Err("선택 타일을 만들지 못했다", ErrorCode::OutOfMemory);
    }
    u8* p = tile->mutablePixels();
    if (p == nullptr) {
        return Err("선택 타일이 쓰기를 거절했다", ErrorCode::OutOfMemory);
    }
    if (!existed && outside_ != 0) {
        // 🔴 새 타일은 0 으로 태어난다. 이 마스크에서 "없음"은 outside_ 라서 그 값으로 채운다.
        std::memset(p, outside_, kTilePixels);
    }
    boundsValid_ = false;
    return Ok(p);
}

Result<void> SelectionMask::fillRect(const Rect& area, u8 v) {
    const Rect r = area.intersected(Rect{0, 0, canvas_.width, canvas_.height});
    if (r.isEmpty()) {
        return Ok();
    }
    for (i32 ty = tileIndexFor(r.y); ty <= tileIndexFor(r.bottom() - 1); ++ty) {
        for (i32 tx = tileIndexFor(r.x); tx <= tileIndexFor(r.right() - 1); ++tx) {
            const TileCoord c{tx, ty};
            const Rect tr = c.canvasRect().intersected(r);
            if (tr.isEmpty()) {
                continue;
            }
            Result<u8*> p = writableTile(c);
            if (!p.ok()) {
                return p.error();
            }
            for (i32 y = tr.y; y < tr.bottom(); ++y) {
                u8* row = p.value() + static_cast<usize>(y - tileOrigin(ty)) *
                                          static_cast<usize>(kTileSize);
                std::memset(row + static_cast<usize>(tr.x - tileOrigin(tx)), v,
                            static_cast<usize>(tr.width));
            }
        }
    }
    return Ok();
}

Result<void> SelectionMask::writeRegion(const Rect& area, const u8* src, usize srcStride) {
    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    const Rect r = area.intersected(canvas);
    if (r.isEmpty() || src == nullptr) {
        return Ok();
    }
    for (i32 ty = tileIndexFor(r.y); ty <= tileIndexFor(r.bottom() - 1); ++ty) {
        for (i32 tx = tileIndexFor(r.x); tx <= tileIndexFor(r.right() - 1); ++tx) {
            const TileCoord c{tx, ty};
            const Rect tr = c.canvasRect().intersected(r);
            if (tr.isEmpty()) {
                continue;
            }
            Result<u8*> p = writableTile(c);
            if (!p.ok()) {
                return p.error();
            }
            for (i32 y = tr.y; y < tr.bottom(); ++y) {
                u8* dst = p.value() + static_cast<usize>(y - tileOrigin(ty)) *
                                          static_cast<usize>(kTileSize) +
                          static_cast<usize>(tr.x - tileOrigin(tx));
                const u8* s = src + static_cast<usize>(y - area.y) * srcStride +
                              static_cast<usize>(tr.x - area.x);
                std::memcpy(dst, s, static_cast<usize>(tr.width));
            }
        }
    }
    return Ok();
}

void SelectionMask::normalize() {
    if (!tiles_) {
        boundsValid_ = false;
        return;
    }
    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    DirtyTiles list;
    collectTiles(list);
    for (const TileCoord& c : list) {
        const u8* p = tilePixels(c);
        if (p == nullptr) {
            continue;
        }
        const Rect tr = c.canvasRect().intersected(canvas);
        if (tr.isEmpty()) {
            tiles_->erase(c); // 캔버스 밖 타일은 보이지도 않는다
            continue;
        }
        bool uniform = true;
        for (i32 y = tr.y; uniform && y < tr.bottom(); ++y) {
            const u8* row =
                p + static_cast<usize>(y - tileOrigin(c.ty)) * static_cast<usize>(kTileSize);
            for (i32 x = tr.x; x < tr.right(); ++x) {
                if (row[static_cast<usize>(x - tileOrigin(c.tx))] != outside_) {
                    uniform = false;
                    break;
                }
            }
        }
        if (uniform) {
            tiles_->erase(c); // 🔴 여기서 "전체 선택 = 타일 0개" 가 성립한다
        }
    }
    boundsValid_ = false;
}

// ── 변형 ────────────────────────────────────────────────────────────────

void SelectionMask::invert() {
    outside_ = static_cast<u8>(255 - outside_);
    DirtyTiles list;
    collectTiles(list);
    for (const TileCoord& c : list) {
        Result<u8*> p = writableTile(c);
        if (!p.ok()) {
            continue; // 타일 하나를 못 갈라도 나머지는 뒤집는다
        }
        u8* px = p.value();
        for (usize i = 0; i < kTilePixels; ++i) {
            px[i] = static_cast<u8>(255 - px[i]);
        }
    }
    normalize();
}

Result<void> SelectionMask::combine(const SelectionMask& other, SelectionOp op) {
    if (other.canvas_.width != canvas_.width || other.canvas_.height != canvas_.height) {
        return Err("캔버스 크기가 다른 선택은 결합할 수 없다", ErrorCode::InvalidArgument);
    }
    const u8 oldOut = outside_;
    const u8 otherOut = other.outside_;

    DirtyTiles coords;
    collectTiles(coords);
    DirtyTiles ob;
    other.collectTiles(ob);
    for (const TileCoord& c : ob) {
        if (std::find(coords.begin(), coords.end(), c) == coords.end()) {
            coords.push_back(c);
        }
    }

    outside_ = applyOp(oldOut, otherOut, op);

    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    std::vector<u8> buf(kTilePixels);
    for (const TileCoord& c : coords) {
        const u8* ap = tilePixels(c);
        const u8* bp = other.tilePixels(c);
        for (usize i = 0; i < kTilePixels; ++i) {
            buf[i] = applyOp(ap != nullptr ? ap[i] : oldOut, bp != nullptr ? bp[i] : otherOut, op);
        }
        const Rect tr = c.canvasRect().intersected(canvas);
        bool uniform = true;
        for (i32 y = tr.y; uniform && y < tr.bottom(); ++y) {
            for (i32 x = tr.x; x < tr.right(); ++x) {
                const usize idx = static_cast<usize>(y - tileOrigin(c.ty)) *
                                      static_cast<usize>(kTileSize) +
                                  static_cast<usize>(x - tileOrigin(c.tx));
                if (buf[idx] != outside_) {
                    uniform = false;
                    break;
                }
            }
        }
        if (uniform || tr.isEmpty()) {
            if (tiles_) {
                tiles_->erase(c);
            }
            continue;
        }
        Result<u8*> p = writableTile(c);
        if (!p.ok()) {
            return p.error();
        }
        std::memcpy(p.value(), buf.data(), kTilePixels);
    }
    normalize();
    return Ok();
}

Result<void> SelectionMask::expand(i32 by) {
    if (by == 0 || canvas_.isEmpty()) {
        return Ok();
    }
    if (by > 0 && (isAll() || isEmpty())) {
        return Ok(); // 이미 전부이거나, 키울 씨앗이 없다
    }
    if (by < 0 && isEmpty()) {
        return Ok();
    }
    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    const Rect seed = bounds();
    const i32 grow = std::abs(by);
    const Rect work =
        (by > 0 ? Rect{seed.x - grow, seed.y - grow, seed.width + 2 * grow, seed.height + 2 * grow}
                : seed)
            .intersected(canvas);
    if (work.isEmpty()) {
        return Ok();
    }
    const i32 pad = grow + 1;
    const Rect field{work.x - pad, work.y - pad, work.width + 2 * pad, work.height + 2 * pad};

    const auto fw = field.width;
    const auto fh = field.height;
    const f32 kFar = 1.0e20f;
    std::vector<f32> dist(static_cast<usize>(fw) * static_cast<usize>(fh));
    for (i32 y = 0; y < fh; ++y) {
        for (i32 x = 0; x < fw; ++x) {
            // 🔴 팽창은 "선택된 곳까지의 거리", 수축은 "선택 안 된 곳까지의 거리"다.
            const bool on = valueAt(field.x + x, field.y + y) >= 128;
            const bool seedPx = by > 0 ? on : !on;
            dist[static_cast<usize>(y) * static_cast<usize>(fw) + static_cast<usize>(x)] =
                seedPx ? 0.0f : kFar;
        }
    }
    distance2d(dist, fw, fh);

    const f32 limit = static_cast<f32>(grow) * static_cast<f32>(grow);
    std::vector<u8> out(static_cast<usize>(work.width) * static_cast<usize>(work.height));
    for (i32 y = 0; y < work.height; ++y) {
        for (i32 x = 0; x < work.width; ++x) {
            const f32 d2 = dist[static_cast<usize>(y + pad) * static_cast<usize>(fw) +
                                static_cast<usize>(x + pad)];
            const bool sel = by > 0 ? (d2 <= limit) : (d2 > limit);
            out[static_cast<usize>(y) * static_cast<usize>(work.width) + static_cast<usize>(x)] =
                sel ? u8{255} : u8{0};
        }
    }
    const Result<void> w = writeRegion(work, out.data(), static_cast<usize>(work.width));
    if (!w.ok()) {
        return w;
    }
    normalize();
    return Ok();
}

Result<void> SelectionMask::feather(f32 radius) {
    if (!(radius > 0.0f) || canvas_.isEmpty()) {
        return Ok();
    }
    // 🔴 전체 선택은 페더해도 그대로다 — 캔버스 밖을 가장자리 값으로 연장해서 샘플하므로
    //    테두리가 깎이지 않는다. 빈 선택도 부드럽게 할 경계가 없다.
    if (isAll() || isEmpty()) {
        return Ok();
    }
    const f32 sigma = std::max(radius * 0.5f, 0.05f);
    const auto half = std::max(1, static_cast<i32>(std::ceil(3.0f * sigma)));
    const Rect canvas{0, 0, canvas_.width, canvas_.height};
    const Rect seed = bounds();
    const Rect work = Rect{seed.x - half, seed.y - half, seed.width + 2 * half,
                           seed.height + 2 * half}
                          .intersected(canvas);
    if (work.isEmpty()) {
        return Ok();
    }
    const i32 sw = work.width + 2 * half;
    const i32 sh = work.height + 2 * half;
    std::vector<f32> src(static_cast<usize>(sw) * static_cast<usize>(sh));
    for (i32 y = 0; y < sh; ++y) {
        for (i32 x = 0; x < sw; ++x) {
            src[static_cast<usize>(y) * static_cast<usize>(sw) + static_cast<usize>(x)] =
                static_cast<f32>(clampedValueAt(work.x - half + x, work.y - half + y)) *
                (1.0f / 255.0f);
        }
    }
    const std::vector<f32> k = gaussianKernel(sigma, half);

    // 가로 → 세로 (분리형). 가로 패스는 유효 폭 work.width 만 남긴다.
    std::vector<f32> mid(static_cast<usize>(work.width) * static_cast<usize>(sh));
    for (i32 y = 0; y < sh; ++y) {
        const f32* row = src.data() + static_cast<usize>(y) * static_cast<usize>(sw);
        f32* dst = mid.data() + static_cast<usize>(y) * static_cast<usize>(work.width);
        for (i32 x = 0; x < work.width; ++x) {
            f32 acc = 0.0f;
            for (i32 i = -half; i <= half; ++i) {
                acc += row[static_cast<usize>(x + half + i)] * k[static_cast<usize>(i + half)];
            }
            dst[x] = acc;
        }
    }
    std::vector<u8> out(static_cast<usize>(work.width) * static_cast<usize>(work.height));
    for (i32 y = 0; y < work.height; ++y) {
        for (i32 x = 0; x < work.width; ++x) {
            f32 acc = 0.0f;
            for (i32 i = -half; i <= half; ++i) {
                acc += mid[static_cast<usize>(y + half + i) * static_cast<usize>(work.width) +
                           static_cast<usize>(x)] *
                       k[static_cast<usize>(i + half)];
            }
            out[static_cast<usize>(y) * static_cast<usize>(work.width) + static_cast<usize>(x)] =
                toByte(acc);
        }
    }
    const Result<void> w = writeRegion(work, out.data(), static_cast<usize>(work.width));
    if (!w.ok()) {
        return w;
    }
    normalize();
    return Ok();
}

// ── 만들기 ──────────────────────────────────────────────────────────────

Result<SelectionMask> SelectionMask::fromRect(Size canvas, const Rect& r) {
    SelectionMask m = empty(canvas);
    const Rect c{0, 0, canvas.width, canvas.height};
    const Rect clipped = r.intersected(c);
    if (clipped.isEmpty()) {
        return Ok(std::move(m));
    }
    if (clipped == c) {
        return Ok(all(canvas)); // 캔버스 전체 = 타일 0개
    }
    const Result<void> f = m.fillRect(clipped, 255);
    if (!f.ok()) {
        return f.error();
    }
    m.normalize();
    return Ok(std::move(m));
}

Result<SelectionMask> SelectionMask::fromEllipse(Size canvas, const Rect& box, bool antialias) {
    SelectionMask m = empty(canvas);
    const Rect c{0, 0, canvas.width, canvas.height};
    const Rect work = box.intersected(c);
    if (work.isEmpty() || box.isEmpty()) {
        return Ok(std::move(m));
    }
    const f32 cx = static_cast<f32>(box.x) + static_cast<f32>(box.width) * 0.5f;
    const f32 cy = static_cast<f32>(box.y) + static_cast<f32>(box.height) * 0.5f;
    const f32 a = static_cast<f32>(box.width) * 0.5f;
    const f32 b = static_cast<f32>(box.height) * 0.5f;

    const i32 subs = antialias ? kSubSamples : 1;
    const f32 weight = 1.0f / static_cast<f32>(subs);
    std::vector<f32> cov(static_cast<usize>(work.width));
    std::vector<u8> out(static_cast<usize>(work.width) * static_cast<usize>(work.height));
    for (i32 y = 0; y < work.height; ++y) {
        std::fill(cov.begin(), cov.end(), 0.0f);
        for (i32 s = 0; s < subs; ++s) {
            const f32 yc = static_cast<f32>(work.y + y) +
                           (static_cast<f32>(s) + 0.5f) / static_cast<f32>(subs);
            const f32 t = (yc - cy) / b;
            if (t <= -1.0f || t >= 1.0f) {
                continue;
            }
            const f32 dx = a * std::sqrt(1.0f - t * t);
            accumulateSpan(cov.data(), work.width, work.x, cx - dx, cx + dx, weight);
        }
        u8* row = out.data() + static_cast<usize>(y) * static_cast<usize>(work.width);
        for (i32 x = 0; x < work.width; ++x) {
            row[x] = antialias ? toByte(cov[static_cast<usize>(x)])
                               : (cov[static_cast<usize>(x)] >= 0.5f ? u8{255} : u8{0});
        }
    }
    const Result<void> w = m.writeRegion(work, out.data(), static_cast<usize>(work.width));
    if (!w.ok()) {
        return w.error();
    }
    m.normalize();
    return Ok(std::move(m));
}

Result<SelectionMask> SelectionMask::fromPolygon(Size canvas, const std::vector<PointF>& points,
                                                 bool antialias) {
    if (points.size() < 3) {
        return Err("올가미는 점이 3개 이상이어야 한다", ErrorCode::InvalidArgument);
    }
    SelectionMask m = empty(canvas);
    const Rect c{0, 0, canvas.width, canvas.height};
    f32 minx = points[0].x;
    f32 maxx = points[0].x;
    f32 miny = points[0].y;
    f32 maxy = points[0].y;
    for (const PointF& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            return Err("올가미 좌표가 유한하지 않다", ErrorCode::InvalidArgument);
        }
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    const Rect box = Rect::fromBounds(static_cast<i32>(std::floor(minx)),
                                      static_cast<i32>(std::floor(miny)),
                                      static_cast<i32>(std::ceil(maxx)) + 1,
                                      static_cast<i32>(std::ceil(maxy)) + 1);
    const Rect work = box.intersected(c);
    if (work.isEmpty()) {
        return Ok(std::move(m));
    }

    const i32 subs = antialias ? kSubSamples : 1;
    const f32 weight = 1.0f / static_cast<f32>(subs);
    std::vector<f32> cov(static_cast<usize>(work.width));
    std::vector<f32> xs;
    std::vector<u8> out(static_cast<usize>(work.width) * static_cast<usize>(work.height));
    const usize n = points.size();
    for (i32 y = 0; y < work.height; ++y) {
        std::fill(cov.begin(), cov.end(), 0.0f);
        for (i32 s = 0; s < subs; ++s) {
            const f32 yc = static_cast<f32>(work.y + y) +
                           (static_cast<f32>(s) + 0.5f) / static_cast<f32>(subs);
            xs.clear();
            for (usize i = 0; i < n; ++i) {
                const PointF& p0 = points[i];
                const PointF& p1 = points[(i + 1) % n];
                // 반열림 판정 — 꼭짓점에서 두 번 세는 것을 막는다(짝수-홀수 규칙).
                if ((p0.y <= yc && p1.y > yc) || (p1.y <= yc && p0.y > yc)) {
                    const f32 t = (yc - p0.y) / (p1.y - p0.y);
                    xs.push_back(p0.x + t * (p1.x - p0.x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (usize i = 0; i + 1 < xs.size(); i += 2) {
                accumulateSpan(cov.data(), work.width, work.x, xs[i], xs[i + 1], weight);
            }
        }
        u8* row = out.data() + static_cast<usize>(y) * static_cast<usize>(work.width);
        for (i32 x = 0; x < work.width; ++x) {
            row[x] = antialias ? toByte(cov[static_cast<usize>(x)])
                               : (cov[static_cast<usize>(x)] >= 0.5f ? u8{255} : u8{0});
        }
    }
    const Result<void> w = m.writeRegion(work, out.data(), static_cast<usize>(work.width));
    if (!w.ok()) {
        return w.error();
    }
    m.normalize();
    return Ok(std::move(m));
}

Result<SelectionMask> SelectionMask::fromTiles(Size canvas, const TileMap& src, u8 outsideValue,
                                               const std::function<u8(const u8*)>& judge) {
    SelectionMask m = outsideValue != 0 ? SelectionMask::all(canvas) : SelectionMask::empty(canvas);
    if (src.format() != PixelFormat::RGBA8) {
        return Err("선택은 RGBA8 레이어에서만 뽑는다", ErrorCode::Unsupported);
    }
    DirtyTiles list;
    src.collectTiles(src.bounds(), list);
    const Rect c{0, 0, canvas.width, canvas.height};
    std::vector<u8> buf(kTilePixels);
    for (const TileCoord& tc : list) {
        if (tc.canvasRect().intersected(c).isEmpty()) {
            continue;
        }
        const ConstTilePtr t = src.at(tc);
        if (!t) {
            continue;
        }
        const u8* p = t->pixels();
        const usize stride = t->stride();
        for (i32 y = 0; y < kTileSize; ++y) {
            for (i32 x = 0; x < kTileSize; ++x) {
                buf[static_cast<usize>(y) * static_cast<usize>(kTileSize) +
                    static_cast<usize>(x)] =
                    judge(p + static_cast<usize>(y) * stride + static_cast<usize>(x) * 4u);
            }
        }
        const Result<void> w =
            m.writeRegion(tc.canvasRect(), buf.data(), static_cast<usize>(kTileSize));
        if (!w.ok()) {
            return w.error();
        }
    }
    m.normalize();
    return Ok(std::move(m));
}

Result<SelectionMask> SelectionMask::fromColorRange(Size canvas, const TileMap& src, Color8 ref,
                                                    i32 tolerance) {
    if (tolerance < 0 || tolerance > 255) {
        return Err("tolerance 는 0..255 다", ErrorCode::InvalidArgument);
    }
    const auto matches = [ref, tolerance](const u8* px) noexcept -> u8 {
        const i32 dr = std::abs(static_cast<i32>(px[0]) - static_cast<i32>(ref.r));
        const i32 dg = std::abs(static_cast<i32>(px[1]) - static_cast<i32>(ref.g));
        const i32 db = std::abs(static_cast<i32>(px[2]) - static_cast<i32>(ref.b));
        const i32 da = std::abs(static_cast<i32>(px[3]) - static_cast<i32>(ref.a));
        const i32 d = std::max(std::max(dr, dg), std::max(db, da));
        return d <= tolerance ? u8{255} : u8{0};
    };
    // 🔴 할당되지 않은 타일은 완전 투명이다. 기준색이 거기에 걸리면 **타일 없이도** 선택된다.
    const u8 transparent[4] = {0, 0, 0, 0};
    return fromTiles(canvas, src, matches(transparent), matches);
}

Result<SelectionMask> SelectionMask::fromContent(Size canvas, const TileMap& src, u8 threshold) {
    const u8 th = threshold == 0 ? u8{1} : threshold;
    return fromTiles(canvas, src, 0,
                         [th](const u8* px) noexcept -> u8 { return px[3] >= th ? u8{255} : u8{0}; });
}

Result<SelectionMask> SelectionMask::fromLayerAlpha(Size canvas, const TileMap& src) {
    return fromTiles(canvas, src, 0, [](const u8* px) noexcept -> u8 { return px[3]; });
}

Result<SelectionMask> SelectionMask::fromFlood(Size canvas, const TileMap& src, i32 seedX, i32 seedY,
                                               i32 tolerance, i32 gapClose) {
    if (tolerance < 0 || tolerance > 255) {
        return Err("tolerance 는 0..255 다", ErrorCode::InvalidArgument);
    }
    if (canvas.width <= 0 || canvas.height <= 0) {
        return Err("캔버스가 비었다", ErrorCode::InvalidArgument);
    }
    SelectionMask m = SelectionMask::empty(canvas);
    if (seedX < 0 || seedY < 0 || seedX >= canvas.width || seedY >= canvas.height) {
        return Ok(std::move(m));
    }
    const usize W = static_cast<usize>(canvas.width);
    const usize H = static_cast<usize>(canvas.height);

    // 픽셀 읽기: 타일 하나를 캐시한다(스캔라인이 한 타일 안에서 오래 머문다).
    struct Reader {
        const TileMap& tm;
        TileCoord cached{INT32_MIN, INT32_MIN};
        ConstTilePtr tile;
        const u8* px(i32 x, i32 y) noexcept {
            static const u8 transparent[4] = {0, 0, 0, 0};
            const TileCoord c{tileIndexFor(x), tileIndexFor(y)};
            if (!(c == cached)) {
                cached = c;
                tile = tm.at(c);
            }
            if (!tile) return transparent;
            return tile->pixels() + static_cast<usize>(y - tileOrigin(c.ty)) * tile->stride() +
                   static_cast<usize>(x - tileOrigin(c.tx)) * 4u;
        }
    } rd{src};

    const u8* seedPx = rd.px(seedX, seedY);
    const u8 ref[4] = {seedPx[0], seedPx[1], seedPx[2], seedPx[3]};
    const auto matches = [&](i32 x, i32 y) noexcept -> bool {
        const u8* p = rd.px(x, y);
        const i32 d = std::max(std::max(std::abs(p[0] - ref[0]), std::abs(p[1] - ref[1])),
                               std::max(std::abs(p[2] - ref[2]), std::abs(p[3] - ref[3])));
        return d <= tolerance;
    };

    // 🔴 틈 닫기: 경계(불일치) 픽셀에서 gapClose 이내인 픽셀도 경계로 본다. 미리 "두꺼운 경계" 비트맵을
    //    만들어 두면 플러드는 그것만 본다. 8K 캔버스면 8MB 비트 — 비트맵으로 든다.
    std::vector<u8> blocked; // 1바이트 = 8픽셀 아님, 단순화를 위해 1바이트/픽셀 (8K 에서 64MB. 필요하면 비트로)
    if (gapClose > 0) {
        blocked.assign(W * H, 0);
        std::vector<u8> edge(W * H, 0);
        for (i32 y = 0; y < canvas.height; ++y)
            for (i32 x = 0; x < canvas.width; ++x)
                edge[static_cast<usize>(y) * W + static_cast<usize>(x)] = matches(x, y) ? 0 : 1;
        // 분리 가능한 정사각 팽창(체비쇼프 반지름 gapClose) — 가로 후 세로.
        std::vector<u8> tmp(W * H, 0);
        for (i32 y = 0; y < canvas.height; ++y) {
            i32 run = 0;
            for (i32 x = 0; x < canvas.width; ++x) {
                const usize i = static_cast<usize>(y) * W + static_cast<usize>(x);
                if (edge[i]) run = gapClose * 2 + 1;
                if (run > 0) { tmp[i] = 1; --run; }
            }
            run = 0;
            for (i32 x = canvas.width - 1; x >= 0; --x) {
                const usize i = static_cast<usize>(y) * W + static_cast<usize>(x);
                if (edge[i]) run = gapClose * 2 + 1;
                if (run > 0) { tmp[i] = 1; --run; }
            }
        }
        for (i32 x = 0; x < canvas.width; ++x) {
            i32 run = 0;
            for (i32 y = 0; y < canvas.height; ++y) {
                const usize i = static_cast<usize>(y) * W + static_cast<usize>(x);
                if (tmp[i]) run = gapClose * 2 + 1;
                if (run > 0) { blocked[i] = 1; --run; }
            }
            run = 0;
            for (i32 y = canvas.height - 1; y >= 0; --y) {
                const usize i = static_cast<usize>(y) * W + static_cast<usize>(x);
                if (tmp[i]) run = gapClose * 2 + 1;
                if (run > 0) { blocked[i] = 1; --run; }
            }
        }
        // 시드 자체가 두꺼운 경계에 묻혔으면 시드만은 허용한다(아니면 아무것도 안 채워진다).
        blocked[static_cast<usize>(seedY) * W + static_cast<usize>(seedX)] = 0;
    }
    const auto open = [&](i32 x, i32 y) noexcept -> bool {
        if (!blocked.empty()) return !blocked[static_cast<usize>(y) * W + static_cast<usize>(x)];
        return matches(x, y);
    };

    // 스캔라인 플러드. visited 는 비트맵.
    std::vector<u8> visited((W * H + 7) / 8, 0);
    const auto seen = [&](i32 x, i32 y) noexcept -> bool {
        const usize i = static_cast<usize>(y) * W + static_cast<usize>(x);
        return (visited[i >> 3] >> (i & 7)) & 1u;
    };
    const auto mark = [&](i32 x, i32 y) noexcept {
        const usize i = static_cast<usize>(y) * W + static_cast<usize>(x);
        visited[i >> 3] = static_cast<u8>(visited[i >> 3] | (1u << (i & 7)));
    };
    std::vector<std::pair<i32, i32>> stack;
    stack.emplace_back(seedX, seedY);
    std::vector<u8> tileBuf(static_cast<usize>(kTileSize) * kTileSize);
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        if (seen(x, y) || !open(x, y)) continue;
        i32 x0 = x, x1 = x;
        while (x0 > 0 && !seen(x0 - 1, y) && open(x0 - 1, y)) --x0;
        while (x1 + 1 < canvas.width && !seen(x1 + 1, y) && open(x1 + 1, y)) ++x1;
        for (i32 i = x0; i <= x1; ++i) mark(i, y);
        for (const i32 ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= canvas.height) continue;
            bool inRun = false;
            for (i32 i = x0; i <= x1; ++i) {
                const bool o = !seen(i, ny) && open(i, ny);
                if (o && !inRun) { stack.emplace_back(i, ny); inRun = true; }
                else if (!o) inRun = false;
            }
        }
    }

    // 비트맵 → 타일. 비트가 하나라도 있는 타일만 쓴다.
    const i32 tx1 = tileIndexFor(canvas.width - 1);
    const i32 ty1 = tileIndexFor(canvas.height - 1);
    for (i32 ty = 0; ty <= ty1; ++ty) {
        for (i32 tx = 0; tx <= tx1; ++tx) {
            bool any = false;
            const i32 ox = tileOrigin(tx), oy = tileOrigin(ty);
            for (i32 y = 0; y < kTileSize; ++y) {
                for (i32 x = 0; x < kTileSize; ++x) {
                    const i32 cx = ox + x, cy = oy + y;
                    const bool on = cx < canvas.width && cy < canvas.height && seen(cx, cy);
                    tileBuf[static_cast<usize>(y) * kTileSize + static_cast<usize>(x)] = on ? 255 : 0;
                    any = any || on;
                }
            }
            if (!any) continue;
            const Rect tr = Rect{ox, oy, kTileSize, kTileSize}.intersected(Rect{0, 0, canvas.width, canvas.height});
            const Result<void> w = m.writeRegion(tr, tileBuf.data(), static_cast<usize>(kTileSize));
            if (!w.ok()) return w.error();
        }
    }
    m.normalize();
    return Ok(std::move(m));
}

} // namespace mari
