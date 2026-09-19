// Mari Paint — 이미지 연산 구현 (include/mari/app/image_ops.hpp)
#include <mari/app/image_ops.hpp>

#include <mari/app/live_stroke.hpp>
#include <mari/app/stroke_entry.hpp>
#include <mari/core/undo.hpp>
#include <mari/ora/image.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace mari::app {

namespace {

constexpr f32 kInv255 = 1.0f / 255.0f;

[[nodiscard]] u8 toByte(f32 v) noexcept {
    return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 255.0f)));
}

// ── 색 공간 ───────────────────────────────────────────────────────────────

void rgbToHsl(f32 r, f32 g, f32 b, f32& h, f32& s, f32& l) noexcept {
    const f32 mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5f;
    const f32 d = mx - mn;
    if (d < 1e-6f) {
        h = 0.0f;
        s = 0.0f;
        return;
    }
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    h *= 60.0f;
}

f32 hueToRgb(f32 p, f32 q, f32 t) noexcept {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void hslToRgb(f32 h, f32 s, f32 l, f32& r, f32& g, f32& b) noexcept {
    if (s <= 0.0f) {
        r = g = b = l;
        return;
    }
    const f32 q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const f32 p = 2.0f * l - q;
    const f32 hh = h / 360.0f;
    r = hueToRgb(p, q, hh + 1.0f / 3.0f);
    g = hueToRgb(p, q, hh);
    b = hueToRgb(p, q, hh - 1.0f / 3.0f);
}

/// 곡선을 256칸 LUT 로. 점 0개면 항등. 구간 선형(단조 가정 없음, x 정렬).
void curveToLut(std::vector<CurvePt> pts, u8 (&lut)[256]) {
    if (pts.empty()) {
        for (int i = 0; i < 256; ++i) lut[i] = static_cast<u8>(i);
        return;
    }
    std::sort(pts.begin(), pts.end(), [](const CurvePt& a, const CurvePt& b) { return a.x < b.x; });
    for (int i = 0; i < 256; ++i) {
        const f32 x = static_cast<f32>(i) / 255.0f;
        f32 y;
        if (x <= pts.front().x) y = pts.front().y;
        else if (x >= pts.back().x) y = pts.back().y;
        else {
            y = pts.back().y;
            for (usize k = 1; k < pts.size(); ++k) {
                if (x <= pts[k].x) {
                    const f32 span = pts[k].x - pts[k - 1].x;
                    const f32 t = span > 1e-6f ? (x - pts[k - 1].x) / span : 1.0f;
                    y = pts[k - 1].y + (pts[k].y - pts[k - 1].y) * t;
                    break;
                }
            }
        }
        lut[i] = toByte(y * 255.0f);
    }
}

/// 선택 마스크로 원본과 섞는다(알파는 원본 그대로 — 색 보정은 알파를 안 바꾼다).
void blendBySelection(const SelectionMask& sel, const Rect& area, const ora::Image8& orig, ora::Image8& img) {
    if (sel.isAll()) return;
    for (i32 y = 0; y < area.height; ++y) {
        u8* d = img.pixels.data() + static_cast<usize>(y) * img.stride();
        const u8* o = orig.pixels.data() + static_cast<usize>(y) * orig.stride();
        for (i32 x = 0; x < area.width; ++x, d += 4, o += 4) {
            const u32 m = sel.valueAt(area.x + x, area.y + y);
            if (m == 255) continue;
            if (m == 0) {
                std::copy(o, o + 4, d);
                continue;
            }
            for (int c = 0; c < 4; ++c) d[c] = static_cast<u8>((d[c] * m + o[c] * (255u - m) + 127u) / 255u);
        }
    }
}

/// 연산 뒤처리: 기록하고, 저널이 고장 났으면 롤백한다(docs/06 결정 ④).
Result<void> finish(Document& doc, const StrokeSource& src, agent::RegionOpKind kind, const Rect& area,
                    LayerId layerId, u32 changed) {
    const Result<void> rec = recordRegionOp(doc, src, kind, area, layerId, false, changed);
    if (!rec.ok()) {
        const Result<void> rolled = doc.undo();
        if (!rolled.ok())
            return Err(std::string(rec.message()) + " (롤백도 실패: " + rolled.message() + ")", ErrorCode::IoError);
        return rec;
    }
    return Ok();
}

Rect canvasRect(const Document& doc) {
    const Size cs = doc.canvasSize();
    return Rect{0, 0, cs.width, cs.height};
}

/// 쌍선형 샘플(프리멀티플라이드로 섞어 가장자리 검은 테두리를 막는다). 밖은 투명.
void sampleBilinear(const ora::Image8& img, f64 fx, f64 fy, u8 out[4]) noexcept {
    const f64 x = fx - 0.5, y = fy - 0.5;
    const i32 x0 = static_cast<i32>(std::floor(x)), y0 = static_cast<i32>(std::floor(y));
    const f32 tx = static_cast<f32>(x - x0), ty = static_cast<f32>(y - y0);
    f32 acc[4] = {0, 0, 0, 0};
    const auto tap = [&](i32 px, i32 py, f32 w) {
        if (w <= 0.0f || px < 0 || py < 0 || px >= img.width || py >= img.height) return;
        const u8* p = img.pixels.data() + (static_cast<usize>(py) * static_cast<usize>(img.width) + static_cast<usize>(px)) * 4u;
        const f32 a = static_cast<f32>(p[3]) * kInv255 * w;
        acc[0] += static_cast<f32>(p[0]) * a;
        acc[1] += static_cast<f32>(p[1]) * a;
        acc[2] += static_cast<f32>(p[2]) * a;
        acc[3] += a;
    };
    tap(x0, y0, (1 - tx) * (1 - ty));
    tap(x0 + 1, y0, tx * (1 - ty));
    tap(x0, y0 + 1, (1 - tx) * ty);
    tap(x0 + 1, y0 + 1, tx * ty);
    if (acc[3] <= 1e-6f) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = toByte(acc[0] / acc[3]);
    out[1] = toByte(acc[1] / acc[3]);
    out[2] = toByte(acc[2] / acc[3]);
    out[3] = toByte(acc[3] * 255.0f);
}

void sampleNearest(const ora::Image8& img, f64 fx, f64 fy, u8 out[4]) noexcept {
    const i32 px = static_cast<i32>(std::floor(fx)), py = static_cast<i32>(std::floor(fy));
    if (px < 0 || py < 0 || px >= img.width || py >= img.height) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    const u8* p = img.pixels.data() + (static_cast<usize>(py) * static_cast<usize>(img.width) + static_cast<usize>(px)) * 4u;
    std::copy(p, p + 4, out);
}

/// 소스 오버(straight alpha).
void over(u8* dst, const u8* s) noexcept {
    const u32 sa = s[3];
    if (sa == 0) return;
    const u32 da = dst[3];
    const u32 oa = sa + (da * (255u - sa) + 127u) / 255u;
    if (oa == 0) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }
    const u32 wd = (da * (255u - sa) + 127u) / 255u;
    for (int c = 0; c < 3; ++c) dst[c] = static_cast<u8>((s[c] * sa + dst[c] * wd) / oa);
    dst[3] = static_cast<u8>(oa);
}

/// 모든 래스터 레이어를 깊이 우선으로.
void rasterLayers(const std::vector<LayerPtr>& list, std::vector<LayerPtr>& out) {
    for (const LayerPtr& l : list) {
        if (l->kind() == LayerKind::Group) rasterLayers(l->children(), out);
        else out.push_back(l);
    }
}

/// 캔버스 크기 변경 명령(트리 + 선택). 픽셀은 별도 명령이 맡는다.
class CanvasSizeCommand final : public UndoCommand {
public:
    CanvasSizeCommand(Document& doc, Size before, Size after) : doc_(doc), before_(before), after_(after) {}
    [[nodiscard]] const std::string& text() const override { return text_; }
    [[nodiscard]] Result<void> undo() override { apply(before_); return Ok(); }
    [[nodiscard]] Result<void> redo() override { apply(after_); return Ok(); }
    void affectedTiles(DirtyTiles& out) const override {
        const Size s{std::max(before_.width, after_.width), std::max(before_.height, after_.height)};
        tilesForRect(Rect{0, 0, s.width, s.height}, out);
    }

private:
    void apply(Size s) {
        doc_.layers().setCanvasSize(s);
        doc_.setSelectionMask(SelectionMask::all(s));
    }
    Document& doc_;
    Size before_, after_;
    std::string text_ = "캔버스 크기";
};

/// 레이어 하나의 영역을 새 이미지로 덮어쓰는 스냅샷 명령을 만들고 적용한다(compound 용 — 스택에 직접 넣지 않는다).
Result<std::unique_ptr<TileSnapshotCommand>> writeLayerRegion(Layer& layer, const Rect& area, const ora::Image8& img,
                                                              const std::string& text, u32& changed) {
    TileMap* map = layer.tiles();
    if (map == nullptr) return Err("그룹 레이어", ErrorCode::InvalidArgument);
    DirtyTiles tiles;
    tilesForRect(area, tiles);
    // 원래 내용이 있던 타일도 전부(영역 밖 내용은 clear 로 지운다).
    map->collectTiles(map->bounds(), tiles);
    auto cmd = TileSnapshotCommand::begin(text, map);
    cmd->captureBefore(tiles);
    // 영역 밖 픽셀은 없애고(캔버스 연산은 캔버스 밖 내용을 버린다), 영역을 쓴다.
    for (const TileCoord& c : tiles) {
        const Rect tr = c.canvasRect();
        if (!tr.intersected(area).isEmpty()) continue;
        map->erase(c);
    }
    if (!area.isEmpty()) {
        const Result<void> w = ora::writeRegion(*map, area, img);
        if (!w.ok()) return w.error();
    }
    cmd->captureAfter(tiles);
    changed = static_cast<u32>(cmd->changedTileCount());
    return Ok(std::move(cmd));
}

} // namespace

// ── 색 보정 ───────────────────────────────────────────────────────────────

void applyAdjust(const AdjustParams& p, u8* rgba, usize n) {
    switch (p.kind) {
    case AdjustKind::HueSaturation: {
        const f32 dh = p.hue, ds = p.saturation / 100.0f, dl = p.lightness / 100.0f;
        for (usize i = 0; i < n; ++i, rgba += 4) {
            f32 h, s, l;
            rgbToHsl(rgba[0] * kInv255, rgba[1] * kInv255, rgba[2] * kInv255, h, s, l);
            h = std::fmod(h + dh + 360.0f, 360.0f);
            s = std::clamp(ds >= 0.0f ? s + (1.0f - s) * ds : s * (1.0f + ds), 0.0f, 1.0f);
            l = std::clamp(dl >= 0.0f ? l + (1.0f - l) * dl : l * (1.0f + dl), 0.0f, 1.0f);
            f32 r, g, b;
            hslToRgb(h, s, l, r, g, b);
            rgba[0] = toByte(r * 255.0f);
            rgba[1] = toByte(g * 255.0f);
            rgba[2] = toByte(b * 255.0f);
        }
        break;
    }
    case AdjustKind::BrightnessContrast: {
        const f32 br = p.brightness / 100.0f * 255.0f;
        const f32 c = std::clamp(p.contrast, -100.0f, 100.0f) / 100.0f;
        const f32 k = c >= 0.0f ? 1.0f / std::max(1.0f - c, 0.01f) : 1.0f + c;
        u8 lut[256];
        for (int i = 0; i < 256; ++i) lut[i] = toByte((static_cast<f32>(i) - 127.5f) * k + 127.5f + br);
        for (usize i = 0; i < n; ++i, rgba += 4) {
            rgba[0] = lut[rgba[0]];
            rgba[1] = lut[rgba[1]];
            rgba[2] = lut[rgba[2]];
        }
        break;
    }
    case AdjustKind::Levels: {
        u8 lut[256];
        const f32 ib = static_cast<f32>(std::clamp(p.inBlack, 0, 255)), iw = static_cast<f32>(std::clamp(p.inWhite, 0, 255));
        const f32 ob = static_cast<f32>(std::clamp(p.outBlack, 0, 255)), ow = static_cast<f32>(std::clamp(p.outWhite, 0, 255));
        const f32 g = 1.0f / std::clamp(p.gamma, 0.1f, 10.0f);
        for (int i = 0; i < 256; ++i) {
            f32 t = (static_cast<f32>(i) - ib) / std::max(iw - ib, 1.0f);
            t = std::pow(std::clamp(t, 0.0f, 1.0f), g);
            lut[i] = toByte(ob + (ow - ob) * t);
        }
        for (usize i = 0; i < n; ++i, rgba += 4) {
            rgba[0] = lut[rgba[0]];
            rgba[1] = lut[rgba[1]];
            rgba[2] = lut[rgba[2]];
        }
        break;
    }
    case AdjustKind::Curves: {
        u8 m[256], r[256], g[256], b[256];
        curveToLut(p.curve, m);
        curveToLut(p.curveR, r);
        curveToLut(p.curveG, g);
        curveToLut(p.curveB, b);
        for (usize i = 0; i < n; ++i, rgba += 4) {
            rgba[0] = m[r[rgba[0]]];
            rgba[1] = m[g[rgba[1]]];
            rgba[2] = m[b[rgba[2]]];
        }
        break;
    }
    case AdjustKind::Invert:
        for (usize i = 0; i < n; ++i, rgba += 4) {
            rgba[0] = static_cast<u8>(255 - rgba[0]);
            rgba[1] = static_cast<u8>(255 - rgba[1]);
            rgba[2] = static_cast<u8>(255 - rgba[2]);
        }
        break;
    case AdjustKind::Desaturate:
        for (usize i = 0; i < n; ++i, rgba += 4) {
            const u8 y = toByte(0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2]);
            rgba[0] = rgba[1] = rgba[2] = y;
        }
        break;
    case AdjustKind::Threshold:
        for (usize i = 0; i < n; ++i, rgba += 4) {
            const f32 y = 0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2];
            const u8 v = y >= static_cast<f32>(p.threshold) ? 255 : 0;
            rgba[0] = rgba[1] = rgba[2] = v;
        }
        break;
    case AdjustKind::Posterize: {
        const int levels = std::clamp(p.levels, 2, 255);
        u8 lut[256];
        for (int i = 0; i < 256; ++i) {
            const int q = i * levels / 256;
            lut[i] = toByte(static_cast<f32>(q) * 255.0f / static_cast<f32>(levels - 1));
        }
        for (usize i = 0; i < n; ++i, rgba += 4) {
            rgba[0] = lut[rgba[0]];
            rgba[1] = lut[rgba[1]];
            rgba[2] = lut[rgba[2]];
        }
        break;
    }
    }
}

Result<u32> adjustLayer(Document& doc, const StrokeSource& src, LayerId layerId, const AdjustParams& p) {
    if (doc.recordingBroken()) return Err("기록이 고장 나 있다 — 기록 없이 바꾸지 않는다(docs/06 결정 ④)", ErrorCode::IoError);
    const LayerPtr layer = doc.layers().find(layerId);
    if (!layer || layer->tiles() == nullptr) return Err("래스터 레이어가 아니다", ErrorCode::InvalidArgument);
    if (layer->locked()) return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    Rect area = layer->tiles()->bounds().intersected(canvasRect(doc));
    const SelectionMask& sel = doc.selectionMask();
    if (!sel.isAll()) area = area.intersected(sel.bounds());
    if (area.isEmpty()) return Ok(u32{0});

    Result<ora::Image8> had = ora::readRegion(*layer->tiles(), area);
    if (!had.ok()) return had.error();
    ora::Image8 img = had.value();
    applyAdjust(p, img.pixels.data(), static_cast<usize>(area.width) * static_cast<usize>(area.height));
    blendBySelection(sel, area, had.value(), img);

    u32 changed = 0;
    const Result<void> w = doc.paintPixels(layerId, area, img.pixels.data(), img.pixels.size(), "색 보정", &changed);
    if (!w.ok()) return w.error();
    const Result<void> f = finish(doc, src, agent::RegionOpKind::Adjust, area, layerId, changed);
    if (!f.ok()) return f.error();
    return Ok(changed);
}

// ── 자유 변형 ─────────────────────────────────────────────────────────────

Result<ora::Image8> extractForTransform(Document& doc, LayerId layerId, Rect& outArea) {
    outArea = Rect{};
    const LayerPtr layer = doc.layers().find(layerId);
    if (!layer || layer->tiles() == nullptr) return Err("래스터 레이어가 아니다", ErrorCode::InvalidArgument);
    if (layer->locked()) return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    const Rect canvas = canvasRect(doc);
    const SelectionMask& sel = doc.selectionMask();
    Rect S = layer->tiles()->bounds().intersected(canvas);
    if (!sel.isAll()) S = S.intersected(sel.bounds());
    if (S.isEmpty()) return Ok(ora::Image8{});
    // 타일 경계는 64px 단위라 헐렁하다 — 실제 내용(알파>0 · 선택 안)으로 조인다.
    Result<ora::Image8> probe = ora::readRegion(*layer->tiles(), S);
    if (!probe.ok()) return probe.error();
    i32 minx = S.width, miny = S.height, maxx = -1, maxy = -1;
    for (i32 y = 0; y < S.height; ++y) {
        const u8* row = probe.value().pixels.data() + static_cast<usize>(y) * probe.value().stride();
        for (i32 x = 0; x < S.width; ++x) {
            if (row[x * 4 + 3] == 0) continue;
            if (!sel.isAll() && sel.valueAt(S.x + x, S.y + y) == 0) continue;
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);
        }
    }
    if (maxx < 0) return Ok(ora::Image8{});
    S = Rect{S.x + minx, S.y + miny, maxx - minx + 1, maxy - miny + 1};
    Result<ora::Image8> had = ora::readRegion(*layer->tiles(), S);
    if (!had.ok()) return had.error();
    ora::Image8 extract = std::move(had).value();
    if (!sel.isAll()) {
        for (i32 y = 0; y < S.height; ++y) {
            u8* e = extract.pixels.data() + static_cast<usize>(y) * extract.stride();
            for (i32 x = 0; x < S.width; ++x, e += 4) {
                const u32 m = sel.valueAt(S.x + x, S.y + y);
                e[3] = static_cast<u8>((e[3] * m + 127u) / 255u);
            }
        }
    }
    outArea = S;
    return Ok(std::move(extract));
}

ora::Image8 remainderForTransform(Document& doc, LayerId layerId, const Rect& S) {
    ora::Image8 remain = ora::Image8::make(S.width, S.height);
    const LayerPtr layer = doc.layers().find(layerId);
    if (!layer || layer->tiles() == nullptr || S.isEmpty()) return remain;
    const SelectionMask& sel = doc.selectionMask();
    if (sel.isAll()) return remain; // 전부 잘려 나간다
    Result<ora::Image8> had = ora::readRegion(*layer->tiles(), S);
    if (!had.ok()) return remain;
    remain = std::move(had).value();
    for (i32 y = 0; y < S.height; ++y) {
        u8* r = remain.pixels.data() + static_cast<usize>(y) * remain.stride();
        for (i32 x = 0; x < S.width; ++x, r += 4) {
            const u32 m = sel.valueAt(S.x + x, S.y + y);
            r[3] = static_cast<u8>((r[3] * (255u - m) + 127u) / 255u);
        }
    }
    return remain;
}

Result<Rect> transformLayer(Document& doc, const StrokeSource& src, LayerId layerId, const TransformParams& p) {
    if (doc.recordingBroken()) return Err("기록이 고장 나 있다 — 기록 없이 바꾸지 않는다(docs/06 결정 ④)", ErrorCode::IoError);
    const LayerPtr layer = doc.layers().find(layerId);
    if (!layer || layer->tiles() == nullptr) return Err("래스터 레이어가 아니다", ErrorCode::InvalidArgument);
    if (layer->locked()) return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    if (!(std::fabs(p.scaleX) > 1e-4 && std::fabs(p.scaleY) > 1e-4)) return Err("배율이 0 이다", ErrorCode::InvalidArgument);

    const Rect canvas = canvasRect(doc);
    const SelectionMask& sel = doc.selectionMask();
    Rect S{};
    Result<ora::Image8> ext = extractForTransform(doc, layerId, S);
    if (!ext.ok()) return ext.error();
    if (S.isEmpty()) return Ok(Rect{});
    ora::Image8 extract = std::move(ext).value();
    ora::Image8 remain = remainderForTransform(doc, layerId, S);
    (void)sel;

    // 정방향 행렬: 원본 좌표 → 결과 좌표. T(pivot + d) · R · Scale · Flip · T(−pivot)
    const f64 px = p.usePivot ? p.pivotX : S.x + S.width * 0.5;
    const f64 py = p.usePivot ? p.pivotY : S.y + S.height * 0.5;
    const f64 th = p.rotateDeg * 3.14159265358979323846 / 180.0;
    const f64 ct = std::cos(th), st = std::sin(th);
    const f64 sx = p.scaleX * (p.flipH ? -1.0 : 1.0), sy = p.scaleY * (p.flipV ? -1.0 : 1.0);
    // M = [a b; c d], 결과 = M·(src − pivot) + pivot + d
    const f64 a = ct * sx, b = -st * sy, c = st * sx, d = ct * sy;
    const auto fwd = [&](f64 x, f64 y, f64& ox, f64& oy) {
        const f64 rx = x - px, ry = y - py;
        ox = a * rx + b * ry + px + p.dx;
        oy = c * rx + d * ry + py + p.dy;
    };
    // 결과 경계 상자.
    f64 minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (const auto& [cx, cy] : {std::pair<f64, f64>{S.x, S.y}, {S.x + S.width, S.y}, {S.x, S.y + S.height},
                                 {S.x + S.width, S.y + S.height}}) {
        f64 ox, oy;
        fwd(cx, cy, ox, oy);
        minx = std::min(minx, ox); maxx = std::max(maxx, ox);
        miny = std::min(miny, oy); maxy = std::max(maxy, oy);
    }
    Rect D{static_cast<i32>(std::floor(minx)) - 1, static_cast<i32>(std::floor(miny)) - 1,
           static_cast<i32>(std::ceil(maxx)) - static_cast<i32>(std::floor(minx)) + 2,
           static_cast<i32>(std::ceil(maxy)) - static_cast<i32>(std::floor(miny)) + 2};
    D = D.intersected(canvas);
    const Rect U = D.isEmpty() ? S : S.united(D);

    // 역행렬(결과 → 원본).
    const f64 det = a * d - b * c;
    if (std::fabs(det) < 1e-12) return Err("변형 행렬이 특이하다", ErrorCode::InvalidArgument);
    const f64 ia = d / det, ib = -b / det, ic = -c / det, id = a / det;

    // 결과 이미지: 현재 픽셀에서 시작 → S 자리는 remain 으로 → 변형본을 소스 오버.
    Result<ora::Image8> base = ora::readRegion(*layer->tiles(), U);
    if (!base.ok()) return base.error();
    ora::Image8 out = std::move(base).value();
    for (i32 y = 0; y < S.height; ++y) {
        u8* o = out.pixels.data() + (static_cast<usize>(S.y - U.y + y) * out.stride()) + static_cast<usize>(S.x - U.x) * 4u;
        const u8* r = remain.pixels.data() + static_cast<usize>(y) * remain.stride();
        std::copy(r, r + static_cast<usize>(S.width) * 4u, o);
    }
    if (!D.isEmpty()) {
        for (i32 y = 0; y < D.height; ++y) {
            u8* o = out.pixels.data() + (static_cast<usize>(D.y - U.y + y) * out.stride()) + static_cast<usize>(D.x - U.x) * 4u;
            for (i32 x = 0; x < D.width; ++x, o += 4) {
                const f64 ox = D.x + x + 0.5 - px - p.dx, oy = D.y + y + 0.5 - py - p.dy;
                const f64 sxf = ia * ox + ib * oy + px - S.x;
                const f64 syf = ic * ox + id * oy + py - S.y;
                u8 s[4];
                if (p.bilinear) sampleBilinear(extract, sxf, syf, s);
                else sampleNearest(extract, sxf, syf, s);
                over(o, s);
            }
        }
    }

    u32 changed = 0;
    const Result<void> w = doc.paintPixels(layerId, U, out.pixels.data(), out.pixels.size(), "자유 변형", &changed);
    if (!w.ok()) return w.error();
    const Result<void> f = finish(doc, src, agent::RegionOpKind::Transform, U, layerId, changed);
    if (!f.ok()) return f.error();
    return Ok(U);
}

// ── 그라데이션 ───────────────────────────────────────────────────────────

Result<u32> fillGradient(Document& doc, const StrokeSource& src, LayerId layerId, const GradientParams& p) {
    if (doc.recordingBroken()) return Err("기록이 고장 나 있다 — 기록 없이 바꾸지 않는다(docs/06 결정 ④)", ErrorCode::IoError);
    const LayerPtr layer = doc.layers().find(layerId);
    if (!layer || layer->tiles() == nullptr) return Err("래스터 레이어가 아니다", ErrorCode::InvalidArgument);
    if (layer->locked()) return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    const SelectionMask& sel = doc.selectionMask();
    const Rect area = (sel.isAll() ? canvasRect(doc) : sel.bounds().intersected(canvasRect(doc)));
    if (area.isEmpty()) return Ok(u32{0});
    Result<ora::Image8> had = ora::readRegion(*layer->tiles(), area);
    if (!had.ok()) return had.error();
    ora::Image8 img = had.value();
    const f64 vx = p.to.x - p.from.x, vy = p.to.y - p.from.y;
    const f64 len2 = vx * vx + vy * vy;
    const bool alphaLock = layer->alphaLocked();
    for (i32 y = 0; y < area.height; ++y) {
        u8* d = img.pixels.data() + static_cast<usize>(y) * img.stride();
        for (i32 x = 0; x < area.width; ++x, d += 4) {
            const f64 px = area.x + x + 0.5, py = area.y + y + 0.5;
            f64 t;
            if (len2 < 1e-9) t = 1.0;
            else if (p.radial) t = std::sqrt(((px - p.from.x) * (px - p.from.x) + (py - p.from.y) * (py - p.from.y)) / len2);
            else t = ((px - p.from.x) * vx + (py - p.from.y) * vy) / len2;
            t = std::clamp(t, 0.0, 1.0);
            u8 g[4];
            for (int c = 0; c < 4; ++c) {
                const f64 a = c == 0 ? p.colorA.r : c == 1 ? p.colorA.g : c == 2 ? p.colorA.b : p.colorA.a;
                const f64 b = c == 0 ? p.colorB.r : c == 1 ? p.colorB.g : c == 2 ? p.colorB.b : p.colorB.a;
                g[c] = toByte(static_cast<f32>(a + (b - a) * t));
            }
            if (alphaLock) {
                const u32 sa = g[3];
                for (int c = 0; c < 3; ++c) d[c] = static_cast<u8>((g[c] * sa + d[c] * (255u - sa) + 127u) / 255u);
            } else {
                over(d, g);
            }
        }
    }
    blendBySelection(sel, area, had.value(), img);
    u32 changed = 0;
    const Result<void> w = doc.paintPixels(layerId, area, img.pixels.data(), img.pixels.size(), "그라데이션", &changed);
    if (!w.ok()) return w.error();
    const Result<void> f = finish(doc, src, agent::RegionOpKind::Gradient, area, layerId, changed);
    if (!f.ok()) return f.error();
    return Ok(changed);
}

// ── 캔버스 연산 ───────────────────────────────────────────────────────────

namespace {

/// 모든 래스터 레이어의 캔버스 영역을 fn 으로 바꿔 쓴다(한 실행취소). newSize 가 있으면 캔버스 크기도 바꾼다.
template <typename Fn>
Result<void> rewriteAllLayers(Document& doc, const StrokeSource& src, const char* text, const Size* newSize, Fn fn) {
    if (doc.recordingBroken()) return Err("기록이 고장 나 있다 — 기록 없이 바꾸지 않는다(docs/06 결정 ④)", ErrorCode::IoError);
    const Size before = doc.canvasSize();
    const Size after = newSize != nullptr ? *newSize : before;
    if (after.width <= 0 || after.height <= 0 || after.width > 32768 || after.height > 32768)
        return Err("캔버스 크기가 범위 밖이다", ErrorCode::InvalidArgument);
    std::vector<LayerPtr> layers;
    rasterLayers(doc.layers().roots(), layers);
    auto compound = std::make_unique<CompoundCommand>(text);
    struct Done { LayerId id; Rect area; u32 changed; };
    std::vector<Done> done;
    const Rect srcRect{0, 0, before.width, before.height};
    const Rect dstRect{0, 0, after.width, after.height};
    // 중간에 실패하면 이미 고쳐 쓴 레이어를 되돌린다 — 반쯤 바뀐 문서를 남기지 않는다.
    const auto bail = [&](const Error& e) -> Result<void> {
        const Result<void> un = compound->undo();
        if (!un.ok())
            return Err(e.message + " (되돌리기도 실패: " + un.message() + ")", ErrorCode::IoError);
        return e;
    };
    // 🔴 잠긴 레이어도 같이 간다 — 캔버스 연산은 좌표계 자체를 바꾸므로 건너뛰면 그 레이어만 어긋난다(포토샵도 같다).
    for (const LayerPtr& l : layers) {
        Result<ora::Image8> had = ora::readRegion(*l->tiles(), srcRect);
        if (!had.ok()) return bail(had.error());
        ora::Image8 img = fn(had.value());
        u32 changed = 0;
        Result<std::unique_ptr<TileSnapshotCommand>> cmd = writeLayerRegion(*l, dstRect, img, text, changed);
        if (!cmd.ok()) return bail(cmd.error());
        compound->add(std::move(cmd).value());
        done.push_back({l->id(), dstRect.united(srcRect), changed});
    }
    if (newSize != nullptr && !(after == before)) {
        auto sz = std::make_unique<CanvasSizeCommand>(doc, before, after);
        (void)sz->redo();
        compound->add(std::move(sz));
    } else {
        doc.setSelectionMask(SelectionMask::all(before));
    }
    doc.undoStack().push(std::move(compound));
    doc.markDirty();
    for (const Done& dn : done) {
        const Result<void> rec = recordRegionOp(doc, src, agent::RegionOpKind::Transform, dn.area, dn.id, false, dn.changed);
        if (!rec.ok()) {
            const Result<void> rolled = doc.undo();
            if (!rolled.ok())
                return Err(std::string(rec.message()) + " (롤백도 실패: " + rolled.message() + ")", ErrorCode::IoError);
            return rec;
        }
    }
    return Ok();
}

} // namespace

Result<void> flipCanvas(Document& doc, const StrokeSource& src, bool horizontal) {
    return rewriteAllLayers(doc, src, horizontal ? "캔버스 좌우 뒤집기" : "캔버스 상하 뒤집기", nullptr,
                            [&](const ora::Image8& in) {
                                ora::Image8 out = ora::Image8::make(in.width, in.height);
                                for (i32 y = 0; y < in.height; ++y)
                                    for (i32 x = 0; x < in.width; ++x) {
                                        const i32 sx = horizontal ? in.width - 1 - x : x;
                                        const i32 sy = horizontal ? y : in.height - 1 - y;
                                        const u8* s = in.pixels.data() + (static_cast<usize>(sy) * in.width + sx) * 4u;
                                        u8* d = out.pixels.data() + (static_cast<usize>(y) * out.width + x) * 4u;
                                        std::copy(s, s + 4, d);
                                    }
                                return out;
                            });
}

Result<void> rotateCanvas(Document& doc, const StrokeSource& src, int quarterTurns) {
    const int q = ((quarterTurns % 4) + 4) % 4;
    if (q == 0) return Ok();
    const Size before = doc.canvasSize();
    const Size after = (q == 2) ? before : Size{before.height, before.width};
    return rewriteAllLayers(doc, src, "캔버스 회전", &after, [&](const ora::Image8& in) {
        ora::Image8 out = ora::Image8::make(after.width, after.height);
        for (i32 y = 0; y < out.height; ++y)
            for (i32 x = 0; x < out.width; ++x) {
                i32 sx, sy;
                if (q == 1)      { sx = y;                 sy = in.height - 1 - x; } // 시계 90°
                else if (q == 2) { sx = in.width - 1 - x;  sy = in.height - 1 - y; }
                else             { sx = in.width - 1 - y;  sy = x; }                // 270°
                const u8* s = in.pixels.data() + (static_cast<usize>(sy) * in.width + sx) * 4u;
                u8* d = out.pixels.data() + (static_cast<usize>(y) * out.width + x) * 4u;
                std::copy(s, s + 4, d);
            }
        return out;
    });
}

Result<void> resizeCanvas(Document& doc, const StrokeSource& src, Size newSize, int anchorX, int anchorY) {
    const Size before = doc.canvasSize();
    const i32 offX = (newSize.width - before.width) * std::clamp(anchorX, 0, 2) / 2;
    const i32 offY = (newSize.height - before.height) * std::clamp(anchorY, 0, 2) / 2;
    return rewriteAllLayers(doc, src, "캔버스 크기 조절", &newSize, [&](const ora::Image8& in) {
        ora::Image8 out = ora::Image8::make(newSize.width, newSize.height);
        for (i32 y = 0; y < in.height; ++y) {
            const i32 dy = y + offY;
            if (dy < 0 || dy >= out.height) continue;
            for (i32 x = 0; x < in.width; ++x) {
                const i32 dx = x + offX;
                if (dx < 0 || dx >= out.width) continue;
                const u8* s = in.pixels.data() + (static_cast<usize>(y) * in.width + x) * 4u;
                u8* d = out.pixels.data() + (static_cast<usize>(dy) * out.width + dx) * 4u;
                std::copy(s, s + 4, d);
            }
        }
        return out;
    });
}

Result<void> cropCanvas(Document& doc, const StrokeSource& src, const Rect& area) {
    const Rect r = area.intersected(canvasRect(doc));
    if (r.isEmpty()) return Err("자를 영역이 비었다", ErrorCode::InvalidArgument);
    const Size newSize{r.width, r.height};
    return rewriteAllLayers(doc, src, "자르기", &newSize, [&](const ora::Image8& in) {
        ora::Image8 out = ora::Image8::make(r.width, r.height);
        for (i32 y = 0; y < r.height; ++y) {
            const u8* s = in.pixels.data() + (static_cast<usize>(r.y + y) * in.width + r.x) * 4u;
            u8* d = out.pixels.data() + static_cast<usize>(y) * out.stride();
            std::copy(s, s + static_cast<usize>(r.width) * 4u, d);
        }
        return out;
    });
}

Result<void> scaleImage(Document& doc, const StrokeSource& src, Size newSize) {
    const Size before = doc.canvasSize();
    if (newSize.width <= 0 || newSize.height <= 0) return Err("크기가 0 이다", ErrorCode::InvalidArgument);
    const f64 kx = static_cast<f64>(before.width) / newSize.width, ky = static_cast<f64>(before.height) / newSize.height;
    return rewriteAllLayers(doc, src, "이미지 크기 조절", &newSize, [&](const ora::Image8& in) {
        // 축소는 알파 가중 박스 필터, 확대는 쌍선형.
        if (kx > 1.0 || ky > 1.0) {
            ora::Image8 out = ora::Image8::make(newSize.width, newSize.height);
            for (i32 y = 0; y < out.height; ++y) {
                const i32 y0 = static_cast<i32>(std::floor(y * ky)), y1 = std::max(y0 + 1, static_cast<i32>(std::floor((y + 1) * ky)));
                for (i32 x = 0; x < out.width; ++x) {
                    const i32 x0 = static_cast<i32>(std::floor(x * kx)), x1 = std::max(x0 + 1, static_cast<i32>(std::floor((x + 1) * kx)));
                    f64 acc[4] = {0, 0, 0, 0};
                    i32 cnt = 0;
                    for (i32 sy = y0; sy < std::min(y1, in.height); ++sy)
                        for (i32 sx = x0; sx < std::min(x1, in.width); ++sx, ++cnt) {
                            const u8* s = in.pixels.data() + (static_cast<usize>(sy) * in.width + sx) * 4u;
                            const f64 a = s[3] / 255.0;
                            acc[0] += s[0] * a; acc[1] += s[1] * a; acc[2] += s[2] * a; acc[3] += a;
                        }
                    u8* d = out.pixels.data() + (static_cast<usize>(y) * out.width + x) * 4u;
                    if (cnt == 0 || acc[3] <= 1e-9) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                    d[0] = toByte(static_cast<f32>(acc[0] / acc[3]));
                    d[1] = toByte(static_cast<f32>(acc[1] / acc[3]));
                    d[2] = toByte(static_cast<f32>(acc[2] / acc[3]));
                    d[3] = toByte(static_cast<f32>(acc[3] / cnt * 255.0));
                }
            }
            return out;
        }
        ora::Image8 out = ora::Image8::make(newSize.width, newSize.height);
        for (i32 y = 0; y < out.height; ++y)
            for (i32 x = 0; x < out.width; ++x) {
                u8* d = out.pixels.data() + (static_cast<usize>(y) * out.width + x) * 4u;
                sampleBilinear(in, (x + 0.5) * kx, (y + 0.5) * ky, d);
            }
        return out;
    });
}

} // namespace mari::app
