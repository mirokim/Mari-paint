// Mari Paint — 🔴 시각 피드백 (docs/05 2.1 — "AI 는 자기가 그린 걸 볼 수 있어야 한다")
//
// **이게 이 모듈에서 제일 중요하다.** 눈 없는 AI 는 그림을 못 그린다.
// 그래서 **모든 연산**이 `view` 옵션을 받고, 결과에 이미지를 실을 수 있다.
//
// 타일 캔버스라서 이게 공짜다: 스트로크 파이프라인이 이미 더티 타일을 추적하므로
// **바뀐 타일만** 잘라 PNG 로 돌려준다. 헤드리스 앱을 스크린샷 찍는 것보다
// 증분적이고 의미가 있다(docs/05 2.1).
//
// | mode           | 반환 |
// |----------------|------|
// | `none`         | 이미지 없음(빠름) |
// | `dirty`        | **바뀐 영역만** — 기본값 |
// | `full`         | 캔버스 전체(max 로 축소) |
// | `layer:<id>`   | 그 레이어의 픽셀만(합성하지 않는다) |
// | `region:[x,y,w,h]` | 지정 영역 |
//
// PNG 인코더는 io/ora 의 것을 **재사용한다** — 두 번째 구현을 만들지 않는다.
#ifndef MARI_AGENT_VIEW_HPP
#define MARI_AGENT_VIEW_HPP

#include <mari/agent/json.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

#include <string>

namespace mari::agent {

/// 시각 피드백 모드.
enum class ViewMode : u8 {
    None = 0, ///< 이미지 없음
    Dirty,    ///< 바뀐 영역만(기본값)
    Full,     ///< 캔버스 전체
    Layer,    ///< 레이어 하나
    Region,   ///< 지정 영역
};

/// 안정 문자열(capabilities 와 응답에 그대로 쓴다).
[[nodiscard]] const char* viewModeName(ViewMode m) noexcept;

/// 요청에 실린 `view` 옵션.
struct ViewSpec {
    ViewMode mode = ViewMode::Dirty;
    /// 긴 변 상한(px). 넘으면 알파 가중 박스 필터로 줄인다. 확대는 하지 않는다.
    i32 max = 512;
    /// `layer:<id>` 의 대상. Layer 모드에서만 본다.
    LayerId layer = kInvalidLayerId;
    /// `region:[x,y,w,h]` 의 대상. Region 모드에서만 본다.
    Rect region{};
    /// PNG 압축 강도 0~9. 에이전트 왕복에서는 속도가 크기보다 중요해서 낮게 잡았다.
    int pngLevel = 3;
};

/// 상한. 4096 을 넘는 미리보기는 base64 로 실어 보낼 물건이 아니다.
inline constexpr i32 kMaxViewSide = 4096;
/// `max` 를 주지 않았을 때의 기본값(docs/05 2.1 예시와 같다).
inline constexpr i32 kDefaultViewMax = 512;

/// `view` 필드를 읽는다. 없으면 기본값(dirty/512).
///
/// 두 표기를 모두 받는다 — 에이전트가 짧게 쓸 수 있어야 한다:
///   · 문자열: `"none"` `"dirty"` `"full"` `"layer:12"` `"region:[0,0,64,64]"`
///   · 객체:   `{"mode":"dirty","max":512}` / `{"mode":"layer","layer":12}`
///
/// 🔴 모르는 키는 **거절한다.** 오타가 조용히 무시되면 AI 는 안 보이는 이유를 못 찾는다.
[[nodiscard]] Result<ViewSpec> parseViewSpec(const Json& v);

/// 렌더 결과. 이미지가 없으면 `image.isNull()`.
struct ViewResult {
    Json image = Json::null(); ///< {png, x, y, w, h, scale, srcW, srcH, ...}
    Rect area{};               ///< 실제로 잘라 낸 캔버스 영역(축소 전)
};

/// 시각 피드백을 만든다.
///
/// `dirtyArea` 는 이번 연산이 바꾼 캔버스 영역이다(`dirty` 모드가 이걸 쓴다).
/// 비어 있으면 `dirty` 모드는 **이미지를 만들지 않는다** — 안 바뀐 걸 보여 주지 않는다.
[[nodiscard]] Result<ViewResult> renderView(const LayerTree& tree, const ViewSpec& spec,
                                            const Rect& dirtyArea);

/// 바이트를 base64 로. 줄바꿈을 넣지 않는다(JSON 문자열 한 줄).
[[nodiscard]] std::string base64Encode(const u8* data, usize len);
/// base64 를 바이트로. 깨진 입력은 ParseError. (테스트가 이미지를 되짚을 때 쓴다.)
[[nodiscard]] Result<std::vector<u8>> base64Decode(std::string_view text);

/// 더티 타일 목록을 캔버스 경계 상자로. core 의 것을 그대로 쓴다.
[[nodiscard]] Rect dirtyAreaOf(const DirtyTiles& tiles);

} // namespace mari::agent

#endif // MARI_AGENT_VIEW_HPP
