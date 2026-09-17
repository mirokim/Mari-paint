// Mari Paint — 시맨틱 주소 (docs/05 2.4 — "좌표를 외우게 하지 않는다")
//
// AI 에게 "레이어 인덱스 3" 을 기억시키면 안 된다. 이름과 내용으로 지목하게 한다.
//
//     { "layer": "선화" }                                   // 이름
//     { "layer": { "role": "sketch" } }                     // 역할 태그
//     { "layer": "active" }                                 // 지금 편집 중인 것
//     { "region": { "content": "nonEmpty", "layer": "선화" } } // 내용이 있는 영역
//     { "region": { "selection": "current" } }
//
// 🔴 역할(role)은 **코어에 없는 개념이다.** core 의 Layer 에는 role 필드가 없고,
//    .ora 에도 그런 칸이 없다. 그래서 역할은 이 세션이 들고 있는 태그이고,
//    이름에서 유추도 한다("선화" → lineart). 없는 필드를 있는 척하지 않으려고
//    `layer.list` 응답에 역할의 출처(`roleSource`: "tag" | "name" | "")를 같이 적는다.
#ifndef MARI_AGENT_ADDRESS_HPP
#define MARI_AGENT_ADDRESS_HPP

#include <mari/agent/json.hpp>
#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>

#include <string>
#include <unordered_map>

namespace mari::agent {

/// 레이어 역할 태그. 이름 문자열이 정본이다(자유 문자열이 아니라 이 목록이다).
enum class LayerRole : u8 {
    None = 0,
    Background, ///< 배경
    Sketch,     ///< 러프·스케치
    Lineart,    ///< 선화
    Color,      ///< 채색
    Shading,    ///< 명암
    Effects,    ///< 효과
    Text,       ///< 텍스트
};

[[nodiscard]] const char* layerRoleName(LayerRole r) noexcept;
/// 문자열 → 역할. 모르면 None.
[[nodiscard]] LayerRole layerRoleFromName(std::string_view s) noexcept;
/// 레이어 **이름**에서 역할을 유추한다(한국어·영어). 못 찾으면 None.
/// 🔴 유추일 뿐이다. 응답에 "name" 출처로 표시해서 태그와 구분한다.
[[nodiscard]] LayerRole guessRoleFromLayerName(std::string_view layerName) noexcept;

/// 역할 태그 보관함. 세션이 들고 있고 문서에 저장되지 않는다.
class RoleTags {
public:
    void set(LayerId id, LayerRole role);
    void erase(LayerId id);
    /// 태그만 본다(이름 유추는 안 한다). 없으면 None.
    [[nodiscard]] LayerRole tagged(LayerId id) const;
    /// 태그 → 없으면 이름 유추. `source` 에 "tag"/"name"/"" 를 적는다.
    [[nodiscard]] LayerRole effective(const Layer& layer, const char** source = nullptr) const;
    [[nodiscard]] usize size() const noexcept { return tags_.size(); }

private:
    std::unordered_map<LayerId, LayerRole> tags_;
};

/// 레이어 주소를 푼다.
///
/// 받는 형태:
///   · 문자열 `"선화"` — 이름이 정확히 일치하는 레이어. `"active"` 는 활성 레이어.
///   · 정수 `12` — 레이어 id(권장하지 않지만 막지는 않는다).
///   · 객체 `{"id":12}` `{"name":"선화"}` `{"role":"lineart"}` `{"index":0}`
///
/// 이름/역할이 **여러 개에 걸리면 실패한다.** 애매한 지목을 조용히 하나 골라 주면
/// AI 가 엉뚱한 레이어에 그리고도 모른다.
[[nodiscard]] Result<LayerId> resolveLayer(const LayerTree& tree, const RoleTags& roles,
                                           const Json& addr);

/// 영역 주소를 푼다.
///
///   · `[x,y,w,h]` — 그대로
///   · `"canvas"` — 캔버스 전체, `"selection"` — 현재 선택
///   · `{"rect":[x,y,w,h]}`
///   · `{"selection":"current"}`
///   · `{"content":"nonEmpty","layer":<주소>}` — 그 레이어에 내용이 있는 영역
///   · `{"content":"all"}` — 모든 레이어 내용의 합집합
///
/// `fallback` 은 주소가 없을 때(`null`) 쓸 기본 영역이다.
[[nodiscard]] Result<Rect> resolveRegion(const LayerTree& tree, const RoleTags& roles,
                                         const Json& addr, const Rect& selection,
                                         const Rect& fallback);

} // namespace mari::agent

#endif // MARI_AGENT_ADDRESS_HPP
