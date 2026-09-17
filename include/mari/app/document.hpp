// Mari Paint — IDocumentBridge 의 실제 구현체 (플랫폼 중립)
//
// core 의 `LayerTree`/`TileMap`/`UndoStack`, crypto 의 해시, io/ora 의 저장·읽기를
// 브리지 인터페이스에 물린다. Win32 를 **한 줄도** 쓰지 않는다.
#ifndef MARI_APP_DOCUMENT_HPP
#define MARI_APP_DOCUMENT_HPP

#include <mari/app/bridge.hpp>
#include <mari/core/undo.hpp>

#include <memory>
#include <string>

namespace mari::app {

/// 문서 한 벌. 레이어 트리 + 실행취소 + 뷰 상태 + 선택 영역.
///
/// 스레드 규약: 한 인스턴스는 한 스레드가 쓴다. 이벤트는 부른 스레드에서 그대로 뿌린다.
class Document final : public IDocumentBridge {
public:
    /// 빈 문서를 만든다. 래스터 레이어 한 장("레이어 1")이 들어간 상태로 시작한다 —
    /// 레이어가 0장인 문서는 스크립트가 바로 그릴 수 없어서 쓸모가 없다.
    /// `events` 는 소유하지 않는다. nullptr 이면 이벤트를 쏘지 않는다.
    [[nodiscard]] static Result<std::unique_ptr<Document>> create(Size canvasSize,
                                                                  EventHub* events = nullptr);
    /// 이미 만들어진 트리를 감싼다(.ora 를 읽은 뒤에 쓴다).
    [[nodiscard]] static Result<std::unique_ptr<Document>> adopt(LayerTreePtr tree,
                                                                 std::string path,
                                                                 EventHub* events = nullptr);

    ~Document() override = default;

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // ── IDocumentBridge ──────────────────────────────────────────────────
    [[nodiscard]] Size canvasSize() const override;
    [[nodiscard]] std::string fullPath() const override { return path_; }
    [[nodiscard]] bool isSaved() const override { return saved_; }
    [[nodiscard]] LayerTree& layers() override { return *tree_; }

    [[nodiscard]] ViewState viewState() const override { return view_; }

    [[nodiscard]] Result<void> save() override;
    [[nodiscard]] Result<void> saveAs(const std::string& path, const std::string& format) override;
    [[nodiscard]] Result<void> close(bool saveChanges) override;

    [[nodiscard]] Result<void> exportComposite(std::vector<u8>& dst) override;
    [[nodiscard]] Result<LayerId> importPixels(const std::string& layerName, const u8* data,
                                               usize len, i32 w, i32 h) override;

    [[nodiscard]] Result<std::string> canvasHash() override;
    [[nodiscard]] Result<std::string> layerHash(LayerId id) override;

    [[nodiscard]] Result<void> exportLayerPixels(LayerId id, std::vector<u8>& dst,
                                                 Rect& outArea) override;

    [[nodiscard]] Result<void> undo() override;
    [[nodiscard]] Result<void> redo() override;

    [[nodiscard]] Rect selection() const override { return selection_; }
    void setSelection(Rect r) override { selection_ = r; }

    /// 🔴 격리 호스트(hosts/)는 Windows 전용이고 docs/04 2절에서 "미검증"이다.
    ///    없는 기능을 있는 척하지 않는다 — 여기서는 Unsupported 를 돌려준다.
    [[nodiscard]] Result<i32> apply8bf(const std::string& pluginPath,
                                       const std::string& filterName, i32 timeoutMs,
                                       std::vector<std::string>& notes) override;

    // ── 브리지 밖의 편의 (본체·테스트가 쓴다) ────────────────────────────
    /// 뷰 상태를 바꾸고 `OnViewChanged` 를 쏜다.
    void setViewState(const ViewState& v);

    /// 기존 래스터 레이어의 영역을 RGBA8 로 **덮어쓴다**(합성 아님).
    /// 실행취소 스택에 타일 단위로 기록된다. `importPixels()` 의 알맹이다.
    [[nodiscard]] Result<void> paintPixels(LayerId id, const Rect& area, const u8* rgba, usize len,
                                           std::string undoText);

    /// 캔버스 해시를 계산해서 `OnCanvasSnapshot` 으로 보고한다(docs/03 S3 의 자리).
    /// 🔴 해시를 낼 뿐 봉인하지 않는다. 체인은 Sigan 이 만든다.
    [[nodiscard]] Result<std::string> takeCanvasSnapshot();

    [[nodiscard]] UndoStack& undoStack() noexcept { return undo_; }
    [[nodiscard]] bool isClosed() const noexcept { return closed_; }
    /// 픽셀이 바뀌었다고 표시한다(저장 상태를 깬다).
    void markDirty() noexcept { saved_ = false; }

private:
    Document(LayerTreePtr tree, std::string path, EventHub* events);

    LayerTreePtr tree_;
    UndoStack undo_{128};
    std::string path_;
    ViewState view_{};
    Rect selection_{};
    EventHub* events_ = nullptr;
    bool saved_ = false;
    bool closed_ = false;
};

} // namespace mari::app

#endif // MARI_APP_DOCUMENT_HPP
