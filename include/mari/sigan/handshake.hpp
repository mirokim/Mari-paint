// Mari Paint — 핸드셰이크 · 버전 협상 (docs/03 5.5)
//
// 규칙 3개를 코드로 강제한다:
//   1. 필드는 **더하기만** 한다 — 빼거나 의미를 바꾸지 않는다.
//   2. **모르는 필드는 무시**한다 — 에러가 아니다.
//   3. `proto` 가 다르면 **공통 최소로 내려간다** — 연결을 끊지 않는다.
#ifndef MARI_SIGAN_HANDSHAKE_HPP
#define MARI_SIGAN_HANDSHAKE_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <string>
#include <vector>

namespace mari::sigan {

/// Mari 가 말할 수 있는 능력들. 안정 문자열 — **뒤에만 더해라.**
namespace caps {
inline constexpr const char* kCanvasXy = "canvas-xy"; ///< 캔버스 좌표 (줌·회전 불변)
inline constexpr const char* kLayer = "layer";        ///< layerId / brushId
inline constexpr const char* kBrush = "brush";
inline constexpr const char* kPixelHash = "pixel-hash"; ///< 픽셀 스냅샷 해시 (S3)
inline constexpr const char* kView = "view";            ///< 뷰 변환 보고
inline constexpr const char* kResume = "resume";        ///< 밀린 seq 재전송 (docs/03 5.3)
} // namespace caps

/// Mari 가 구현하는 프로토콜 버전. 올릴 때는 필드를 더하기만 한다.
inline constexpr u32 kProtoVersion = 1;

/// 한쪽이 상대에게 알리는 자기소개. 모르는 키는 파서가 그냥 버린다.
struct Handshake {
    u32 proto = kProtoVersion;
    std::string app;               ///< "mari-paint/0.1.0"
    std::vector<std::string> caps; ///< 보내는 쪽 = 제공 능력, 받는 쪽 = accepts
    u64 sessionId = 0;             ///< 재연결 시 같은 세션임을 밝힌다
    u64 resumeFromSeq = 0;         ///< "seq N 부터 밀렸다" (0 = 밀린 것 없음)
    i64 wallAnchorUnixMs = 0;      ///< 세션 시작 벽시계 앵커 (한 번만)

    [[nodiscard]] bool hasCap(const std::string& c) const noexcept;
};

/// 협상 결과. **실패라는 결과는 없다** — 연결은 끊지 않는다(규칙 3).
struct Negotiated {
    u32 proto = kProtoVersion;     ///< min(local, remote)
    std::vector<std::string> caps; ///< 교집합. 여기 없는 능력은 프레임에서 생략(0)한다
    bool protoDowngraded = false;  ///< proto 가 서로 달라 내려갔다
    std::vector<std::string> dropped; ///< 상대가 모르는 능력. 정직하게 남긴다

    [[nodiscard]] bool hasCap(const std::string& c) const noexcept;
};

/// 규칙 3개를 적용한다. 어떤 입력에도 에러를 내지 않는다.
Negotiated negotiate(const Handshake& local, const Handshake& remote) noexcept;

/// 핸드셰이크를 한 줄 JSON 으로 쓴다(개행으로 끝난다).
std::string encodeHandshake(const Handshake& h);

/// 한 줄 JSON 을 읽는다. **모르는 키는 조용히 무시한다**(규칙 2).
/// 형태 자체가 JSON 오브젝트가 아닐 때만 실패한다.
Result<Handshake> decodeHandshake(const std::string& json);

} // namespace mari::sigan

#endif // MARI_SIGAN_HANDSHAKE_HPP
