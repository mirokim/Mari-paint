// Mari Paint — 브리지 구현체 검증 (docs/04 2절이 "구현체가 리포 어디에도 없다"고 쓴 그 구멍)
//
// 여기서 지키려는 것:
//   · 브리지가 **실제** 문서/레이어를 조작한다 — 가짜가 아니라 core 의 진짜 타입이다.
//   · 해시가 브리지를 통해서도 같은 값을 낸다(crypto 직접 호출과 일치).
//   · 이벤트가 실제로 나간다 — 특히 🔴 `OnPaste(Script)`.
//   · **Win32 비의존** — 이 테스트가 Linux 에서 도는 것 자체가 그 증명이고,
//     헤더 전수 스캔으로 한 번 더 못 박는다.
#include <mari/app/application.hpp>
#include <mari/app/document.hpp>
#include <mari/crypto/canvas_hash.hpp>
#include <mari/crypto/sha256.hpp>
#include <mari/test/harness.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace mari;
using namespace mari::app;

namespace {

/// 받은 이벤트를 전부 적어 두는 리스너. COM 브로드캐스터가 앉을 자리에 대신 앉는다.
class RecordingListener final : public IAppEventListener {
public:
    struct Paste {
        PasteSource source = PasteSource::Unknown;
    };

    void onDocumentOpened(const std::string& path, const std::string& fileHash, i32 w,
                          i32 h) override {
        openedPath = path;
        openedHash = fileHash;
        openedSize = Size{w, h};
        ++opened;
    }
    void onDocumentSaved(const std::string& path, const std::string& fileHash,
                         i64 sizeBytes) override {
        savedPath = path;
        savedHash = fileHash;
        savedBytes = sizeBytes;
        ++saved;
    }
    void onCanvasSnapshot(const std::string& canvasHash) override {
        snapshotHash = canvasHash;
        ++snapshots;
    }
    void onViewChanged(f64 zoom, f64 rotationDeg) override {
        lastZoom = zoom;
        lastRotation = rotationDeg;
        ++viewChanges;
    }
    void onPaste(PasteSource source) override { pastes.push_back(Paste{source}); }
    void onUndo(i32 steps) override { undoSteps.push_back(steps); }
    void onLayerChanged(LayerId layerId, const std::string& changeKind) override {
        layerChanges.push_back(changeKind);
        lastLayer = layerId;
    }

    int opened = 0;
    int saved = 0;
    int snapshots = 0;
    int viewChanges = 0;
    std::string openedPath, openedHash, savedPath, savedHash, snapshotHash;
    Size openedSize{};
    i64 savedBytes = 0;
    f64 lastZoom = 0.0, lastRotation = 0.0;
    std::vector<Paste> pastes;
    std::vector<i32> undoSteps;
    std::vector<std::string> layerChanges;
    LayerId lastLayer = kInvalidLayerId;
};

/// w*h 짜리 단색 RGBA8 버퍼.
std::vector<u8> solid(i32 w, i32 h, Color8 c) {
    std::vector<u8> px(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
    for (usize i = 0; i < px.size(); i += 4) {
        px[i] = c.r;
        px[i + 1] = c.g;
        px[i + 2] = c.b;
        px[i + 3] = c.a;
    }
    return px;
}

std::string readWholeFile(const std::filesystem::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr) {
        return std::string{};
    }
    std::string out;
    char buf[4096];
    usize got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, got);
    }
    (void)std::fclose(f);
    return out;
}

std::filesystem::path repoRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

} // namespace

// ── 문서/레이어를 실제로 조작한다 ────────────────────────────────────────

MARI_TEST(application_creates_a_real_document) {
    Application app;
    RecordingListener rec;
    app.events().addListener(&rec);

    CHECK_EQ(app.documentCount(), static_cast<usize>(0));
    CHECK(app.activeDocument() == nullptr);

    const Result<IDocumentBridge*> doc = app.createDocument(300, 200);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();
    CHECK_EQ(app.documentCount(), static_cast<usize>(1));
    CHECK(app.activeDocument() == d);
    CHECK(app.documentAt(0) == d);
    CHECK(app.documentAt(1) == nullptr);

    CHECK_EQ(d->canvasSize().width, 300);
    CHECK_EQ(d->canvasSize().height, 200);
    CHECK(d->fullPath().empty());
    CHECK(!d->isSaved());
    // 🔴 가짜 트리가 아니라 core 의 진짜 LayerTree 다. 레이어 한 장이 깔려 있다.
    CHECK_EQ(d->layers().roots().size(), static_cast<usize>(1));
    CHECK(d->layers().activeLayer() != kInvalidLayerId);

    CHECK_EQ(app.version(), std::string("0.1.0"));
    CHECK(!app.visible());
    app.setVisible(true);
    CHECK(app.visible());
}

MARI_TEST(import_pixels_reports_paste_script) {
    // 🔴 붓으로 그리지 않은 픽셀이 들어오면 반드시 보고한다(docs/03 2절 정신,
    //    app_bridge.hpp 의 importPixels 주석이 요구하는 것).
    Application app;
    RecordingListener rec;
    app.events().addListener(&rec);
    const Result<IDocumentBridge*> doc = app.createDocument(64, 64);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();

    const std::vector<u8> px = solid(20, 10, Color8::rgba(10, 200, 30));
    const Result<LayerId> id = d->importPixels("가져옴", px.data(), px.size(), 20, 10);
    CHECK(id.ok());
    if (!id.ok()) {
        return;
    }
    CHECK(d->layers().find(id.value()) != nullptr);
    CHECK_EQ(d->layers().roots().size(), static_cast<usize>(2));

    CHECK_EQ(rec.pastes.size(), static_cast<usize>(1));
    if (!rec.pastes.empty()) {
        CHECK(rec.pastes[0].source == PasteSource::Script);
        CHECK_EQ(std::string(pasteSourceName(rec.pastes[0].source)), std::string("script"));
    }
    CHECK(!d->isSaved());
}

MARI_TEST(import_pixels_rejects_a_short_buffer) {
    // 버퍼가 모자라면 만든 레이어를 도로 치운다 — 반쯤 만들어진 상태를 남기지 않는다.
    Application app;
    const Result<IDocumentBridge*> doc = app.createDocument(64, 64);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();
    const usize before = d->layers().roots().size();

    const std::vector<u8> px = solid(4, 4, Color8::rgba(1, 2, 3));
    const Result<LayerId> id = d->importPixels("짧음", px.data(), px.size(), 40, 40);
    CHECK(!id.ok());
    CHECK_EQ(d->layers().roots().size(), before);
}

MARI_TEST(canvas_hash_moves_with_the_pixels) {
    Application app;
    const Result<IDocumentBridge*> doc = app.createDocument(80, 60);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();

    const Result<std::string> empty = d->canvasHash();
    CHECK(empty.ok());

    const std::vector<u8> px = solid(80, 60, Color8::rgba(255, 0, 0));
    const Result<LayerId> id = d->importPixels("빨강", px.data(), px.size(), 80, 60);
    CHECK(id.ok());

    const Result<std::string> filled = d->canvasHash();
    CHECK(filled.ok());
    if (empty.ok() && filled.ok()) {
        CHECK_NE(empty.value(), filled.value());
        // 브리지를 거친 값과 crypto 를 직접 부른 값이 같아야 한다.
        const Result<std::string> direct = crypto::canvasHashHex(d->layers());
        CHECK(direct.ok());
        if (direct.ok()) {
            CHECK_EQ(filled.value(), direct.value());
        }
    }

    // 레이어 해시도 브리지를 통해 같은 값을 낸다.
    if (id.ok()) {
        const Result<std::string> lh = d->layerHash(id.value());
        CHECK(lh.ok());
        const LayerPtr layer = d->layers().find(id.value());
        CHECK(layer != nullptr);
        if (lh.ok() && layer) {
            const Result<std::string> direct = crypto::layerHashHex(*layer);
            CHECK(direct.ok());
            if (direct.ok()) {
                CHECK_EQ(lh.value(), direct.value());
            }
        }
    }
    const Result<std::string> missing = d->layerHash(999999u);
    CHECK(!missing.ok());
}

MARI_TEST(undo_and_redo_move_the_canvas_hash_back_and_forth) {
    Application app;
    RecordingListener rec;
    app.events().addListener(&rec);
    const Result<IDocumentBridge*> doc = app.createDocument(70, 70);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();
    const LayerId base = d->layers().activeLayer();

    const Result<std::string> before = d->canvasHash();
    CHECK(before.ok());

    // 기존 레이어에 칠한다(레이어 생성이 아니라 픽셀만 바뀌는 경로).
    Document* impl = static_cast<Document*>(d);
    const std::vector<u8> px = solid(30, 30, Color8::rgba(0, 0, 255));
    const Result<void> paint =
        impl->paintPixels(base, Rect{5, 5, 30, 30}, px.data(), px.size(), "칠하기");
    CHECK(paint.ok());

    const Result<std::string> painted = d->canvasHash();
    CHECK(painted.ok());
    if (before.ok() && painted.ok()) {
        CHECK_NE(before.value(), painted.value());
    }

    CHECK(d->undo().ok());
    const Result<std::string> undone = d->canvasHash();
    CHECK(undone.ok());
    if (before.ok() && undone.ok()) {
        CHECK_EQ(before.value(), undone.value()); // 되돌아왔다
    }

    CHECK(d->redo().ok());
    const Result<std::string> redone = d->canvasHash();
    if (painted.ok() && redone.ok()) {
        CHECK_EQ(painted.value(), redone.value());
    }

    // 더 되돌릴 게 없으면 정직하게 실패한다.
    CHECK(d->undo().ok());
    CHECK(!d->undo().ok());

    // 이벤트: undo 는 +1, redo 는 -1 로 보고한다.
    CHECK(rec.undoSteps.size() >= 2);
    if (rec.undoSteps.size() >= 2) {
        CHECK_EQ(rec.undoSteps[0], 1);
        CHECK_EQ(rec.undoSteps[1], -1);
    }
}

MARI_TEST(export_composite_and_layer_pixels) {
    Application app;
    const Result<IDocumentBridge*> doc = app.createDocument(16, 8);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();
    Document* impl = static_cast<Document*>(d);

    const std::vector<u8> px = solid(4, 4, Color8::rgba(9, 8, 7));
    const Result<void> paint =
        impl->paintPixels(d->layers().activeLayer(), Rect{2, 1, 4, 4}, px.data(), px.size(), "칠");
    CHECK(paint.ok());

    std::vector<u8> flat;
    CHECK(d->exportComposite(flat).ok());
    CHECK_EQ(flat.size(), static_cast<usize>(16 * 8 * 4));
    // (2,1) 픽셀이 칠해져 있어야 한다.
    const usize at = (static_cast<usize>(1) * 16u + 2u) * 4u;
    CHECK_EQ(static_cast<int>(flat[at]), 9);
    CHECK_EQ(static_cast<int>(flat[at + 3]), 255);
    // (0,0) 은 비어 있다.
    CHECK_EQ(static_cast<int>(flat[3]), 0);

    std::vector<u8> layerPx;
    Rect area{};
    CHECK(d->exportLayerPixels(d->layers().activeLayer(), layerPx, area).ok());
    // 타일 단위 bounds 라 64 의 배수다 — 반올림하지 않고 실제 영역을 돌려준다.
    CHECK(!area.isEmpty());
    CHECK_EQ(layerPx.size(),
             static_cast<usize>(area.width) * static_cast<usize>(area.height) * 4u);
}

MARI_TEST(view_state_and_snapshot_events) {
    Application app;
    RecordingListener rec;
    app.events().addListener(&rec);
    const Result<IDocumentBridge*> doc = app.createDocument(32, 32);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    Document* impl = static_cast<Document*>(doc.value());

    ViewState v;
    v.zoom = 2.5;
    v.rotationDeg = 90.0;
    impl->setViewState(v);
    CHECK_EQ(rec.viewChanges, 1);
    CHECK_EQ(rec.lastZoom, 2.5);
    CHECK_EQ(rec.lastRotation, 90.0);
    CHECK_EQ(impl->viewState().zoom, 2.5);

    // 🔴 스냅샷은 해시를 **계산해서 보고**할 뿐이다. 봉인하지 않는다.
    const Result<std::string> snap = impl->takeCanvasSnapshot();
    CHECK(snap.ok());
    CHECK_EQ(rec.snapshots, 1);
    if (snap.ok()) {
        CHECK_EQ(rec.snapshotHash, snap.value());
        const Result<std::string> again = impl->canvasHash();
        CHECK(again.ok());
        if (again.ok()) {
            CHECK_EQ(again.value(), snap.value());
        }
    }
}

// ── 저장 / 열기 ──────────────────────────────────────────────────────────

MARI_TEST(save_then_open_round_trips_through_the_bridge) {
    const std::string path = "mari_test_bridge_roundtrip.ora";
    std::string savedCanvasHash;
    std::string fileHash;

    {
        Application app;
        RecordingListener rec;
        app.events().addListener(&rec);
        const Result<IDocumentBridge*> doc = app.createDocument(100, 50);
        CHECK(doc.ok());
        if (!doc.ok()) {
            return;
        }
        IDocumentBridge* d = doc.value();
        const std::vector<u8> px = solid(40, 20, Color8::rgba(3, 250, 120));
        CHECK(d->importPixels("색", px.data(), px.size(), 40, 20).ok());

        const Result<std::string> ch = d->canvasHash();
        CHECK(ch.ok());
        if (ch.ok()) {
            savedCanvasHash = ch.value();
        }

        CHECK(d->saveAs(path, "ora").ok());
        CHECK(d->isSaved());
        CHECK_EQ(d->fullPath(), path);
        CHECK_EQ(rec.saved, 1);
        CHECK_EQ(rec.savedPath, path);
        CHECK(rec.savedBytes > 0);

        // 🔴 저장 파일 해시는 파일 바이트 그대로 — sha256FileHex 와 같아야 한다.
        const Result<std::string> fh = crypto::sha256FileHex(path);
        CHECK(fh.ok());
        if (fh.ok()) {
            CHECK_EQ(rec.savedHash, fh.value());
            fileHash = fh.value();
        }

        // 저장 뒤 save() 는 같은 경로로 다시 쓴다.
        CHECK(d->save().ok());
    }

    {
        Application app;
        RecordingListener rec;
        app.events().addListener(&rec);
        const Result<IDocumentBridge*> doc = app.open(path);
        CHECK(doc.ok());
        if (!doc.ok()) {
            (void)std::remove(path.c_str());
            return;
        }
        IDocumentBridge* d = doc.value();
        CHECK(d->isSaved());
        CHECK_EQ(d->fullPath(), path);
        CHECK_EQ(d->canvasSize().width, 100);
        CHECK_EQ(d->canvasSize().height, 50);

        CHECK_EQ(rec.opened, 1);
        CHECK_EQ(rec.openedSize.width, 100);
        CHECK(!rec.openedHash.empty());
        CHECK_EQ(rec.openedHash, fileHash);

        // 🔴 .ora 왕복 뒤에도 캔버스 해시가 같다 — 해시 규약이 저장 포맷과 어긋나지 않는다.
        const Result<std::string> ch = d->canvasHash();
        CHECK(ch.ok());
        if (ch.ok()) {
            CHECK_EQ(ch.value(), savedCanvasHash);
        }
    }
    (void)std::remove(path.c_str());
}

MARI_TEST(unsupported_paths_fail_honestly) {
    Application app;
    const Result<IDocumentBridge*> doc = app.createDocument(8, 8);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();

    // .psd 쓰기는 없다. 있는 척하지 않는다(docs/04 3절).
    const Result<void> psd = d->saveAs("x.psd", "psd");
    CHECK(!psd.ok());
    if (!psd.ok()) {
        CHECK(psd.code() == ErrorCode::Unsupported);
    }
    const Result<void> unknown = d->saveAs("x.zzz", "zzz");
    CHECK(!unknown.ok());

    // 저장된 적 없는 문서의 save() 는 경로가 없다고 말한다.
    const Result<void> noPath = d->save();
    CHECK(!noPath.ok());

    // .psd 열기도 마찬가지.
    const Result<IDocumentBridge*> open = app.open("없는파일.psd");
    CHECK(!open.ok());
    if (!open.ok()) {
        CHECK(open.code() == ErrorCode::Unsupported);
    }

    // 8bf 격리 호스트는 이 빌드에 없다. 조용히 성공한 척하지 않는다.
    std::vector<std::string> notes;
    const Result<i32> filter = d->apply8bf("a.8bf", "Blur", 0, notes);
    CHECK(!filter.ok());
    if (!filter.ok()) {
        CHECK(filter.code() == ErrorCode::Unsupported);
    }
    CHECK(!notes.empty()); // 왜 안 됐는지 남긴다
}

MARI_TEST(save_as_png_does_not_claim_the_document_path) {
    const std::string path = "mari_test_bridge_flat.png";
    Application app;
    const Result<IDocumentBridge*> doc = app.createDocument(12, 9);
    CHECK(doc.ok());
    if (!doc.ok()) {
        return;
    }
    IDocumentBridge* d = doc.value();
    CHECK(d->saveAs(path, "png").ok());
    // png 는 레이어가 사라지는 내보내기다 — 문서의 정본 경로가 되면 안 된다.
    CHECK(d->fullPath().empty());
    CHECK(!d->isSaved());
    CHECK(std::filesystem::exists(path));
    (void)std::remove(path.c_str());
}

// ── 앱 수명 / Sigan 상태 ─────────────────────────────────────────────────

MARI_TEST(documents_close_and_quit_does_not_save) {
    Application app;
    const Result<IDocumentBridge*> a = app.createDocument(8, 8);
    const Result<IDocumentBridge*> b = app.createDocument(8, 8);
    CHECK(a.ok() && b.ok());
    if (!a.ok() || !b.ok()) {
        return;
    }
    CHECK_EQ(app.documentCount(), static_cast<usize>(2));
    CHECK(app.setActiveDocument(0).ok());
    CHECK(app.activeDocument() == a.value());
    CHECK(!app.setActiveDocument(5).ok());

    CHECK(app.closeDocument(a.value(), false).ok());
    CHECK_EQ(app.documentCount(), static_cast<usize>(1));
    CHECK(!app.closeDocument(a.value(), false).ok()); // 이미 없다

    // quit() 은 저장하지 않는다 — 스크립트가 조용히 덮어쓰면 안 된다.
    app.quit();
    CHECK(app.quitRequested());
    CHECK_EQ(app.documentCount(), static_cast<usize>(0));
    CHECK(app.activeDocument() == nullptr);
}

MARI_TEST(sigan_status_is_reported_not_toggled) {
    // docs/03 5.1 — 토글이 없다. 상태를 받아 적을 뿐이다.
    Application app;
    CHECK(!app.siganStatus().connected);
    CHECK_EQ(app.siganStatus().spooledFrames, static_cast<u64>(0));

    SiganStatus s;
    s.connected = true;
    s.spooledFrames = 17;
    s.lastSeq = 4242;
    app.setSiganStatus(s);
    CHECK(app.siganStatus().connected);
    CHECK_EQ(app.siganStatus().spooledFrames, static_cast<u64>(17));
    CHECK_EQ(app.siganStatus().lastSeq, static_cast<u64>(4242));
}

MARI_TEST(process_wide_bridge_slot_starts_empty) {
    CHECK(applicationBridge() == nullptr);
    Application app;
    setApplicationBridge(&app);
    CHECK(applicationBridge() == &app);
    setApplicationBridge(nullptr); // 반쯤 살아 있는 객체를 남기지 않는다
    CHECK(applicationBridge() == nullptr);
}

MARI_TEST(event_hub_does_not_double_register) {
    EventHub hub;
    RecordingListener a;
    hub.addListener(&a);
    hub.addListener(&a);
    hub.addListener(nullptr);
    CHECK_EQ(hub.listenerCount(), static_cast<usize>(1));
    hub.firePaste(PasteSource::Clipboard);
    CHECK_EQ(a.pastes.size(), static_cast<usize>(1));
    CHECK(hub.removeListener(&a));
    CHECK(!hub.removeListener(&a));
    hub.firePaste(PasteSource::Clipboard);
    CHECK_EQ(a.pastes.size(), static_cast<usize>(1));
}

// ── Win32 비의존 ─────────────────────────────────────────────────────────

MARI_TEST(app_headers_are_free_of_win32) {
    // 🔴 브리지 구현 자체는 플랫폼 중립이어야 한다. COM 래퍼만 Windows 전용이다.
    //    이 테스트가 Linux 에서 도는 것 자체가 증명이지만, 헤더에 Win32 가 스며드는
    //    순간 다시 docs/04 2절의 "컴파일조차 못 한 것"으로 굴러떨어지므로 못 박는다.
    const char* rels[] = {
        "include/mari/app/bridge.hpp",
        "include/mari/app/events.hpp",
        "include/mari/app/document.hpp",
        "include/mari/app/application.hpp",
    };
    const char* banned[] = {"windows.h", "HRESULT", "BSTR",      "IUnknown", "STDMETHODCALLTYPE",
                            "_WIN32",    "IDispatch", "REFIID",  "__stdcall"};
    for (const char* rel : rels) {
        const std::string text = readWholeFile(repoRoot() / rel);
        CHECK(!text.empty());
        for (const char* bad : banned) {
            if (text.find(bad) != std::string::npos) {
                mari_ctx.fail(__FILE__, __LINE__,
                              std::string(rel) + " 에 Win32 흔적이 있다: " + bad);
            }
        }
    }
}

MARI_TEST_MAIN()
