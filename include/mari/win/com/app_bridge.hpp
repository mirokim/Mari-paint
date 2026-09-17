// Mari Paint — COM 레이어 ↔ 본체 사이의 다리
//
// 🔴 왜 이런 게 있나
//    COM 레이어가 문서 관리자·UI 를 직접 들고 있으면 `com → ui` 역방향 의존이 생긴다.
//    docs/02 2절이 금지하는 것이다("역방향 의존이 생기면 리뷰에서 막는다").
//    그래서 COM 구현체는 이 순수 가상 인터페이스만 보고, 본체가 그걸 구현한다.
//
//    덕분에 COM 레이어는 Qt 를 모르고, 테스트에서는 가짜 브리지로 갈아끼울 수 있다.
//
// ⚠️ Windows 전용(COM 레이어 전용 헤더).
#ifndef MARI_WIN_COM_APP_BRIDGE_HPP
#define MARI_WIN_COM_APP_BRIDGE_HPP

#if !defined(_WIN32)
#error "mari/win/com/app_bridge.hpp 는 Windows 전용이다"
#endif

#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/types.hpp>
#include <mari/win/com/event_sink.hpp>
#include <mari/win/input/view_transform.hpp>

#include <string>
#include <vector>

namespace mari::win::com {

/// COM 이 보는 문서 하나. 본체가 구현한다.
class IDocumentBridge {
public:
    virtual ~IDocumentBridge() = default;

    [[nodiscard]] virtual Size canvasSize() const = 0;
    /// 저장된 적 없으면 빈 문자열.
    [[nodiscard]] virtual std::string fullPath() const = 0;
    [[nodiscard]] virtual bool isSaved() const = 0;
    [[nodiscard]] virtual LayerTree& layers() = 0;

    /// 현재 뷰 변환. 🔴 Sigan 의 두 증인 대조에 쓴다(docs/03 7절).
    [[nodiscard]] virtual ViewState viewState() const = 0;

    [[nodiscard]] virtual Result<void> save() = 0;
    /// format 은 "ora" | "png" | "psd".
    [[nodiscard]] virtual Result<void> saveAs(const std::string& path,
                                              const std::string& format) = 0;
    [[nodiscard]] virtual Result<void> close(bool saveChanges) = 0;

    /// 합성 결과를 RGBA8 로 뽑는다. dst 는 w*h*4 바이트.
    [[nodiscard]] virtual Result<void> exportComposite(std::vector<u8>& dst) = 0;

    /// RGBA8 픽셀을 새 레이어로 들여온다.
    /// 🔴 이 경로로 들어온 픽셀은 반드시 `OnPaste(Script)` 로 보고해야 한다 —
    ///    붓으로 그리지 않은 픽셀을 그린 것처럼 위장하지 않는다(docs/03 2절 정신).
    [[nodiscard]] virtual Result<LayerId> importPixels(const std::string& layerName, const u8* data,
                                                       usize len, i32 w, i32 h) = 0;

    /// 합성 결과의 SHA-256(소문자 16진).
    /// 🔴 해시를 **계산**할 뿐이다. 체인 봉인·서명은 Sigan 의 몫이다(docs/03 2절).
    [[nodiscard]] virtual Result<std::string> canvasHash() = 0;
    /// 레이어 하나의 픽셀 해시.
    [[nodiscard]] virtual Result<std::string> layerHash(LayerId id) = 0;

    /// 레이어 하나를 RGBA8 로 뽑는다. `outArea` 는 실제로 뽑힌 캔버스 영역
    /// (= 레이어 bounds), `dst` 는 outArea.w*outArea.h*4 바이트.
    /// 🔴 타일 순회·블렌드는 core 의 일이다. COM 레이어가 직접 하지 않는다.
    [[nodiscard]] virtual Result<void> exportLayerPixels(LayerId id, std::vector<u8>& dst,
                                                         Rect& outArea) = 0;

    [[nodiscard]] virtual Result<void> undo() = 0;
    [[nodiscard]] virtual Result<void> redo() = 0;

    /// 선택 영역(캔버스 좌표, 반열림). 비어 있으면 빈 Rect.
    [[nodiscard]] virtual Rect selection() const = 0;
    virtual void setSelection(Rect r) = 0;

    /// .8bf 필터를 건다. 격리 호스트에서 돈다. timeoutMs <= 0 이면 기본값.
    [[nodiscard]] virtual Result<i32> apply8bf(const std::string& pluginPath,
                                               const std::string& filterName, i32 timeoutMs,
                                               std::vector<std::string>& notes) = 0;
};

/// COM 이 보는 애플리케이션. 본체가 구현한다.
class IApplicationBridge {
public:
    virtual ~IApplicationBridge() = default;

    [[nodiscard]] virtual std::string version() const = 0;
    [[nodiscard]] virtual bool visible() const = 0;
    virtual void setVisible(bool v) = 0;
    virtual void quit() = 0;

    [[nodiscard]] virtual usize documentCount() const = 0;
    /// 범위를 벗어나면 nullptr.
    [[nodiscard]] virtual IDocumentBridge* documentAt(usize index) = 0;
    [[nodiscard]] virtual IDocumentBridge* activeDocument() = 0;

    [[nodiscard]] virtual Result<IDocumentBridge*> createDocument(i32 w, i32 h) = 0;
    [[nodiscard]] virtual Result<IDocumentBridge*> open(const std::string& path) = 0;

    /// 이벤트 방송기. COM 이 AdviseEvents 를 여기로 넘긴다.
    [[nodiscard]] virtual EventBroadcaster& events() = 0;

    // ── Sigan 연동 상태 (docs/03 5.1 — 토글은 없다. 상태 표시만 한다) ──────
    [[nodiscard]] virtual bool siganConnected() const = 0;
    /// 싱크가 못 받아 저널에만 있는 프레임 수. 🔴 드롭이 아니라 스풀이다(docs/03 4.2).
    [[nodiscard]] virtual u64 siganSpooledFrames() const = 0;
    [[nodiscard]] virtual u64 siganLastSeq() const = 0;
};

/// 프로세스에 하나뿐인 브리지. 본체가 COM 서버를 켜기 전에 심는다.
/// nullptr 이면 COM 객체 생성이 `CO_E_SERVER_STOPPING` 으로 실패한다 —
/// **반쯤 살아 있는 객체를 스크립트에 넘기지 않는다.**
void setApplicationBridge(IApplicationBridge* bridge) noexcept;
[[nodiscard]] IApplicationBridge* applicationBridge() noexcept;

/// 🔴 항상 "windows-ink" 다. WinTab 은 구현하지 않는다(docs/03 3절).
///    이 값이 다른 것을 반환하면 설계 위반이고, CI 가 그것과 별개로
///    `wintab32.dll` 임포트를 검사한다(scripts/ci/check-no-wintab.ps1).
inline constexpr const char* kPenInputApi = "windows-ink";

} // namespace mari::win::com

#endif // MARI_WIN_COM_APP_BRIDGE_HPP
