// Mari Paint — 브러시 미리보기 구현 (ui/brush_preview.hpp)
#include "brush_preview.hpp"

#include <mari/core/tile.hpp>
#include <mari/ora/image.hpp>
#include <mari/stroke/native_engine.hpp>

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace mari::ui {

namespace {

QImage checkerboard(QSize size, int cell = 6) {
    QImage img(size, QImage::Format_RGBA8888);
    img.fill(QColor(200, 200, 200));
    QPainter p(&img);
    for (int y = 0; y < size.height(); y += cell) {
        for (int x = (y / cell % 2) * cell; x < size.width(); x += cell * 2) {
            p.fillRect(x, y, cell, cell, QColor(160, 160, 160));
        }
    }
    return img;
}

} // namespace

QImage renderBrushPreview(const brush::MariBrushPreset& preset, QSize size, QColor fg, QColor bg,
                          bool fitDiameter, bool checker) {
    QImage out = checker ? checkerboard(size) : QImage(size, QImage::Format_RGBA8888);
    if (!checker) out.fill(bg);
    if (size.width() < 8 || size.height() < 8) return out;

    brush::MariBrushPreset p = preset;
    if (fitDiameter) {
        // 큰 붓도 한눈에: 지름을 높이의 45% 이하로. 듀얼 팁도 같은 비율로.
        const f32 maxD = static_cast<f32>(size.height()) * 0.45f;
        if (p.tip.diameter > maxD) {
            const f32 k = maxD / p.tip.diameter;
            p.tip.diameter = maxD;
            if (p.dual.has_value()) p.dual->tip.diameter *= k;
            if (p.texture.has_value()) p.texture->scale = std::max(0.05f, p.texture->scale * k);
        }
    }
    Result<brush::BrushEnginePtr> eng = stroke::makeNativeEngine();
    if (!eng.ok()) return out;
    brush::IBrushEngine& e = *eng.value();
    if (!e.setPreset(p, nullptr).ok()) return out;
    Result<TileMapPtr> map = makeTileMap(PixelFormat::RGBA8);
    if (!map.ok()) return out;

    brush::StrokeContext ctx(StrokeSource::humanPen());
    ctx.target = map.value().get();
    ctx.color = Color8::rgba(static_cast<u8>(fg.red()), static_cast<u8>(fg.green()), static_cast<u8>(fg.blue()), 255);
    ctx.background = Color8::rgba(static_cast<u8>(bg.red()), static_cast<u8>(bg.green()), static_cast<u8>(bg.blue()), 255);
    ctx.seed = 7;
    if (!e.beginStroke(ctx).ok()) return out;

    // S 곡선(3차 베지어), 양 끝이 가는 필압.
    const f32 w = static_cast<f32>(size.width()), h = static_cast<f32>(size.height());
    const f32 pad = std::max(6.0f, p.tip.diameter * 0.55f);
    const PointF P0{pad, h * 0.62f}, P1{w * 0.30f, h * 0.05f}, P2{w * 0.62f, h * 0.98f}, P3{w - pad, h * 0.40f};
    const auto at = [&](f32 t) {
        const f32 u = 1.0f - t;
        return PointF{u * u * u * P0.x + 3 * u * u * t * P1.x + 3 * u * t * t * P2.x + t * t * t * P3.x,
                      u * u * u * P0.y + 3 * u * u * t * P1.y + 3 * u * t * t * P2.y + t * t * t * P3.y};
    };
    DirtyTiles dirty;
    f32 t = 0.0f;
    PointF last = at(0.0f);
    f32 carried = 0.0f;
    int stamps = 0;
    const f32 dt = 1.0f / 600.0f;
    while (t <= 1.0f && stamps < 4000) {
        const PointF cur = at(t);
        const f32 pressure = std::clamp(std::sin(t * 3.14159265f), 0.05f, 1.0f);
        const f32 seg = std::hypot(cur.x - last.x, cur.y - last.y);
        carried += seg;
        const f32 spacing = std::max(e.spacingPx(pressure), 0.5f);
        if (stamps == 0 || carried >= spacing) {
            brush::StampInput in;
            in.pos = cur;
            in.pressure = pressure;
            in.fade = t;
            in.velocity = 0.3f;
            e.stamp(in, dirty);
            carried = 0.0f;
            ++stamps;
        }
        last = cur;
        t += dt;
    }
    e.endStroke(dirty);

    Result<ora::Image8> px = ora::readRegion(*map.value(), Rect{0, 0, size.width(), size.height()});
    if (!px.ok()) return out;
    const QImage stroke(px.value().pixels.data(), size.width(), size.height(), static_cast<int>(px.value().stride()),
                        QImage::Format_RGBA8888);
    QPainter painter(&out);
    painter.drawImage(0, 0, stroke);
    return out;
}

QImage renderTipPreview(const brush::BrushTip& tip, int px, QColor fg) {
    brush::MariBrushPreset p;
    p.tip = tip;
    p.tip.diameter = static_cast<f32>(px) * 0.8f;
    p.opacity = 1.0f;
    p.flow = 1.0f;
    Result<brush::BrushEnginePtr> eng = stroke::makeNativeEngine();
    QImage out(px, px, QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    if (!eng.ok() || !eng.value()->setPreset(p, nullptr).ok()) return out;
    Result<TileMapPtr> map = makeTileMap(PixelFormat::RGBA8);
    if (!map.ok()) return out;
    brush::StrokeContext ctx(StrokeSource::humanPen());
    ctx.target = map.value().get();
    ctx.color = Color8::rgba(static_cast<u8>(fg.red()), static_cast<u8>(fg.green()), static_cast<u8>(fg.blue()), 255);
    if (!eng.value()->beginStroke(ctx).ok()) return out;
    DirtyTiles dirty;
    brush::StampInput in;
    in.pos = PointF{static_cast<f32>(px) * 0.5f, static_cast<f32>(px) * 0.5f};
    eng.value()->stamp(in, dirty);
    eng.value()->endStroke(dirty);
    Result<ora::Image8> img = ora::readRegion(*map.value(), Rect{0, 0, px, px});
    if (!img.ok()) return out;
    return QImage(img.value().pixels.data(), px, px, static_cast<int>(img.value().stride()), QImage::Format_RGBA8888).copy();
}

} // namespace mari::ui
