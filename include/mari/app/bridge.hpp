// Mari Paint — 앱 브리지 인터페이스 (플랫폼 중립)
//
// 🔴 이 파일이 생긴 이유 (docs/04 2절 마지막 문단)
//    `mari/win/com/app_bridge.hpp` 에 `IDocumentBridge`/`IApplicationBridge` 가
//    순수 가상으로 선언돼 있지만 **구현체가 리포 어디에도 없었다.** COM 레이어는
//    주입받을 앱 객체를 기다리는 껍데기였고, 그 헤더는 Windows 전용(`#error`)이라
//    Linux 에서는 한 줄도 컴파일되지 않는다.
//
//    그래서 같은 메서드 집합을 **Win32 없이** 여기 다시 세우고, 실제 구현체
//    (`mari::app::Document` / `mari::app::Application`)를 붙였다.
//    · 이 헤더는 Win32 를 포함하지 않는다 → Linux 에서 컴파일·테스트된다.
//    · COM 래퍼만 Windows 전용으로 남는다. Windows 빌드가 처음 초록이 되는 날
//      `mari::win::com::IDocumentBridge` 는 이 인터페이스를 감싸는 얇은 어댑터가 된다.
//      (그 어댑터는 아직 없다 — docs/04 2절의 "작성됨, 미검증"을 늘리지 않으려고
//       Windows SDK 없이 검증할 수 없는 코드는 쓰지 않았다.)
//
//    차이는 하나뿐이다: `events()` 가 COM 의 `EventBroadcaster` 대신 중립
//    `mari::app::EventHub` 를 돌려준다. COM 브로드캐스터는 그 허브의 리스너가 된다.
//
// 좌표는 전부 **캔버스 좌표**(좌상단 원점, y 아래로 증가)다.
#ifndef MARI_APP_BRIDGE_HPP
#define MARI_APP_BRIDGE_HPP

#include <mari/app/events.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/types.hpp>
#include <mari/win/input/view_transform.hpp>

#include <string>
#include <vector>

namespace mari::app {

/// 뷰 상태. 이미 있는 중립 타입을 그대로 쓴다 — 두 벌 만들지 않는다.
/// (`mari/win/input/view_transform.hpp` 는 Win32 를 포함하지 않는 순수 수학이라
///  이름만 win 이고 Linux 에서 이미 테스트되고 있다.)
using ViewState = mari::win::ViewState;

/// 스크립트/COM 이 보는 문서 하나.
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

    /// 합성 결과를 RGBA8 로 뽑는다. dst 는 w*h*4 바이트로 맞춰진다.
    [[nodiscard]] virtual Result<void> exportComposite(std::vector<u8>& dst) = 0;

    /// RGBA8 픽셀을 새 레이어로 들여온다.
    /// 🔴 이 경로로 들어온 픽셀은 반드시 `OnPaste(Script)` 로 보고한다 —
    ///    붓으로 그리지 않은 픽셀을 그린 것처럼 위장하지 않는다(docs/03 2절 정신).
    [[nodiscard]] virtual Result<LayerId> importPixels(const std::string& layerName, const u8* data,
                                                       usize len, i32 w, i32 h) = 0;

    /// 합성 결과의 SHA-256(소문자 16진). 규약은 `mari/crypto/canvas_hash.hpp`.
    /// 🔴 해시를 **계산**할 뿐이다. 체인 봉인·서명은 Sigan 의 몫이다(docs/03 2절).
    [[nodiscard]] virtual Result<std::string> canvasHash() = 0;
    /// 레이어 하나의 픽셀 해시.
    [[nodiscard]] virtual Result<std::string> layerHash(LayerId id) = 0;

    /// 레이어 하나를 RGBA8 로 뽑는다. `outArea` 는 실제로 뽑힌 캔버스 영역
    /// (= 레이어 bounds), `dst` 는 outArea.width*outArea.height*4 바이트.
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

/// 🔴 Sigan 연동 상태 (docs/03 5.1 — **토글은 없다. 상태 표시만 한다**).
///
/// 값을 앱이 계산하지 않고 밖에서 밀어 넣는다. 앱이 `SiganPublisher` 를 직접
/// 들고 있으면 app → sigan 의존이 생기고, 그러면 UI 없는 헤드리스에서도 발행기를
/// 끼워야 한다. 발행기를 소유한 쪽이 `setSiganStatus()` 로 알려 주는 편이 얇다.
struct SiganStatus {
    bool connected = false;
    /// 싱크가 못 받아 저널에만 있는 프레임 수.
    /// 🔴 **드롭이 아니라 스풀이다**(docs/03 4.2). 유실 카운터가 아니다.
    u64 spooledFrames = 0;
    u64 lastSeq = 0;
};

/// 스크립트/COM 이 보는 애플리케이션.
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
    [[nodiscard]] virtual EventHub& events() = 0;

    [[nodiscard]] virtual SiganStatus siganStatus() const = 0;
};

/// 프로세스에 하나뿐인 브리지. 본체가 COM 서버를 켜기 전에 심는다.
/// nullptr 이면 COM 객체 생성이 실패해야 한다 —
/// **반쯤 살아 있는 객체를 스크립트에 넘기지 않는다.**
void setApplicationBridge(IApplicationBridge* bridge) noexcept;
[[nodiscard]] IApplicationBridge* applicationBridge() noexcept;

/// 🔴 항상 "windows-ink" 다. WinTab 은 구현하지 않는다(docs/03 3절).
inline constexpr const char* kPenInputApi = "windows-ink";

} // namespace mari::app

#endif // MARI_APP_BRIDGE_HPP
