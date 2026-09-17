// Mari Paint — 큰 캔버스(4096×4096) 저장/열기.
//
// 여기서 지켜보는 건 두 가지다.
//   1) 4096² 캔버스가 실제로 왕복한다 (Zip64 없이도 크기가 맞는다)
//   2) 희소성이 유지된다 — 큰 캔버스에 점 몇 개만 찍으면 타일도 몇 개만 잡힌다
//      (docs/02 3절. 이게 깨지면 "가볍다"가 무너진다)
#include <mari/ora/image.hpp>
#include <mari/ora/ora.hpp>
#include <mari/test/harness.hpp>

#include <string>
#include <vector>

using namespace mari;
using namespace mari::ora;

namespace {

void fill(Layer& layer, const Rect& r, Color8 c) {
    Image8 img = Image8::make(r.width, r.height);
    for (usize i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i + 0] = c.r;
        img.pixels[i + 1] = c.g;
        img.pixels[i + 2] = c.b;
        img.pixels[i + 3] = c.a;
    }
    (void)writeRegion(*layer.tiles(), r, img);
}

constexpr i32 kSide = 4096;

} // namespace

MARI_TEST(큰_캔버스_왕복) {
    auto tree = makeLayerTree(Size{kSide, kSide}).value();

    // 구석 두 군데와 한가운데에만 칠한다. 타일은 그 주변만 잡혀야 한다.
    auto sparse = tree->addRaster("희소 레이어").value();
    fill(*sparse, Rect{0, 0, 64, 64}, Color8::rgba(255, 0, 0, 255));
    fill(*sparse, Rect{kSide - 64, kSide - 64, 64, 64}, Color8::rgba(0, 0, 255, 255));
    fill(*sparse, Rect{2048, 2048, 128, 128}, Color8::rgba(0, 255, 0, 200));
    // 4096² 을 꽉 채우면 4096 타일이다. 우리는 그 근처도 가면 안 된다.
    CHECK(sparse->tiles()->tileCount() <= 8);

    // 큰 캔버스에서는 mergedimage 인코딩이 제일 비싸다. 압축 강도를 낮춰 둔다.
    SaveOptions opts;
    opts.compressionLevel = 1;
    auto saved = saveToMemory(*tree, opts);
    CHECK(saved.ok());
    if (!saved.ok()) {
        CHECK_FAIL(saved.message());
        return;
    }

    auto doc = loadFromMemory(std::move(saved).value());
    CHECK(doc.ok());
    if (!doc.ok()) {
        CHECK_FAIL(doc.message());
        return;
    }
    CHECK_EQ(doc.value().tree->canvasSize().width, kSide);
    CHECK_EQ(doc.value().tree->canvasSize().height, kSide);
    CHECK_EQ(doc.value().tree->roots().size(), usize{1});

    // 읽어들인 쪽도 희소해야 한다. 레이어 PNG 는 경계 상자 하나라서
    // 구석 ~ 구석을 덮는 큰 이미지지만, 투명한 부분은 타일을 만들지 않는다.
    const TileMap* got = doc.value().tree->roots()[0]->tiles();
    CHECK(got != nullptr);
    if (got == nullptr)
        return;
    CHECK(got->tileCount() <= 8);

    // 칠한 세 군데의 픽셀이 그대로인지 본다.
    struct Probe {
        Rect area;
        Color8 color;
    };
    const Probe probes[] = {
        {Rect{0, 0, 64, 64}, Color8::rgba(255, 0, 0, 255)},
        {Rect{kSide - 64, kSide - 64, 64, 64}, Color8::rgba(0, 0, 255, 255)},
        {Rect{2048, 2048, 128, 128}, Color8::rgba(0, 255, 0, 200)},
    };
    for (const Probe& p : probes) {
        auto img = readRegion(*got, p.area);
        CHECK(img.ok());
        if (!img.ok())
            continue;
        bool same = true;
        for (usize i = 0; i < img.value().pixels.size(); i += 4) {
            const u8* q = img.value().pixels.data() + i;
            if (q[0] != p.color.r || q[1] != p.color.g || q[2] != p.color.b || q[3] != p.color.a) {
                same = false;
                break;
            }
        }
        CHECK(same);
    }

    // 칠하지 않은 곳은 완전 투명이어야 한다.
    auto blank = readRegion(*got, Rect{1024, 512, 64, 64});
    CHECK(blank.ok());
    if (blank.ok()) {
        bool clear = true;
        for (u8 v : blank.value().pixels)
            if (v != 0) {
                clear = false;
                break;
            }
        CHECK(clear);
    }
}

MARI_TEST_MAIN()
