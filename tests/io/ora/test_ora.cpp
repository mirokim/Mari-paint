// Mari Paint — .ora 저장/열기 테스트.
//
// 확인하는 것:
//   · 그룹 중첩을 포함한 레이어 트리의 구조·픽셀 왕복
//   · .ora 순서 규약(첫 자식 = 맨 위)을 지켜 쓰고 되읽는다
//   · BlendMode ↔ composite-op 매핑 왕복
//   · mari/prooflog.json 통로 (docs/03 6절)
//   · 깨진 입력에서 크래시 없이 오류를 돌려준다
#include <mari/ora/composite_op.hpp>
#include <mari/ora/image.hpp>
#include <mari/ora/ora.hpp>
#include <mari/ora/xml.hpp>
#include <mari/ora/zip.hpp>
#include <mari/test/harness.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mari;
using namespace mari::ora;

namespace {

/// 캔버스 좌표 rect 를 단색으로 칠한다. 스트로크 엔진 없이 픽셀을 만들기 위한 것.
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

/// 두 타일맵의 픽셀이 같은지. 두 경계 상자의 합집합 전체를 비교한다
/// (한쪽에만 타일이 있으면 그쪽이 투명인지까지 본다).
bool samePixels(const TileMap& a, const TileMap& b) {
    const Rect area = a.bounds().united(b.bounds());
    if (area.isEmpty())
        return true;
    auto ia = readRegion(a, area);
    auto ib = readRegion(b, area);
    return ia.ok() && ib.ok() && ia.value().pixels == ib.value().pixels;
}

/// 레이어 트리 두 벌이 구조·속성·픽셀까지 같은지 재귀 비교.
bool sameTree(const std::vector<LayerPtr>& a, const std::vector<LayerPtr>& b,
              mari::test::Context& ctx, const char* file, int line) {
    if (a.size() != b.size()) {
        ctx.fail(file, line, "자식 수가 다르다");
        return false;
    }
    bool ok = true;
    for (usize i = 0; i < a.size(); ++i) {
        const Layer& x = *a[i];
        const Layer& y = *b[i];
        if (x.name() != y.name()) {
            ctx.fail(file, line, "이름이 다르다: " + x.name() + " vs " + y.name());
            ok = false;
        }
        if (x.kind() != y.kind()) {
            ctx.fail(file, line, "종류가 다르다: " + x.name());
            ok = false;
        }
        if (x.blendMode() != y.blendMode()) {
            ctx.fail(file, line, "블렌드 모드가 다르다: " + x.name());
            ok = false;
        }
        if (x.visible() != y.visible() || x.locked() != y.locked() ||
            x.alphaLocked() != y.alphaLocked()) {
            ctx.fail(file, line, "플래그가 다르다: " + x.name());
            ok = false;
        }
        if (std::fabs(x.opacity() - y.opacity()) > 1e-4f) {
            ctx.fail(file, line, "불투명도가 다르다: " + x.name());
            ok = false;
        }
        if (x.tiles() != nullptr && y.tiles() != nullptr && !samePixels(*x.tiles(), *y.tiles())) {
            ctx.fail(file, line, "픽셀이 다르다: " + x.name());
            ok = false;
        }
        if (!sameTree(x.children(), y.children(), ctx, file, line))
            ok = false;
    }
    return ok;
}

#define CHECK_TREE(a, b) sameTree((a), (b), mari_ctx, __FILE__, __LINE__)

/// 테스트용 문서: 배경 + 그룹(그 안에 중첩 그룹) + 위쪽 레이어.
LayerTreePtr makeSample() {
    auto made = makeLayerTree(Size{256, 192});
    LayerTreePtr tree = made.value();

    auto bg = tree->addRaster("배경").value();
    fill(*bg, Rect{0, 0, 256, 192}, Color8::rgba(240, 230, 210, 255));

    auto group = tree->addGroup("그룹 하나").value();
    group->setOpacity(0.75f);
    group->setBlendMode(BlendMode::Multiply);

    auto inner = tree->addRaster("안쪽 선", group->id()).value();
    fill(*inner, Rect{30, 40, 70, 50}, Color8::rgba(20, 30, 200, 128));

    auto nested = tree->addGroup("중첩 그룹", group->id()).value();
    nested->setVisible(false);
    auto deep = tree->addRaster("깊은 곳", nested->id()).value();
    fill(*deep, Rect{130, 100, 64, 64}, Color8::rgba(200, 10, 10, 255));

    auto top = tree->addRaster("맨 위").value();
    top->setOpacity(0.35f);
    top->setBlendMode(BlendMode::Screen);
    top->setAlphaLocked(true);
    top->setLocked(true);
    fill(*top, Rect{-20, -10, 80, 80}, Color8::rgba(0, 255, 0, 200)); // 캔버스 밖으로 나간다

    (void)tree->setActiveLayer(inner->id());
    return tree;
}

} // namespace

MARI_TEST(composite_op_매핑_왕복) {
    // BlendMode 전부가 왕복해야 한다. 하나라도 빠지면 저장할 때 조용히 normal 이 된다.
    for (int v = 0; v <= static_cast<int>(BlendMode::Erase); ++v) {
        const BlendMode mode = static_cast<BlendMode>(v);
        const std::string op(compositeOpName(mode));
        const auto back = blendModeFromCompositeOp(op);
        CHECK(back.has_value());
        if (back.has_value())
            CHECK_EQ(static_cast<int>(*back), v);
    }
    // 스펙이 필수로 요구하는 이름.
    CHECK_EQ(std::string(compositeOpName(BlendMode::Normal)), std::string("svg:src-over"));
    // 다른 툴이 쓰는 별칭도 받아준다.
    CHECK_EQ(static_cast<int>(*blendModeFromCompositeOp("normal")),
             static_cast<int>(BlendMode::Normal));
    CHECK_EQ(static_cast<int>(*blendModeFromCompositeOp("svg:add")),
             static_cast<int>(BlendMode::Add));
    CHECK_EQ(static_cast<int>(*blendModeFromCompositeOp("krita:subtract")),
             static_cast<int>(BlendMode::Subtract));
    CHECK_EQ(static_cast<int>(*blendModeFromCompositeOp("")), static_cast<int>(BlendMode::Normal));
    // 모르는 건 조용히 normal 로 만들지 않고 nullopt 로 알린다.
    CHECK(!blendModeFromCompositeOp("svg:그런거없다").has_value());
}

MARI_TEST(레이어_트리_왕복) {
    const LayerTreePtr src = makeSample();
    auto saved = saveToMemory(*src);
    CHECK(saved.ok());
    if (!saved.ok())
        return;

    auto doc = loadFromMemory(saved.value());
    CHECK(doc.ok());
    if (!doc.ok()) {
        CHECK_FAIL(doc.message());
        return;
    }
    CHECK(doc.value().warnings.empty());
    CHECK_EQ(doc.value().tree->canvasSize().width, 256);
    CHECK_EQ(doc.value().tree->canvasSize().height, 192);
    CHECK_TREE(src->roots(), doc.value().tree->roots());

    // 활성 레이어(selected)도 살아 있어야 한다.
    const LayerPtr active = doc.value().tree->find(doc.value().tree->activeLayer());
    CHECK(active != nullptr);
    if (active)
        CHECK_EQ(active->name(), std::string("안쪽 선"));
}

MARI_TEST(모든_블렌드_모드가_파일을_왕복한다) {
    auto tree = makeLayerTree(Size{64, 64}).value();
    for (int v = 0; v <= static_cast<int>(BlendMode::Erase); ++v) {
        auto l = tree->addRaster("모드 " + std::to_string(v)).value();
        l->setBlendMode(static_cast<BlendMode>(v));
        fill(*l, Rect{0, 0, 8, 8}, Color8::rgba(static_cast<u8>(v * 10), 0, 0, 255));
    }
    auto saved = saveToMemory(*tree);
    CHECK(saved.ok());
    auto doc = loadFromMemory(std::move(saved).value());
    CHECK(doc.ok());
    if (!doc.ok())
        return;
    CHECK(doc.value().warnings.empty());
    const std::vector<LayerPtr>& got = doc.value().tree->roots();
    CHECK_EQ(got.size(), static_cast<usize>(static_cast<int>(BlendMode::Erase) + 1));
    for (usize i = 0; i < got.size(); ++i)
        CHECK_EQ(static_cast<int>(got[i]->blendMode()), static_cast<int>(i));
}

MARI_TEST(아카이브_구성이_스펙대로다) {
    const LayerTreePtr src = makeSample();
    auto saved = saveToMemory(*src);
    CHECK(saved.ok());
    auto opened = ZipReader::open(std::move(saved).value());
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    const ZipReader& z = opened.value();

    CHECK_EQ(z.entries()[0].name, std::string(kMimeTypeEntry));
    CHECK(z.contains(kStackEntry));
    CHECK(z.contains(kMergedEntry));
    CHECK(z.contains(kThumbnailEntry));
    CHECK(!z.contains(kProofLogEntry)); // 안 넣었으면 없다

    // 썸네일은 256×256 을 넘지 않는다.
    auto thumbBytes = z.read(kThumbnailEntry);
    CHECK(thumbBytes.ok());
    if (thumbBytes.ok()) {
        auto thumb = decodePng(thumbBytes.value().data(), thumbBytes.value().size());
        CHECK(thumb.ok());
        if (thumb.ok()) {
            CHECK(thumb.value().width <= kThumbnailMaxSide);
            CHECK(thumb.value().height <= kThumbnailMaxSide);
            CHECK(thumb.value().width > 0);
        }
    }

    // mergedimage 는 캔버스 크기 그대로다.
    auto mergedBytes = z.read(kMergedEntry);
    CHECK(mergedBytes.ok());
    if (mergedBytes.ok()) {
        auto merged = decodePng(mergedBytes.value().data(), mergedBytes.value().size());
        CHECK(merged.ok());
        if (merged.ok()) {
            CHECK_EQ(merged.value().width, 256);
            CHECK_EQ(merged.value().height, 192);
        }
    }

    // stack.xml 은 파싱되고, 첫 자식이 **맨 위** 레이어여야 한다.
    auto xmlBytes = z.read(kStackEntry);
    CHECK(xmlBytes.ok());
    auto root = parseXml(std::string_view(
        reinterpret_cast<const char*>(xmlBytes.value().data()), xmlBytes.value().size()));
    CHECK(root.ok());
    if (root.ok()) {
        CHECK_EQ(root.value().name, std::string("image"));
        CHECK_EQ(root.value().attrOr("w", ""), std::string("256"));
        const XmlNode& stack = root.value().children.at(0);
        CHECK_EQ(stack.name, std::string("stack"));
        CHECK_EQ(stack.children.at(0).attrOr("name", ""), std::string("맨 위"));
        CHECK_EQ(stack.children.at(2).attrOr("name", ""), std::string("배경"));
    }
}

MARI_TEST(prooflog_통로) {
    // docs/03 6절: Sigan 미설치 시의 로컬 무서명 로그. Mari 는 내용을 만들지도
    // 검증하지도 않는다 — 넣고 빼는 통로만 낸다.
    const std::string log = R"({"schema":"mari.prooflog.v1","signed":false,"events":[]})";
    const LayerTreePtr src = makeSample();

    SaveOptions opts;
    opts.proofLog = log;
    auto saved = saveToMemory(*src, opts);
    CHECK(saved.ok());
    const std::vector<u8> bytes = std::move(saved).value();

    auto opened = ZipReader::open(bytes);
    CHECK(opened.ok());
    CHECK(opened.value().contains(kProofLogEntry));

    auto doc = loadFromMemory(bytes);
    CHECK(doc.ok());
    if (doc.ok()) {
        CHECK(doc.value().proofLog.has_value());
        if (doc.value().proofLog)
            CHECK_EQ(*doc.value().proofLog, log);
    }

    // 로그를 안 읽겠다고 하면 안 읽는다.
    LoadOptions noLog;
    noLog.loadProofLog = false;
    auto doc2 = loadFromMemory(bytes, noLog);
    CHECK(doc2.ok());
    if (doc2.ok())
        CHECK(!doc2.value().proofLog.has_value());
}

MARI_TEST(파일_왕복과_prooflog_단독_읽기) {
    const std::string path = "mari_ora_roundtrip_test.ora";
    const std::string log = R"({"schema":"mari.prooflog.v1","signed":false})";
    const LayerTreePtr src = makeSample();

    SaveOptions opts;
    opts.proofLog = log;
    CHECK(save(*src, path, opts).ok());

    auto doc = load(path);
    CHECK(doc.ok());
    if (doc.ok())
        CHECK_TREE(src->roots(), doc.value().tree->roots());

    auto onlyLog = readProofLog(path);
    CHECK(onlyLog.ok());
    if (onlyLog.ok()) {
        CHECK(onlyLog.value().has_value());
        if (onlyLog.value())
            CHECK_EQ(*onlyLog.value(), log);
    }
    std::remove(path.c_str());

    auto missingFile = load("없는파일이다.ora");
    CHECK(!missingFile.ok());
    CHECK_EQ(missingFile.code(), ErrorCode::IoError);
}

MARI_TEST(빈_문서와_빈_레이어) {
    auto tree = makeLayerTree(Size{32, 32}).value();
    (void)tree->addRaster("아무것도 안 그린 레이어");
    auto saved = saveToMemory(*tree);
    CHECK(saved.ok());
    auto doc = loadFromMemory(std::move(saved).value());
    CHECK(doc.ok());
    if (doc.ok()) {
        CHECK_EQ(doc.value().tree->roots().size(), usize{1});
        CHECK_EQ(doc.value().tree->roots()[0]->tiles()->tileCount(), usize{0});
    }
}

MARI_TEST(깨진_입력은_오류로_돌아온다) {
    // 1) ZIP 조차 아니다
    auto notZip = loadFromMemory(std::vector<u8>(500, u8{0x7E}));
    CHECK(!notZip.ok());

    // 2) ZIP 이지만 mimetype 이 없다
    {
        ZipWriter w;
        CHECK(w.add("stack.xml", "<image w=\"1\" h=\"1\"><stack/></image>", ZipMethod::Deflate)
                  .ok());
        auto z = w.finish();
        auto doc = loadFromMemory(std::move(z).value());
        CHECK(!doc.ok());
        CHECK_EQ(doc.code(), ErrorCode::ParseError);
    }
    // 3) mimetype 은 맞는데 stack.xml 이 없다
    {
        ZipWriter w;
        CHECK(w.beginOra().ok());
        auto z = w.finish();
        auto doc = loadFromMemory(std::move(z).value());
        CHECK(!doc.ok());
        CHECK_EQ(doc.code(), ErrorCode::ParseError);
        CHECK(doc.message().find("stack.xml") != std::string::npos);
    }
    // 4) stack.xml 이 XML 로 깨졌다
    {
        ZipWriter w;
        CHECK(w.beginOra().ok());
        CHECK(w.add("stack.xml", "<image><stack>", ZipMethod::Deflate).ok());
        auto z = w.finish();
        auto doc = loadFromMemory(std::move(z).value());
        CHECK(!doc.ok());
        CHECK_EQ(doc.code(), ErrorCode::ParseError);
    }
    // 5) 루트가 <image> 가 아니다
    {
        ZipWriter w;
        CHECK(w.beginOra().ok());
        CHECK(w.add("stack.xml", "<svg/>", ZipMethod::Deflate).ok());
        auto z = w.finish();
        CHECK(!loadFromMemory(std::move(z).value()).ok());
    }
    // 6) 레이어가 가리키는 PNG 가 없다 → 크래시도 실패도 아니고 **경고**다
    {
        ZipWriter w;
        CHECK(w.beginOra().ok());
        CHECK(w.add("stack.xml",
                    "<image w=\"8\" h=\"8\"><stack>"
                    "<layer name=\"없는그림\" src=\"data/nope.png\" x=\"0\" y=\"0\"/>"
                    "</stack></image>",
                    ZipMethod::Deflate)
                  .ok());
        auto z = w.finish();
        auto doc = loadFromMemory(std::move(z).value());
        CHECK(doc.ok());
        if (doc.ok()) {
            CHECK_EQ(doc.value().tree->roots().size(), usize{1});
            CHECK_EQ(doc.value().warnings.size(), usize{1});
        }
    }
    // 7) 모르는 composite-op → normal 로 떨어뜨리고 **경고를 남긴다**
    {
        ZipWriter w;
        CHECK(w.beginOra().ok());
        CHECK(w.add("stack.xml",
                    "<image w=\"8\" h=\"8\"><stack>"
                    "<layer name=\"이상한모드\" composite-op=\"svg:없는연산\"/>"
                    "</stack></image>",
                    ZipMethod::Deflate)
                  .ok());
        auto z = w.finish();
        auto doc = loadFromMemory(std::move(z).value());
        CHECK(doc.ok());
        if (doc.ok()) {
            CHECK_EQ(static_cast<int>(doc.value().tree->roots()[0]->blendMode()),
                     static_cast<int>(BlendMode::Normal));
            CHECK(!doc.value().warnings.empty());
        }
    }
    // 8) PNG 가 깨졌다 → 빈 레이어 + 경고
    {
        ZipWriter w;
        CHECK(w.beginOra().ok());
        CHECK(w.add("stack.xml",
                    "<image w=\"8\" h=\"8\"><stack>"
                    "<layer name=\"깨진그림\" src=\"data/bad.png\"/></stack></image>",
                    ZipMethod::Deflate)
                  .ok());
        const std::vector<u8> junk(64, u8{0x11});
        CHECK(w.add("data/bad.png", junk.data(), junk.size(), ZipMethod::Store).ok());
        auto z = w.finish();
        auto doc = loadFromMemory(std::move(z).value());
        CHECK(doc.ok());
        if (doc.ok())
            CHECK(!doc.value().warnings.empty());
    }
}

MARI_TEST(png_코덱_왕복) {
    Image8 img = Image8::make(37, 19);
    for (i32 y = 0; y < img.height; ++y)
        for (i32 x = 0; x < img.width; ++x) {
            u8* p = img.pixels.data() + static_cast<usize>(y) * img.stride() +
                    static_cast<usize>(x) * 4u;
            p[0] = static_cast<u8>(x * 7);
            p[1] = static_cast<u8>(y * 13);
            p[2] = static_cast<u8>(x ^ y);
            p[3] = static_cast<u8>((x + y) % 256);
        }
    auto png = encodePng(img, 6);
    CHECK(png.ok());
    auto back = decodePng(png.value().data(), png.value().size());
    CHECK(back.ok());
    if (back.ok()) {
        CHECK_EQ(back.value().width, img.width);
        CHECK_EQ(back.value().height, img.height);
        CHECK(back.value().pixels == img.pixels);
    }
    // 쓰레기 입력에도 던지지 않는다.
    const std::vector<u8> junk(100, u8{0x42});
    CHECK(!decodePng(junk.data(), junk.size()).ok());
    CHECK(!decodePng(nullptr, 0).ok());
}

MARI_TEST_MAIN()
