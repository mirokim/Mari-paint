// Mari Paint — Application: IApplicationBridge 구현 (플랫폼 중립)
#include <mari/app/application.hpp>

#include <mari/crypto/sha256.hpp>
#include <mari/ora/ora.hpp>

#include <algorithm>
#include <utility>

namespace mari::app {
namespace {

/// 프로세스에 하나뿐인 브리지. COM 서버가 켜지기 전에 본체가 심는다.
IApplicationBridge*& bridgeSlot() noexcept {
    static IApplicationBridge* g = nullptr;
    return g;
}

/// 확장자(점 제외, 소문자). 없으면 빈 문자열.
std::string extensionOf(const std::string& path) {
    const usize dot = path.find_last_of('.');
    const usize slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return std::string{};
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

} // namespace

void setApplicationBridge(IApplicationBridge* bridge) noexcept { bridgeSlot() = bridge; }
IApplicationBridge* applicationBridge() noexcept { return bridgeSlot(); }

std::string Application::version() const {
    // CMake 의 project(MariPaint VERSION ...) 와 같이 간다. 갈라지면 여기를 고친다.
    return "0.1.0";
}

void Application::quit() {
    // 🔴 저장하지 않는다. 스크립트가 quit() 한 번으로 조용히 덮어쓰면 안 된다.
    for (std::unique_ptr<Document>& d : docs_) {
        (void)d->close(false);
    }
    docs_.clear();
    active_ = 0;
    quitRequested_ = true;
}

IDocumentBridge* Application::documentAt(usize index) {
    if (index >= docs_.size()) {
        return nullptr;
    }
    return docs_[index].get();
}

IDocumentBridge* Application::activeDocument() {
    if (docs_.empty()) {
        return nullptr;
    }
    return docs_[active_ < docs_.size() ? active_ : docs_.size() - 1].get();
}

Result<IDocumentBridge*> Application::createDocument(i32 w, i32 h) {
    Result<std::unique_ptr<Document>> doc = Document::create(Size{w, h}, &events_);
    if (!doc.ok()) {
        return doc.error();
    }
    Document* raw = doc.value().get();
    docs_.push_back(std::move(doc).value());
    active_ = docs_.size() - 1;
    return Ok(static_cast<IDocumentBridge*>(raw));
}

Result<IDocumentBridge*> Application::open(const std::string& path) {
    const std::string ext = extensionOf(path);
    if (ext != "ora") {
        // 추측해서 열지 않는다(io/brush 의 "정직하게 실패한다"와 같은 태도).
        return Err("지금은 .ora 만 연다: " + path, ErrorCode::Unsupported);
    }
    Result<ora::Document> loaded = ora::load(path);
    if (!loaded.ok()) {
        return loaded.error();
    }
    Result<std::unique_ptr<Document>> doc =
        Document::adopt(std::move(loaded).value().tree, path, &events_);
    if (!doc.ok()) {
        return doc.error();
    }
    Document* raw = doc.value().get();
    docs_.push_back(std::move(doc).value());
    active_ = docs_.size() - 1;

    // 🔴 연 **파일**의 해시다(도메인 접두사 없음). 캔버스 픽셀 해시와 다르다.
    std::string fileHash;
    const Result<std::string> h = crypto::sha256FileHex(path);
    if (h.ok()) {
        fileHash = h.value();
    }
    const Size sz = raw->canvasSize();
    events_.fireDocumentOpened(path, fileHash, sz.width, sz.height);
    return Ok(static_cast<IDocumentBridge*>(raw));
}

Result<void> Application::closeDocument(IDocumentBridge* doc, bool saveChanges) {
    const auto it = std::find_if(docs_.begin(), docs_.end(),
                                 [doc](const std::unique_ptr<Document>& d) {
                                     return static_cast<IDocumentBridge*>(d.get()) == doc;
                                 });
    if (it == docs_.end()) {
        return Err("이 앱이 들고 있는 문서가 아니다", ErrorCode::NotFound);
    }
    const Result<void> r = (*it)->close(saveChanges);
    if (!r.ok()) {
        return r;
    }
    docs_.erase(it);
    if (active_ >= docs_.size()) {
        active_ = docs_.empty() ? 0 : docs_.size() - 1;
    }
    return Ok();
}

Result<void> Application::setActiveDocument(usize index) {
    if (index >= docs_.size()) {
        return Err("문서 인덱스가 범위를 벗어났다", ErrorCode::NotFound);
    }
    active_ = index;
    return Ok();
}

} // namespace mari::app
