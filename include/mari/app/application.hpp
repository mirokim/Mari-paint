// Mari Paint — IApplicationBridge 의 실제 구현체 (플랫폼 중립)
#ifndef MARI_APP_APPLICATION_HPP
#define MARI_APP_APPLICATION_HPP

#include <mari/app/bridge.hpp>
#include <mari/app/document.hpp>

#include <memory>
#include <vector>

namespace mari::app {

/// 열린 문서들을 들고 있는 애플리케이션. UI 를 모른다 — 헤드리스가 기본값이다
/// (docs/05 2.8 "같은 바이너리, 같은 코드 경로").
class Application final : public IApplicationBridge {
public:
    Application() = default;
    ~Application() override = default;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] std::string version() const override;
    [[nodiscard]] bool visible() const override { return visible_; }
    void setVisible(bool v) override { visible_ = v; }
    /// 문서를 전부 닫고 종료 요청을 세운다. **저장하지 않는다** —
    /// 저장 여부는 부르는 쪽이 정한다(스크립트가 조용히 덮어쓰면 안 된다).
    void quit() override;

    [[nodiscard]] usize documentCount() const override { return docs_.size(); }
    [[nodiscard]] IDocumentBridge* documentAt(usize index) override;
    [[nodiscard]] IDocumentBridge* activeDocument() override;

    [[nodiscard]] Result<IDocumentBridge*> createDocument(i32 w, i32 h) override;
    /// 지금은 .ora 만 읽는다. 다른 확장자는 Unsupported 다(추측해서 열지 않는다).
    [[nodiscard]] Result<IDocumentBridge*> open(const std::string& path) override;

    [[nodiscard]] EventHub& events() override { return events_; }
    [[nodiscard]] SiganStatus siganStatus() const override { return sigan_; }

    // ── 브리지 밖의 편의 ─────────────────────────────────────────────────
    /// 🔴 상태를 **받아 적는다.** 토글이 아니다(docs/03 5.1).
    ///    발행기(`sigan::SiganPublisher`)를 소유한 쪽이 주기적으로 밀어 넣는다.
    void setSiganStatus(const SiganStatus& s) noexcept { sigan_ = s; }

    /// 문서를 닫고 목록에서 뺀다. 모르는 포인터면 NotFound.
    [[nodiscard]] Result<void> closeDocument(IDocumentBridge* doc, bool saveChanges);
    [[nodiscard]] Result<void> setActiveDocument(usize index);
    [[nodiscard]] bool quitRequested() const noexcept { return quitRequested_; }

private:
    std::vector<std::unique_ptr<Document>> docs_;
    EventHub events_;
    SiganStatus sigan_{};
    usize active_ = 0;
    bool visible_ = false;
    bool quitRequested_ = false;
};

} // namespace mari::app

#endif // MARI_APP_APPLICATION_HPP
