// Mari Paint — IDocumentBridge 의 실제 구현체 (플랫폼 중립)
//
// core 의 `LayerTree`/`TileMap`/`UndoStack`, crypto 의 해시, io/ora 의 저장·읽기를
// 브리지 인터페이스에 물린다. Win32 를 **한 줄도** 쓰지 않는다.
#ifndef MARI_APP_DOCUMENT_HPP
#define MARI_APP_DOCUMENT_HPP

#include <mari/agent/recording.hpp>
#include <mari/app/bridge.hpp>
#include <mari/core/selection.hpp>
#include <mari/core/undo.hpp>

#include <memory>
#include <optional>
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
    /// `background` 를 주면 그 색으로 채운 잠긴 "배경" 레이어를 맨 아래 깐다(포토샵 배경 · CSP 용지).
    /// 기록기가 붙기 전이라 획·연산으로 남지 않는다 — 문서의 초기 상태다. 주지 않으면 투명.
    [[nodiscard]] static Result<std::unique_ptr<Document>> create(Size canvasSize,
                                                                  EventHub* events = nullptr,
                                                                  std::optional<Color8> background = std::nullopt);
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
    /// 현재 상태를 .ora 로 **복사만** 쓴다(경로·저장됨 표시를 건드리지 않는다). 자동 저장·백업용.
    [[nodiscard]] Result<void> writeCopy(const std::string& path);
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

    /// 🔴 브리지(COM)는 여전히 사각형 하나만 안다. 마스크가 들어온 뒤에도 이 약속은
    ///    그대로다 — **경계 상자**를 돌려준다. 전체 선택은 "제한 없음"이라 빈 Rect 다
    ///    (그 규약 위에서 `region: "selection"` · 그리기 기본 영역이 이미 돌고 있다).
    ///    사각형이 아닌 선택을 사각형으로 받아 가면 정보가 준다는 사실은 숨기지 않는다:
    ///    진짜 모양은 `selectionMask()` 에 있다.
    [[nodiscard]] Rect selection() const override {
        return selectionMask_.isAll() ? Rect{} : selectionMask_.bounds();
    }
    void setSelection(Rect r) override;

    /// 선택 마스크 정본. 그리기 경로가 이걸 본다.
    [[nodiscard]] const SelectionMask& selectionMask() const noexcept { return selectionMask_; }
    /// 마스크를 통째로 갈아 끼운다. 캔버스 크기가 다르면 맞춰 준다.
    void setSelectionMask(SelectionMask m);

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
    /// `outChangedTiles` 를 주면 실제로 바뀐 타일 수를 담아 준다(docs/06 결정 ② 축 C).
    [[nodiscard]] Result<void> paintPixels(LayerId id, const Rect& area, const u8* rgba, usize len,
                                           std::string undoText,
                                           u32* outChangedTiles = nullptr);

    // ── 기록 (docs/06) ───────────────────────────────────────────────────
    //
    // 🔴 레코더는 **문서에** 붙는다. 세션에 붙이지 않는다(docs/06 결정 ⑤) —
    //    세션은 오고 가지만 한 작품이 만들어진 구간은 이어져야 하기 때문이다.
    //    문서가 닫히면 구간이 끝나고 레코더도 함께 닫힌다.
    //
    // 🔴 문서는 `IStrokeRecorder` 인터페이스만 안다. `SiganPublisher` 도, 저널도,
    //    파이프도 모른다 — 그래서 app 은 sigan 을 링크하지 않는다.

    /// 이 문서의 기록 구간을 붙인다. nullptr 이면 기록하지 않는다(널 레코더).
    void attachRecorder(std::unique_ptr<agent::IStrokeRecorder> rec) noexcept {
        recorder_ = std::move(rec);
    }
    [[nodiscard]] agent::IStrokeRecorder* recorder() noexcept { return recorder_.get(); }
    [[nodiscard]] const agent::IStrokeRecorder* recorder() const noexcept {
        return recorder_.get();
    }
    /// 기록이 고장 나 **남지 않는 상태**인가(docs/06 결정 ④).
    /// 레코더가 아예 없는 것과 다르다 — 없으면 애초에 기록하지 않기로 한 것이고,
    /// 고장은 기록하기로 해 놓고 못 하는 것이다. 그때는 그리기를 거절한다.
    [[nodiscard]] bool recordingBroken() const noexcept {
        return recorder_ != nullptr && recorder_->recordingBroken();
    }

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
    /// 🔴 선택 정본은 **마스크**다. 사각형은 그 경계 상자일 뿐이다.
    ///    전체 선택·빈 선택은 타일 0개라 빈 문서가 선택 때문에 메모리를 쓰지 않는다.
    SelectionMask selectionMask_;
    std::unique_ptr<agent::IStrokeRecorder> recorder_;
    EventHub* events_ = nullptr;
    bool saved_ = false;
    bool closed_ = false;
};

} // namespace mari::app

#endif // MARI_APP_DOCUMENT_HPP
