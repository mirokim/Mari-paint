// Mari Paint — Document: IDocumentBridge 구현 (플랫폼 중립)
#include <mari/app/document.hpp>

#include <mari/core/compositor.hpp>
#include <mari/crypto/canvas_hash.hpp>
#include <mari/crypto/sha256.hpp>
#include <mari/ora/image.hpp>
#include <mari/ora/ora.hpp>
#include <mari/psd/psd.hpp>

#include <cctype>
#include <filesystem>
#include <utility>

namespace mari::app {
namespace {

/// 캔버스 영역이 걸치는 타일 좌표를 전부 모은다(실행취소 범위 계산용).
void tilesForRect(const Rect& r, DirtyTiles& out) {
    if (r.isEmpty()) {
        return;
    }
    const i32 tx0 = tileIndexFor(r.x);
    const i32 ty0 = tileIndexFor(r.y);
    const i32 tx1 = tileIndexFor(r.right() - 1);
    const i32 ty1 = tileIndexFor(r.bottom() - 1);
    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            out.push_back(TileCoord{tx, ty});
        }
    }
}

/// 소문자 확장자/포맷 이름.
std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

} // namespace

Document::Document(LayerTreePtr tree, std::string path, EventHub* events)
    : tree_(std::move(tree)), path_(std::move(path)), events_(events) {
    // 🔴 문서의 시작 상태는 **전체 선택**(= 제한 없음)이다. 타일 0개라 공짜다.
    //    "선택이 비었다"로 시작하면 아무 데도 못 그리는 문서가 된다.
    selectionMask_ = SelectionMask::all(tree_->canvasSize());
}

void Document::setSelection(Rect r) {
    const Size sz = tree_->canvasSize();
    if (r.isEmpty()) {
        selectionMask_ = SelectionMask::all(sz); // 선택 해제 = 제한 없음
        return;
    }
    Result<SelectionMask> m = SelectionMask::fromRect(sz, r);
    if (m.ok()) {
        selectionMask_ = std::move(m).value();
    }
}

void Document::setSelectionMask(SelectionMask m) {
    const Size sz = tree_->canvasSize();
    if (m.canvasSize() != sz) {
        m.setCanvasSize(sz);
    }
    selectionMask_ = std::move(m);
}

Result<std::unique_ptr<Document>> Document::create(Size canvasSize, EventHub* events) {
    if (canvasSize.isEmpty()) {
        return Err("캔버스 크기가 0 이하다", ErrorCode::InvalidArgument);
    }
    Result<LayerTreePtr> tree = makeLayerTree(canvasSize);
    if (!tree.ok()) {
        return tree.error();
    }
    // 레이어 0장으로 시작하면 스크립트가 바로 그릴 수 없다. 한 장 깔아 준다.
    const Result<LayerPtr> first = tree.value()->addRaster("레이어 1");
    if (!first.ok()) {
        return first.error();
    }
    const Result<void> act = tree.value()->setActiveLayer(first.value()->id());
    if (!act.ok()) {
        return act.error();
    }
    return Ok(std::unique_ptr<Document>(new Document(tree.value(), std::string{}, events)));
}

Result<std::unique_ptr<Document>> Document::adopt(LayerTreePtr tree, std::string path,
                                                  EventHub* events) {
    if (!tree) {
        return Err("레이어 트리가 null 이다", ErrorCode::InvalidArgument);
    }
    const bool hadPath = !path.empty();
    std::unique_ptr<Document> doc(new Document(std::move(tree), std::move(path), events));
    // 파일에서 읽어 온 문서는 "저장된 상태"로 시작한다.
    doc->saved_ = hadPath;
    return Ok(std::move(doc));
}

Size Document::canvasSize() const { return tree_->canvasSize(); }

void Document::setViewState(const ViewState& v) {
    view_ = v;
    if (events_ != nullptr) {
        events_->fireViewChanged(view_.zoom, view_.rotationDeg);
    }
}

Result<void> Document::exportComposite(std::vector<u8>& dst) {
    const Size sz = tree_->canvasSize();
    if (sz.isEmpty()) {
        dst.clear();
        return Ok();
    }
    const usize rowBytes = static_cast<usize>(sz.width) * 4u;
    dst.assign(rowBytes * static_cast<usize>(sz.height), static_cast<u8>(0));
    return compositeArea(*tree_, Rect{0, 0, sz.width, sz.height}, dst.data(), rowBytes);
}

Result<void> Document::paintPixels(LayerId id, const Rect& area, const u8* rgba, usize len,
                                   std::string undoText, u32* outChangedTiles) {
    if (outChangedTiles != nullptr) {
        *outChangedTiles = 0;
    }
    if (closed_) {
        return Err("닫힌 문서다", ErrorCode::InvalidArgument);
    }
    if (area.isEmpty()) {
        return Err("영역이 비었다", ErrorCode::InvalidArgument);
    }
    const usize need = static_cast<usize>(area.width) * static_cast<usize>(area.height) * 4u;
    if (rgba == nullptr || len < need) {
        return Err("픽셀 버퍼가 영역보다 작다", ErrorCode::InvalidArgument);
    }
    const LayerPtr layer = tree_->find(id);
    if (!layer) {
        return Err("레이어를 찾을 수 없다", ErrorCode::NotFound);
    }
    if (layer->locked()) {
        return Err("잠긴 레이어다", ErrorCode::InvalidArgument);
    }
    TileMap* map = layer->tiles();
    if (map == nullptr) {
        return Err("그룹 레이어에는 픽셀을 쓸 수 없다", ErrorCode::InvalidArgument);
    }

    DirtyTiles tiles;
    tilesForRect(area, tiles);

    // 🔴 칠하기 **전에** 담는다(core/undo.hpp 규약). 순서가 뒤바뀌면 실행취소가 거짓말한다.
    std::unique_ptr<TileSnapshotCommand> cmd =
        TileSnapshotCommand::begin(std::move(undoText), map);
    cmd->captureBefore(tiles);

    ora::Image8 img;
    img.width = area.width;
    img.height = area.height;
    img.pixels.assign(rgba, rgba + need);
    const Result<void> w = ora::writeRegion(*map, area, img);
    if (!w.ok()) {
        return w;
    }

    cmd->captureAfter(tiles);
    if (outChangedTiles != nullptr) {
        // 🔴 타일맵이 이미 정확히 세고 있는 값을 그대로 쓴다. 다시 재지 않는다.
        *outChangedTiles = static_cast<u32>(cmd->changedTileCount());
    }
    if (!cmd->empty()) {
        undo_.push(std::move(cmd));
    }
    saved_ = false;
    if (events_ != nullptr) {
        events_->fireLayerChanged(id, "pixels");
    }
    return Ok();
}

Result<LayerId> Document::importPixels(const std::string& layerName, const u8* data, usize len,
                                       i32 w, i32 h) {
    if (closed_) {
        return Err("닫힌 문서다", ErrorCode::InvalidArgument);
    }
    if (w <= 0 || h <= 0) {
        return Err("들여올 이미지 크기가 0 이하다", ErrorCode::InvalidArgument);
    }
    const Result<LayerPtr> added = tree_->addRaster(layerName.empty() ? "가져온 레이어" : layerName);
    if (!added.ok()) {
        return added.error();
    }
    const LayerId id = added.value()->id();
    const Result<void> paint =
        paintPixels(id, Rect{0, 0, w, h}, data, len, std::string("픽셀 가져오기"));
    if (!paint.ok()) {
        // 실패하면 만든 레이어를 도로 치운다 — 반쯤 만들어진 상태를 남기지 않는다.
        (void)tree_->remove(id);
        return paint.error();
    }
    if (events_ != nullptr) {
        // 🔴 붓으로 그리지 않은 픽셀이 들어왔다. 반드시 보고한다(docs/03 2절 정신).
        events_->firePaste(PasteSource::Script);
        events_->fireLayerChanged(id, "added");
    }
    return Ok(id);
}

Result<std::string> Document::canvasHash() { return crypto::canvasHashHex(*tree_); }

Result<std::string> Document::layerHash(LayerId id) {
    const LayerPtr layer = tree_->find(id);
    if (!layer) {
        return Err("레이어를 찾을 수 없다", ErrorCode::NotFound);
    }
    return crypto::layerHashHex(*layer);
}

Result<std::string> Document::takeCanvasSnapshot() {
    Result<std::string> h = crypto::canvasHashHex(*tree_);
    if (!h.ok()) {
        return h;
    }
    if (events_ != nullptr) {
        events_->fireCanvasSnapshot(h.value());
    }
    return h;
}

Result<void> Document::exportLayerPixels(LayerId id, std::vector<u8>& dst, Rect& outArea) {
    const LayerPtr layer = tree_->find(id);
    if (!layer) {
        return Err("레이어를 찾을 수 없다", ErrorCode::NotFound);
    }
    const TileMap* map = layer->tiles();
    if (map == nullptr) {
        return Err("그룹 레이어에는 픽셀이 없다", ErrorCode::Unsupported);
    }
    outArea = map->bounds();
    if (outArea.isEmpty()) {
        dst.clear();
        return Ok();
    }
    Result<ora::Image8> img = ora::readRegion(*map, outArea);
    if (!img.ok()) {
        return img.error();
    }
    dst = std::move(img.value().pixels);
    return Ok();
}

Result<void> Document::undo() {
    DirtyTiles dirty;
    const Result<void> r = undo_.undo(&dirty);
    if (!r.ok()) {
        return r;
    }
    saved_ = false;
    if (events_ != nullptr) {
        events_->fireUndo(1);
    }
    return Ok();
}

Result<void> Document::redo() {
    DirtyTiles dirty;
    const Result<void> r = undo_.redo(&dirty);
    if (!r.ok()) {
        return r;
    }
    saved_ = false;
    if (events_ != nullptr) {
        events_->fireUndo(-1);
    }
    return Ok();
}

Result<void> Document::save() {
    if (path_.empty()) {
        return Err("저장된 적 없는 문서다 — saveAs() 로 경로를 정해라",
                   ErrorCode::InvalidArgument);
    }
    const auto dot = path_.rfind('.');
    std::string ext = dot == std::string::npos ? std::string() : path_.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return saveAs(path_, ext == "psd" ? "psd" : "ora");
}

Result<void> Document::writeCopy(const std::string& path) {
    if (closed_) {
        return Err("닫힌 문서다", ErrorCode::InvalidArgument);
    }
    ora::SaveOptions opts;
    if (recorder_ != nullptr) {
        Result<std::string> log = recorder_->buildProofLog();
        if (log.ok()) {
            opts.proofLog = std::move(log).value();
        }
    }
    return ora::save(*tree_, path, opts);
}

Result<void> Document::saveAs(const std::string& path, const std::string& format) {
    if (closed_) {
        return Err("닫힌 문서다", ErrorCode::InvalidArgument);
    }
    if (path.empty()) {
        return Err("경로가 비었다", ErrorCode::InvalidArgument);
    }
    const std::string fmt = lower(format);

    if (fmt == "ora") {
        ora::SaveOptions opts;
        // 🔴 Sigan 이 없어도 과정 기록은 파일 안에 남는다(docs/03 6절 · docs/06).
        //    내용은 레코더가 만든다 — Document 는 문자열을 옮겨 담을 뿐이고,
        //    prooflog 를 **만드는** 코드는 리포에 record/ 한 곳뿐이다.
        if (recorder_ != nullptr) {
            Result<std::string> log = recorder_->buildProofLog();
            if (log.ok()) {
                opts.proofLog = std::move(log).value();
            }
        }
        const Result<void> r = ora::save(*tree_, path, opts);
        if (!r.ok()) {
            return r;
        }
    } else if (fmt == "psd") {
        const Result<void> r = psd::save(*tree_, path);
        if (!r.ok()) {
            return r;
        }
    } else if (fmt == "png") {
        // 합성 결과 한 장. 레이어는 사라진다 — 그래서 path_ 를 갱신하지 않는다.
        std::vector<u8> pixels;
        const Result<void> comp = exportComposite(pixels);
        if (!comp.ok()) {
            return comp;
        }
        const Size sz = tree_->canvasSize();
        ora::Image8 img;
        img.width = sz.width;
        img.height = sz.height;
        img.pixels = std::move(pixels);
        Result<std::vector<u8>> png = ora::encodePng(img);
        if (!png.ok()) {
            return png.error();
        }
        const Result<void> w = ora::writeFileBytes(path, png.value().data(), png.value().size());
        if (!w.ok()) {
            return w;
        }
    } else {
        return Err("모르는 포맷이다: " + format + " (ora | png | psd)", ErrorCode::Unsupported);
    }

    // 🔴 저장 파일 해시는 **파일 바이트 그대로**다(crypto/sha256.hpp 규약).
    //    Sigan 이 sha256sum 으로 대조할 수 있어야 한다.
    std::string fileHash;
    const Result<std::string> h = crypto::sha256FileHex(path);
    if (h.ok()) {
        fileHash = h.value();
    }
    i64 sizeBytes = 0;
    std::error_code ec;
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    if (!ec) {
        sizeBytes = static_cast<i64>(sz);
    }

    if (fmt == "ora" || fmt == "psd") {
        path_ = path;
        saved_ = true;
    }
    if (events_ != nullptr) {
        events_->fireDocumentSaved(path, fileHash, sizeBytes);
    }
    return Ok();
}

Result<void> Document::close(bool saveChanges) {
    if (closed_) {
        return Ok();
    }
    if (saveChanges && !saved_) {
        const Result<void> r = save();
        if (!r.ok()) {
            return r;
        }
    }
    closed_ = true;
    undo_.clear();
    // 🔴 문서가 닫히면 작업 구간도 끝난다 — 저널이 여기서 닫힌다(docs/06 결정 ⑤).
    recorder_.reset();
    return Ok();
}

Result<i32> Document::apply8bf(const std::string& pluginPath, const std::string& filterName,
                               i32 timeoutMs, std::vector<std::string>& notes) {
    (void)timeoutMs;
    // 격리 호스트(hosts/)는 Windows 전용이고 아직 한 번도 컴파일된 적이 없다(docs/04 2절).
    // 조용히 성공한 척하면 스크립트가 필터가 걸린 줄 안다. 정직하게 거절한다.
    notes.push_back("8bf 격리 호스트는 이 빌드에 없다: " + pluginPath + " / " + filterName);
    return Err(".8bf 필터는 Windows 격리 호스트가 필요하다(docs/04 2절: 미검증)",
               ErrorCode::Unsupported);
}

} // namespace mari::app
