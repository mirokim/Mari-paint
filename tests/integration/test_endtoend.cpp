// Mari Paint — 모듈 통합 검증.
//
// 각 모듈은 자기 테스트에서 자기만 본다(stroke 는 FakeTileMap 을, sigan 은 FakeSink 를 쓴다).
// 이 파일은 **모듈 사이의 이음매**만 본다 — 진짜 타입끼리 실제로 맞물리는지.
//
//     core(LayerTree/TileMap) → stroke(파이프라인+네이티브 엔진)
//       → core(합성기) → sigan(발행기/저널) → io/ora(왕복)
//
// 여기가 깨지면 "각자 빌드는 되는데 합치면 안 된다"는 뜻이다.
#include <mari/brush/engine.hpp>
#include <mari/core/compositor.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/layer_ops.hpp>
#include <mari/core/tile.hpp>
#include <mari/ora/ora.hpp>
#include <mari/sigan/journal.hpp>
#include <mari/sigan/publisher.hpp>
#include <mari/stroke/native_engine.hpp>
#include <mari/stroke/pipeline.hpp>
#include <mari/test/harness.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace mari;

namespace {

/// 테스트용 임시 파일 경로. 실행파일마다 다른 이름을 준다.
std::string tempPath(const char* stem) {
    static int counter = 0;
    std::string p = "mari_integration_";
    p += stem;
    p += "_";
    p += std::to_string(++counter);
    return p;
}

/// 원형 팁 프리셋 하나.
brush::MariBrushPreset roundPreset(f32 diameter, f32 spacing) {
    brush::MariBrushPreset p;
    p.name = "통합 테스트 원형";
    p.sourceFormat = "native";
    p.engine = stroke::kNativeEngineName;
    p.tip.kind = brush::TipKind::Procedural;
    p.tip.shape = brush::ProceduralShape::Circle;
    p.tip.diameter = diameter;
    p.tip.hardness = 1.0f;
    p.spacing = spacing;
    p.opacity = 1.0f;
    p.flow = 1.0f;
    return p;
}

/// 캔버스 좌표 하나를 읽는다. 타일이 없으면 완전 투명.
Color8 peek(const TileMap& map, i32 x, i32 y) {
    const TileCoord tc{tileIndexFor(x), tileIndexFor(y)};
    ConstTilePtr t = map.at(tc);
    if (!t)
        return Color8{0, 0, 0, 0};
    const u8* p = t->pixels() + static_cast<usize>(y - tileOrigin(tc.ty)) * t->stride() +
                  static_cast<usize>(x - tileOrigin(tc.tx)) * 4u;
    return Color8{p[0], p[1], p[2], p[3]};
}

/// 직선 하나를 raw 이벤트로 만든다. 시각은 단조 증가한다.
std::vector<stroke::RawInputEvent> lineEvents(f64 x0, f64 y0, f64 x1, f64 y1, int n) {
    std::vector<stroke::RawInputEvent> out;
    out.reserve(static_cast<usize>(n));
    for (int i = 0; i < n; ++i) {
        const f64 u = (n == 1) ? 0.0 : static_cast<f64>(i) / static_cast<f64>(n - 1);
        stroke::RawInputEvent e;
        e.x = x0 + (x1 - x0) * u;
        e.y = y0 + (y1 - y0) * u;
        e.pressure = 1.0f;
        e.pressureMax = 1.0f;
        e.hasPressure = true;
        e.hasTilt = false;
        // 8ms 간격. 단조 시계 기준 ns.
        e.timestampNs = static_cast<u64>(i) * 8'000'000ull;
        out.push_back(e);
    }
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. core ↔ stroke: 진짜 TileMap 에 진짜 엔진이 그린다.
//    stroke 테스트는 FakeTileMap 을 쓴다 — 여기서만 core 의 구현과 맞물린다.
// ─────────────────────────────────────────────────────────────────────────────
MARI_TEST(stroke_paints_into_real_core_tilemap) {
    auto treeR = makeLayerTree(Size{512, 512});
    CHECK(treeR.ok());
    LayerTreePtr tree = std::move(treeR).value();

    auto layerR = tree->addRaster("그림");
    CHECK(layerR.ok());
    LayerPtr layer = std::move(layerR).value();
    CHECK(layer->tiles() != nullptr);
    CHECK_EQ(layer->tiles()->tileCount(), usize{0});

    // 엔진 팩토리는 brush 헤더가 선언하고 stroke 모듈이 구현한다 — 그 연결도 여기서 본다.
    auto engineR = brush::createEngine(stroke::kNativeEngineName);
    CHECK(engineR.ok());
    brush::BrushEnginePtr engine = std::move(engineR).value();
    CHECK_EQ(std::string(engine->name()), std::string(stroke::kNativeEngineName));

    brush::ImportReport report;
    auto setR = engine->setPreset(roundPreset(16.0f, 0.1f), &report);
    CHECK(setR.ok());

    stroke::StrokePipeline pipe(engine.get());
    brush::StrokeContext ctx;
    ctx.target = layer->tiles();
    ctx.color = Color8{0, 0, 0, 255};
    ctx.layerId = layer->id();
    ctx.seed = 12345;

    const auto events = lineEvents(100.0, 100.0, 300.0, 100.0, 21);
    auto beginR = pipe.begin(ctx, events.front());
    CHECK(beginR.ok());
    for (usize i = 1; i + 1 < events.size(); ++i)
        pipe.extend(events[i]);
    pipe.end(events.back());

    // 핫 패스가 오류를 삼켰다면 여기서 드러난다.
    CHECK_EQ(static_cast<int>(pipe.lastError().code), static_cast<int>(ErrorCode::Unknown));
    CHECK(pipe.stampCount() > 10);

    const DirtyTiles& dirty = pipe.dirtyTiles();
    CHECK(!dirty.empty());

    // 실제로 칠해졌나. 선 위는 불투명, 멀리 떨어진 곳은 투명이어야 한다.
    const TileMap& tiles = *layer->tiles();
    CHECK_EQ(peek(tiles, 200, 100).a, u8{255});
    CHECK_EQ(peek(tiles, 200, 400).a, u8{0});

    // 희소성: 512×512 를 전부 잡으면 64개다. 가로줄 하나면 그보다 훨씬 적어야 한다.
    CHECK(tiles.tileCount() < usize{20});

    // 더티 목록이 실제 할당된 타일을 벗어나지 않는다(과대 보고 금지).
    for (const TileCoord& c : dirty)
        CHECK(tiles.at(c) != nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. stroke ↔ core 합성기: 더티 타일만 넘겨 평탄화한다.
// ─────────────────────────────────────────────────────────────────────────────
MARI_TEST(dirty_tiles_feed_compositor) {
    auto tree = makeLayerTree(Size{256, 256}).value();
    LayerPtr layer = tree->addRaster("그림").value();

    auto engine = brush::createEngine(stroke::kNativeEngineName).value();
    CHECK(engine->setPreset(roundPreset(20.0f, 0.1f)).ok());

    stroke::StrokePipeline pipe(engine.get());
    brush::StrokeContext ctx;
    ctx.target = layer->tiles();
    ctx.color = Color8{255, 0, 0, 255};
    ctx.layerId = layer->id();

    const auto events = lineEvents(40.0, 128.0, 200.0, 128.0, 17);
    CHECK(pipe.begin(ctx, events.front()).ok());
    for (usize i = 1; i + 1 < events.size(); ++i)
        pipe.extend(events[i]);
    pipe.end(events.back());

    const Rect area = pipe.dirtyBounds();
    CHECK(area.width > 0);
    CHECK(area.height > 0);

    // 더티 영역만 평탄화한다. 전체 캔버스를 그리는 경로는 쓰지 않는다.
    const usize stride = static_cast<usize>(area.width) * 4u;
    std::vector<u8> buf(stride * static_cast<usize>(area.height), 0u);
    auto compR = compositeDirty(*tree, pipe.dirtyTiles(), area, buf.data(), stride);
    CHECK(compR.ok());

    // 선 중앙은 빨간색이어야 한다.
    const i32 px = 120, py = 128;
    CHECK(px >= area.x && px < area.x + area.width);
    CHECK(py >= area.y && py < area.y + area.height);
    const u8* p = buf.data() + static_cast<usize>(py - area.y) * stride +
                  static_cast<usize>(px - area.x) * 4u;
    CHECK_EQ(p[3], u8{255});
    CHECK(p[0] > 200);
    CHECK(p[1] < 60);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. stroke ↔ sigan: 파이프라인이 낸 샘플이 저널에 구멍 없이 남는다.
// ─────────────────────────────────────────────────────────────────────────────
MARI_TEST(pipeline_samples_reach_sigan_journal) {
    const std::string journal = tempPath("journal") + ".mjr";
    std::remove(journal.c_str());

    sigan::PublisherConfig cfg;
    cfg.journalPath = journal;
    auto pubR = sigan::SiganPublisher::open(cfg);
    CHECK(pubR.ok());
    auto pub = std::move(pubR).value();

    auto tree = makeLayerTree(Size{256, 256}).value();
    LayerPtr layer = tree->addRaster("그림").value();
    auto engine = brush::createEngine(stroke::kNativeEngineName).value();
    CHECK(engine->setPreset(roundPreset(12.0f, 0.2f)).ok());

    stroke::StrokePipeline pipe(engine.get());
    brush::StrokeContext ctx;
    ctx.target = layer->tiles();
    ctx.color = Color8{0, 0, 255, 255};
    ctx.layerId = layer->id();

    const auto events = lineEvents(20.0, 30.0, 220.0, 190.0, 25);
    CHECK(pipe.begin(ctx, events.front()).ok());

    u64 lastSeq = 0;
    usize published = 0;
    // 사용자 이벤트 하나 = Sigan 프레임 하나. 보간으로 늘어난 스탬프는 정본이 아니다.
    for (usize i = 0; i < events.size(); ++i) {
        if (i == 0) {
            // begin() 이 이미 먹었다.
        } else if (i + 1 == events.size()) {
            pipe.end(events[i]);
        } else {
            pipe.extend(events[i]);
        }
        const stroke::InputSample& s = pipe.lastSample();
        sigan::StrokeSample fs;
        fs.pos = s.pos;
        fs.pressure = s.pressure;
        fs.tiltX = s.tiltX;
        fs.tiltY = s.tiltY;
        fs.rotation = s.rotationDeg;
        fs.velocity = s.velocity;
        fs.layerId = layer->id();
        const u64 seq = pub->publish(fs);
        CHECK(seq > lastSeq); // seq 는 절대 되돌아가지 않는다
        lastSeq = seq;
        ++published;
    }
    pub->endStroke();
    CHECK_EQ(pub->stats().published, static_cast<u64>(published));
    // 싱크가 없으면 로컬 모드다 — 전부 저널로 간다. 버린 프레임은 없다.
    CHECK_EQ(pub->stats().published, pub->stats().sent + pub->stats().spooled);
    pub.reset();

    auto scanR = sigan::Journal::scan(journal);
    CHECK(scanR.ok());
    const sigan::JournalScan& scan = scanR.value();
    CHECK(!scan.truncated);
    CHECK_EQ(scan.frames.size(), published);
    CHECK(scan.seqGaps().empty());         // 구멍이 없다
    CHECK_EQ(scan.completeCount, published); // 획이 끝까지 기록됐다
    CHECK_EQ(scan.lastCompleteSeq, lastSeq);

    // 정본 좌표가 실제로 살아 있다: 마지막 프레임은 마지막 이벤트 자리다.
    const sigan::StrokeFrame& last = scan.frames.back();
    CHECK_NEAR(static_cast<f64>(last.cx), 220.0, 1.0);
    CHECK_NEAR(static_cast<f64>(last.cy), 190.0, 1.0);

    std::remove(journal.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. core ↔ io/ora: 그린 것이 .ora 왕복에서 살아남는다. 희소성도 같이.
// ─────────────────────────────────────────────────────────────────────────────
MARI_TEST(painted_document_survives_ora_roundtrip) {
    auto tree = makeLayerTree(Size{512, 512}).value();
    LayerPtr bg = tree->addRaster("배경").value();
    LayerPtr fg = tree->addRaster("선").value();
    fg->setOpacity(0.75f);
    fg->setBlendMode(BlendMode::Multiply);

    auto engine = brush::createEngine(stroke::kNativeEngineName).value();
    CHECK(engine->setPreset(roundPreset(24.0f, 0.15f)).ok());

    stroke::StrokePipeline pipe(engine.get());
    brush::StrokeContext ctx;
    ctx.target = fg->tiles();
    ctx.color = Color8{0, 128, 255, 255};
    ctx.layerId = fg->id();

    const auto events = lineEvents(64.0, 64.0, 448.0, 448.0, 33);
    CHECK(pipe.begin(ctx, events.front()).ok());
    for (usize i = 1; i + 1 < events.size(); ++i)
        pipe.extend(events[i]);
    pipe.end(events.back());

    const usize tilesBefore = fg->tiles()->tileCount();
    CHECK(tilesBefore > 0);
    const Color8 before = peek(*fg->tiles(), 256, 256);
    CHECK_EQ(before.a, u8{255});

    ora::SaveOptions opts;
    opts.proofLog = std::string("{\"signed\":false}");
    auto bytesR = ora::saveToMemory(*tree, opts);
    CHECK(bytesR.ok());
    std::vector<u8> bytes = std::move(bytesR).value();
    CHECK(bytes.size() > 100);

    auto docR = ora::loadFromMemory(std::move(bytes));
    CHECK(docR.ok());
    ora::Document doc = std::move(docR).value();
    for (const std::string& w : doc.warnings)
        std::fprintf(stderr, "  [ora warning] %s\n", w.c_str());
    CHECK(doc.warnings.empty());
    CHECK(doc.tree != nullptr);
    CHECK_EQ(doc.tree->canvasSize().width, tree->canvasSize().width);
    CHECK_EQ(doc.tree->roots().size(), usize{2});

    // 순서 규약: 인덱스 0 = 가장 아래. .ora 는 반대라 양쪽에서 뒤집는다 —
    // 뒤집기가 한쪽만 되어 있으면 여기서 레이어가 뒤바뀐 채로 잡힌다.
    CHECK_EQ(doc.tree->roots()[0]->name(), std::string("배경"));
    CHECK_EQ(doc.tree->roots()[1]->name(), std::string("선"));

    const LayerPtr loadedFg = doc.tree->roots()[1];
    CHECK_NEAR(loadedFg->opacity(), 0.75f, 0.01f);
    CHECK_EQ(static_cast<int>(loadedFg->blendMode()), static_cast<int>(BlendMode::Multiply));

    // 픽셀이 살아 있다.
    const Color8 after = peek(*loadedFg->tiles(), 256, 256);
    CHECK_EQ(after.a, before.a);
    CHECK_EQ(after.r, before.r);
    CHECK_EQ(after.g, before.g);
    CHECK_EQ(after.b, before.b);

    // 희소성이 왕복에서 유지된다 — 빈 곳에 타일이 생기지 않는다.
    CHECK(loadedFg->tiles()->tileCount() <= tilesBefore + 4);
    CHECK_EQ(doc.tree->roots()[0]->tiles()->tileCount(), usize{0});

    // 과정 로그는 통로만 낸다. Mari 는 내용을 만들지도 검증하지도 않는다.
    CHECK(doc.proofLog.has_value());
    CHECK_EQ(*doc.proofLog, std::string("{\"signed\":false}"));
}

MARI_TEST_MAIN()
