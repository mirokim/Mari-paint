// Mari Paint — 연산 표 + capabilities. 선언은 include/mari/agent/capabilities.hpp.
//
// 🔴 표 하나가 세 가지를 동시에 한다:
//    (1) 디스패치 대상 목록  (2) 파라미터 스키마(모르는 키 거절)  (3) MCP 도구 생성 원본.
//    셋이 같은 표에서 나오니 어긋날 수가 없다 — 이게 docs/05 2.6 의 요구다.
#include <mari/agent/capabilities.hpp>

#include <mari/agent/session.hpp>

namespace mari::agent {
namespace {

using P = ParamSpec;

/// 모든 그리기 연산이 공유하는 대상 지정 파라미터.
P layerParam() {
    return P{"layer", "layer", false,
             "대상 레이어. 이름·id·{role:...}·\"active\". 없으면 활성 레이어"};
}

std::vector<OpSpec> makeTable() {
    std::vector<OpSpec> t;

    // ── 문서 ─────────────────────────────────────────────────────────────
    t.push_back(OpSpec{"doc.create",
                       "doc",
                       "새 문서를 만들고 활성화한다(래스터 레이어 한 장 포함)",
                       true,
                       "",
                       true,
                       ViewMode::None,
                       {P{"width", "int", true, "캔버스 폭(px). 1..16384"},
                        P{"height", "int", true, "캔버스 높이(px). 1..16384"}},
                       &ops::docCreate});
    t.push_back(OpSpec{"doc.open",
                       "doc",
                       ".ora 파일을 연다(다른 확장자는 추측해서 열지 않는다)",
                       true,
                       "",
                       true,
                       ViewMode::None,
                       {P{"path", "string", true, "열 파일 경로(.ora)"}},
                       &ops::docOpen});
    t.push_back(OpSpec{"doc.save",
                       "doc",
                       "문서를 저장한다. 파일 해시(SHA-256)를 같이 돌려준다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"path", "string", false, "저장 경로. 없으면 원래 경로"},
                        P{"format", "string", false, "ora | png (psd 는 미지원)"}},
                       &ops::docSave});
    t.push_back(OpSpec{"doc.close",
                       "doc",
                       "활성 문서를 닫는다. **기본은 저장하지 않는다**",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"save", "bool", false, "닫기 전에 저장할지. 기본 false"}},
                       &ops::docClose});
    t.push_back(OpSpec{"doc.describe",
                       "doc",
                       "🔴 캔버스 상태를 **사람이 읽는 한 문장**으로. JSON 트리보다 싸고 정확하다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {},
                       &ops::docDescribe});
    t.push_back(OpSpec{"doc.origins",
                       "doc",
                       "🔴 출처별 집계를 **세 축**으로 돌려준다: 붓질 수 · 영역 연산 수 · "
                       "변경된 타일 수. 숫자뿐이고 등급은 없다(판정은 Sigan 의 몫이다)",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {},
                       &ops::docOrigins});
    t.push_back(OpSpec{"capabilities",
                       "doc",
                       "연산·브러시·블렌드모드·한계를 런타임에 돌려준다(MCP 도구 목록의 원본)",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {},
                       &ops::capabilities});

    // ── 스냅샷 (docs/05 2.2) ─────────────────────────────────────────────
    t.push_back(OpSpec{"snapshot",
                       "snapshot",
                       "🔴 O(1) 스냅샷. 픽셀을 복사하지 않는다(COW 타일)",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"label", "string", false, "이름표. 나중에 이 이름으로 restore 할 수 있다"}},
                       &ops::snapshotTake});
    t.push_back(OpSpec{"restore",
                       "snapshot",
                       "🔴 O(1) 복원. 지워졌던 레이어도 **같은 id 로** 돌아온다",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {P{"snapshot", "string|int", false,
                          "스냅샷 id 또는 이름표. 없으면 가장 최근 것"}},
                       &ops::snapshotRestore});
    t.push_back(OpSpec{"branch",
                       "snapshot",
                       "스냅샷에서 갈라져 나온 새 갈래를 연다(복원 + 새 스냅샷)",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {P{"snapshot", "string|int", false, "출발점. 없으면 가장 최근 것"},
                        P{"label", "string", false, "새 갈래의 이름표"}},
                       &ops::snapshotBranch});
    t.push_back(OpSpec{"diff",
                       "snapshot",
                       "두 상태의 차이. 픽셀이 아니라 **타일 포인터**를 비교한다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"from", "string|int", true, "기준 스냅샷"},
                        P{"to", "string|int", false, "비교 대상. 없으면 지금 상태"}},
                       &ops::snapshotDiff});

    // ── 레이어 ───────────────────────────────────────────────────────────
    t.push_back(OpSpec{"layer.list", "layer", "레이어 목록(역할·채움 비율 포함)", true, "", false,
                       ViewMode::None, {}, &ops::layerList});
    t.push_back(OpSpec{"layer.add",
                       "layer",
                       "레이어를 추가한다",
                       true,
                       "",
                       true,
                       ViewMode::None,
                       {P{"name", "string", false, "이름. 없으면 자동"},
                        P{"kind", "string", false, "raster | group. 기본 raster"},
                        P{"parent", "layer", false, "부모 그룹. 없으면 루트"},
                        P{"index", "int", false, "형제 중 위치(0=가장 아래). 없으면 맨 위"},
                        P{"role", "string", false,
                          "역할 태그(background|sketch|lineart|color|shading|effects|text)"}},
                       &ops::layerAdd});
    t.push_back(OpSpec{"layer.remove",
                       "layer",
                       "레이어를 지운다(스냅샷이 있으면 되돌릴 수 있다)",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam()},
                       &ops::layerRemove});
    t.push_back(OpSpec{"layer.move",
                       "layer",
                       "레이어를 다른 부모·위치로 옮긴다",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"parent", "layer", false, "새 부모. 없으면 루트"},
                        P{"index", "int", false, "새 위치(0=가장 아래)"}},
                       &ops::layerMove});
    t.push_back(OpSpec{"layer.duplicate",
                       "layer",
                       "레이어를 복제한다. **픽셀을 복사하지 않는다 — O(1)**",
                       true,
                       "",
                       true,
                       ViewMode::None,
                       {layerParam(), P{"name", "string", false, "복제본 이름"}},
                       &ops::layerDuplicate});
    t.push_back(OpSpec{"layer.merge",
                       "layer",
                       "레이어를 바로 아래 레이어로 합친다(합성 결과를 굽는다)",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam()},
                       &ops::layerMerge});
    t.push_back(OpSpec{"layer.setProps",
                       "layer",
                       "레이어 속성을 바꾼다",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"name", "string", false, "새 이름"},
                        P{"opacity", "number", false, "0..1"},
                        P{"blendMode", "string", false, "normal|multiply|... (capabilities 참고)"},
                        P{"visible", "bool", false, ""}, P{"locked", "bool", false, ""},
                        P{"alphaLocked", "bool", false, "기존에 불투명한 픽셀만 고친다"},
                        P{"clipToBelow", "bool", false, "아래 레이어의 알파 안에서만 보인다(클리핑)"},
                        P{"role", "string", false, "역할 태그(세션 한정. 파일에 저장되지 않는다)"},
                        P{"active", "bool", false, "true 면 이 레이어를 활성으로"}},
                       &ops::layerSetProps});
    t.push_back(OpSpec{"layer.flatten",
                       "layer",
                       "보이는 레이어 전부를 하나로 평탄화한다(실행취소 한 번)",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {},
                       &ops::layerFlatten});
    t.push_back(OpSpec{"layer.mask",
                       "layer",
                       "레이어 마스크. add(전부 보임) · fromSelection · reveal/hide(선택을 마스크에 칠함) · apply · remove",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"action", "string", false, "add|fromSelection|reveal|hide|apply|remove. 기본 add"}},
                       &ops::layerMask});
    t.push_back(OpSpec{"layer.group",
                       "layer",
                       "레이어를 새 그룹으로 감싼다",
                       true,
                       "",
                       true,
                       ViewMode::None,
                       {layerParam(), P{"name", "string", false, "그룹 이름"}},
                       &ops::layerGroup});
    t.push_back(OpSpec{"layer.ungroup",
                       "layer",
                       "그룹을 풀어 자식을 제자리에 놓는다",
                       true,
                       "",
                       true,
                       ViewMode::None,
                       {layerParam()},
                       &ops::layerUngroup});

    // ── 그리기 (docs/05 2.3 — 스트로크로 그린다) ─────────────────────────
    t.push_back(
        OpSpec{"stroke",
               "draw",
               "🔴 점+필압 배열을 **사람 펜과 같은 파이프라인**에 태운다. origin 은 언제나 Agent",
               true,
               "",
               true,
               ViewMode::Dirty,
               {layerParam(),
                P{"points", "array", true,
                  "[{x,y,p?,tx?,ty?,t?}...] 또는 [[x,y]...]. p 필압 0..1(없으면 pressureProfile) · "
                  "tx/ty 기울기 -1..1 · t 획 시작 기준 ms(속도 반응용)"},
                P{"brush", "string|int", false, "브러시 이름·id. 없으면 현재 브러시"},
                P{"color", "color", false, "\"#RRGGBB\" | [r,g,b,a]. 기본 검정"},
                P{"background", "color", false, "배경색 — 브러시 색 변화(전경↔배경 지터)가 쓴다. 기본 흰색"},
                P{"size", "number", false, "팁 지름(px). 없으면 브러시 값"},
                P{"spacing", "number", false, "스탬프 간격(지름 대비 비율)"},
                P{"opacity", "number", false, "0..1"},
                P{"smoothing", "number", false, "떨림 보정 0..1. 기본 0(끔)"},
                P{"deadZone", "number", false, "데드존(캔버스 px, 끈에 매단 펜). 보정이 켜졌을 때만. 기본 0"},
                P{"endCorrection", "bool", false, "펜을 뗀 자리까지 이어 그린다. 기본 true"},
                P{"pressureProfile", "string", false,
                  "flat|taper-in|taper-out|taper-in-out|pulse. p 가 없는 점에 적용"},
                P{"eraser", "bool", false, "지우개 모드"},
                P{"seed", "int", false, "난수 시드. 같은 시드+같은 입력 = 같은 획"}},
               &ops::stroke});
    t.push_back(OpSpec{"bucket",
                       "draw",
                       "페인트통 — 클릭한 점과 이어진 같은 색 영역을 채운다(선택 마스크 안에서, 소스 오버)",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"at", "array", true, "[x, y] 시작점"},
                        P{"color", "color", false, "기본 검정"},
                        P{"tolerance", "int", false, "채널당 허용 오차 0..255. 기본 32"},
                        P{"gapClose", "int", false, "선화 틈 닫기(px) 0..64. 기본 0"},
                        P{"sample", "layer", false, "영역을 판정할 레이어(선화). 기본은 칠할 레이어"},
                        P{"eraser", "bool", false, "채우는 대신 지운다"}},
                       &ops::bucket});
    t.push_back(OpSpec{"fill",
                       "draw",
                       "영역을 색으로 채운다(합성이 아니라 덮어쓰기)",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(),
                        P{"region", "region", false,
                          "[x,y,w,h] | \"canvas\" | \"selection\" | {content:...}. 기본 선택 영역"},
                        P{"color", "color", false, "채울 색. 기본 검정"}},
                       &ops::fill});
    t.push_back(OpSpec{"erase",
                       "draw",
                       "영역을 투명하게 지운다",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"region", "region", false, "기본 선택 영역"}},
                       &ops::erase});
    t.push_back(OpSpec{"gradient",
                       "draw",
                       "선형 그라데이션을 채운다",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"region", "region", false, "기본 선택 영역"},
                        P{"from", "color", true, "시작 색"}, P{"to", "color", true, "끝 색"},
                        P{"angle", "number", false, "각도(도). 0 = 왼→오른쪽. 기본 0"}},
                       &ops::gradient});
    t.push_back(OpSpec{"transform",
                       "draw",
                       "자유 변형(GUI Ctrl+T 와 같은 함수): 이동·확대·회전·뒤집기. 선택이 있으면 선택 안만",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(), P{"dx", "number", false, "가로 이동(px)"},
                        P{"dy", "number", false, "세로 이동(px)"},
                        P{"scale", "number", false, "배율(가로세로 같이)"},
                        P{"scaleX", "number", false, ""}, P{"scaleY", "number", false, ""},
                        P{"rotate", "number", false, "회전(도, 시계 방향)"},
                        P{"pivot", "array", false, "[x,y] 회전·확대 중심. 없으면 내용 중심"},
                        P{"flipH", "bool", false, ""}, P{"flipV", "bool", false, ""},
                        P{"interpolation", "string", false, "bilinear(기본) | nearest"}},
                       &ops::transform});
    t.push_back(OpSpec{"adjust",
                       "draw",
                       "색 보정(GUI 이미지 › 보정 과 같은 함수). 선택 안만",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {layerParam(),
                        P{"kind", "string", true, "hsl | brightnessContrast | levels | curves | invert | desaturate | threshold | posterize"},
                        P{"hue", "number", false, "hsl: −180..180"}, P{"saturation", "number", false, "hsl: −100..100"},
                        P{"lightness", "number", false, "hsl: −100..100"},
                        P{"brightness", "number", false, "−100..100"}, P{"contrast", "number", false, "−100..100"},
                        P{"inBlack", "int", false, "levels 0..255"}, P{"inWhite", "int", false, "levels 0..255"},
                        P{"gamma", "number", false, "levels 0.1..10"}, P{"outBlack", "int", false, ""}, P{"outWhite", "int", false, ""},
                        P{"curve", "array", false, "curves: [[x,y],...] 0..1 마스터"},
                        P{"curveR", "array", false, ""}, P{"curveG", "array", false, ""}, P{"curveB", "array", false, ""},
                        P{"threshold", "int", false, "0..255"}, P{"levels", "int", false, "posterize 2..255"}},
                       &ops::adjust});
    t.push_back(OpSpec{"canvas",
                       "draw",
                       "캔버스 연산(모든 레이어, 실행취소 하나): flip | rotate | resize | scale | crop",
                       true,
                       "",
                       true,
                       ViewMode::Full,
                       {P{"action", "string", true, "flip | rotate | resize | scale | crop"},
                        P{"axis", "string", false, "flip: horizontal(기본) | vertical"},
                        P{"degrees", "int", false, "rotate: 90 의 배수(시계)"},
                        P{"width", "int", false, "resize/scale"}, P{"height", "int", false, "resize/scale"},
                        P{"anchorX", "int", false, "resize: 0 왼쪽 · 1 가운데 · 2 오른쪽"},
                        P{"anchorY", "int", false, "resize: 0 위 · 1 가운데 · 2 아래"},
                        P{"region", "region", false, "crop: 없으면 선택 경계"}},
                       &ops::canvasOp});

    // ── 선택 ─────────────────────────────────────────────────────────────
    t.push_back(OpSpec{"select",
                       "select",
                       "선택 영역을 정한다. 사각형·타원·올가미·색상 범위·내용·레이어 알파, "
                       "그리고 불린 결합까지",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"mode", "string", false,
                          "rect | ellipse | lasso | color | wand | content | alpha. 기본 rect"},
                        P{"at", "array", false, "wand 의 시작점 [x, y]"},
                        P{"gapClose", "int", false, "wand 의 틈 닫기(px) 0..64"},
                        P{"region", "region", false,
                          "rect·ellipse 가 쓴다. 아무 것도 안 주면 선택을 해제한다(= 제한 없음)"},
                        P{"points", "array", false, "lasso 의 폴리곤. [[x,y],...] 또는 [{x,y},...]"},
                        P{"layer", "layer", false, "color·content·alpha 를 뽑아 올 레이어"},
                        P{"color", "color", false, "color 모드의 기준색"},
                        P{"tolerance", "int", false, "color 모드의 채널당 허용 오차 0..255"},
                        P{"threshold", "int", false, "content 모드의 알파 문턱 0..255. 기본 1"},
                        P{"antialias", "bool", false, "타원·올가미 가장자리를 부드럽게. 기본 true"},
                        P{"feather", "number", false, "만든 직후 먹일 페더 반지름(px)"},
                        P{"combine", "string", false,
                          "replace | add | subtract | intersect | xor. 기본 replace. "
                          "🔴 이름이 op 가 아닌 이유: op 는 연산 이름 자리다"}},
                       &ops::select});
    t.push_back(OpSpec{"select.invert",
                       "select",
                       "선택을 반전한다. 타일 수가 늘지 않는다(캔버스를 할당하지 않는다)",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {},
                       &ops::selectInvert});
    t.push_back(OpSpec{"select.expand",
                       "select",
                       "선택을 **원형으로** 키우거나 줄인다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"by", "int", true, "px. 음수면 줄인다. 모서리가 둥글게 처리된다"}},
                       &ops::selectExpand});
    t.push_back(OpSpec{"select.feather",
                       "select",
                       "선택 경계를 가우시안으로 부드럽게 한다(σ = radius/2)",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"radius", "number", true, "px"}},
                       &ops::selectFeather});

    // ── 브러시 ───────────────────────────────────────────────────────────
    t.push_back(OpSpec{"brush.list", "brush", "설치된 브러시를 **런타임에** 열거한다", true, "",
                       false, ViewMode::None, {}, &ops::brushList});
    t.push_back(OpSpec{"brush.import",
                       "brush",
                       ".abr(Photoshop) / .sut(Clip Studio) 를 들여온다. 번역 못 한 것은 notes 에",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"path", "string", true, "브러시 파일 경로"}},
                       &ops::brushImport});
    t.push_back(OpSpec{"brush.save",
                       "brush",
                       "브러시를 사용자 폴더에 .mbp 로 저장한다(같은 이름은 덮어쓴다). preset 을 주면 그 내용을 저장",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"brush", "string|int", false, "저장할 등록 브러시(preset 이 없을 때)"},
                        P{"preset", "object", false, "brush.export 가 준 형식의 프리셋(수정본)"},
                        P{"name", "string", false, "저장 이름(없으면 프리셋 이름)"}},
                       &ops::brushSave});
    t.push_back(OpSpec{"brush.remove",
                       "brush",
                       "사용자 브러시를 지운다(파일도). 내장은 못 지운다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"brush", "string|int", true, "브러시 이름·id"}},
                       &ops::brushRemove});
    t.push_back(OpSpec{"brush.export",
                       "brush",
                       "브러시 프리셋 전체를 JSON(.mbp 형식)으로 준다 — 고쳐서 brush.save 로 돌려줄 수 있다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"brush", "string|int", false, "브러시 이름·id. 없으면 현재 브러시"}},
                       &ops::brushExport});
    t.push_back(OpSpec{"brush.set",
                       "brush",
                       "현재 브러시를 고른다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"brush", "string|int", true, "브러시 이름 또는 id"}},
                       &ops::brushSet});
    t.push_back(OpSpec{"brush.describe",
                       "brush",
                       "브러시 하나의 팁·간격·동적 반응을 자세히",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"brush", "string|int", false, "없으면 현재 브러시"}},
                       &ops::brushDescribe});

    // ── 시각 ─────────────────────────────────────────────────────────────
    t.push_back(OpSpec{"render",
                       "visual",
                       "지금 캔버스를 이미지로 돌려준다(기본 view 는 full)",
                       true,
                       "",
                       false,
                       ViewMode::Full,
                       {},
                       &ops::render});
    t.push_back(OpSpec{"thumbnail",
                       "visual",
                       "작은 미리보기 한 장",
                       true,
                       "",
                       false,
                       ViewMode::Full,
                       {P{"max", "int", false, "긴 변 상한. 기본 128"}},
                       &ops::thumbnail});
    t.push_back(OpSpec{"compare",
                       "visual",
                       "스냅샷과 지금을 견준다(차이 영역 + 그 영역 이미지)",
                       true,
                       "",
                       false,
                       ViewMode::Dirty,
                       {P{"snapshot", "string|int", true, "견줄 스냅샷"}},
                       &ops::compare});

    // ── 일괄 (docs/05 2.5) ───────────────────────────────────────────────
    t.push_back(OpSpec{"batch",
                       "batch",
                       "🔴 여러 연산을 한 번에. atomic 이면 **하나라도 실패하면 전부 롤백**",
                       true,
                       "",
                       true,
                       ViewMode::Dirty,
                       {P{"ops", "array", true, "연산 객체 배열. 각 원소는 {op:...} 하나"},
                        P{"atomic", "bool", false, "기본 true. 실패 시 스냅샷으로 되돌린다"},
                        P{"stopOnError", "bool", false,
                          "atomic 이 false 일 때 첫 실패에서 멈출지. 기본 true"}},
                       &ops::batch});

    // ── 이벤트 (docs/05 2.7) ─────────────────────────────────────────────
    t.push_back(OpSpec{"events.subscribe",
                       "event",
                       "앱 이벤트를 세션 큐에 모으기 시작한다",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"events", "array", false, "받을 종류 이름 배열. 없으면 전부"}},
                       &ops::eventsSubscribe});
    t.push_back(OpSpec{"events.unsubscribe", "event", "이벤트 수집을 멈추고 큐를 비운다", true, "",
                       false, ViewMode::None, {}, &ops::eventsUnsubscribe});
    t.push_back(OpSpec{"events.poll",
                       "event",
                       "모인 이벤트를 꺼낸다(요청/응답만으로는 밀어 줄 수 없어서 당겨 간다)",
                       true,
                       "",
                       false,
                       ViewMode::None,
                       {P{"max", "int", false, "최대 건수. 기본 64"}},
                       &ops::eventsPoll});

    return t;
}

Json paramsToJson(const OpSpec& op) {
    Json arr = Json::array();
    for (const ParamSpec& p : op.params) {
        Json j = Json::object();
        j.set("name", Json::string(p.name));
        j.set("type", Json::string(p.type));
        j.set("required", Json::boolean(p.required));
        j.set("desc", Json::string(p.desc));
        arr.push(std::move(j));
    }
    // 🔴 전 연산이 시각 피드백을 받는다(docs/05 2.1). 표에도 그렇게 적힌다.
    Json v = Json::object();
    v.set("name", Json::string(kUniversalViewParam));
    v.set("type", Json::string("view"));
    v.set("required", Json::boolean(false));
    v.set("desc", Json::string("none | dirty | full | layer:<id> | region:[x,y,w,h] "
                               "또는 {mode,max}. 기본은 이 연산의 defaultView"));
    arr.push(std::move(v));
    return arr;
}

} // namespace

const std::vector<OpSpec>& opTable() {
    static const std::vector<OpSpec> kTable = makeTable();
    return kTable;
}

const OpSpec* findOp(std::string_view name) {
    for (const OpSpec& op : opTable()) {
        if (name == op.name) {
            return &op;
        }
    }
    return nullptr;
}

Json buildCapabilities(const AgentSession& session) {
    Json out = Json::object();
    out.set("schema", Json::string(kApiSchema));
    out.set("version", Json::string(kApiVersion));

    Json ops = Json::array();
    for (const OpSpec& op : opTable()) {
        Json j = Json::object();
        j.set("name", Json::string(op.name));
        j.set("group", Json::string(op.group));
        j.set("summary", Json::string(op.summary));
        j.set("supported", Json::boolean(op.supported));
        if (!op.supported) {
            j.set("unsupportedReason", Json::string(op.unsupportedReason));
        }
        j.set("mutates", Json::boolean(op.mutates));
        j.set("defaultView", Json::string(viewModeName(op.defaultView)));
        j.set("params", paramsToJson(op));
        ops.push(std::move(j));
    }
    out.set("ops", std::move(ops));

    Json view = Json::object();
    Json modes = Json::array();
    modes.push(Json::string("none"));
    modes.push(Json::string("dirty"));
    modes.push(Json::string("full"));
    modes.push(Json::string("layer:<id>"));
    modes.push(Json::string("region:[x,y,w,h]"));
    view.set("modes", std::move(modes));
    view.set("default", Json::string("dirty"));
    view.set("maxDefault", Json::integer(static_cast<i64>(kDefaultViewMax)));
    view.set("format", Json::string("png"));
    view.set("encoding", Json::string("base64"));
    out.set("view", std::move(view));

    Json brushes = Json::array();
    for (const BrushEntry& b : session.brushes()) {
        Json j = Json::object();
        j.set("id", Json::integer(b.id));
        j.set("name", Json::string(b.preset.name));
        j.set("engine", Json::string(b.preset.engine.empty() ? "native" : b.preset.engine));
        j.set("source", Json::string(b.source));
        j.set("diameter", Json::number(static_cast<f64>(b.preset.tip.diameter)));
        j.set("spacing", Json::number(static_cast<f64>(b.preset.spacing)));
        brushes.push(std::move(j));
    }
    out.set("brushes", std::move(brushes));
    out.set("currentBrush", Json::integer(session.currentBrush()));

    Json blends = Json::array();
    for (int i = 0; i <= static_cast<int>(BlendMode::Erase); ++i) {
        blends.push(Json::string(blendModeName(static_cast<BlendMode>(i))));
    }
    out.set("blendModes", std::move(blends));

    Json profiles = Json::array();
    for (int i = 0; i <= static_cast<int>(PressureProfile::Pulse); ++i) {
        profiles.push(Json::string(pressureProfileName(static_cast<PressureProfile>(i))));
    }
    out.set("pressureProfiles", std::move(profiles));

    Json roles = Json::array();
    for (int i = 1; i <= static_cast<int>(LayerRole::Text); ++i) {
        roles.push(Json::string(layerRoleName(static_cast<LayerRole>(i))));
    }
    out.set("layerRoles", std::move(roles));

    const ApiLimits lim;
    Json limits = Json::object();
    limits.set("maxCanvas", Json::integer(lim.maxCanvas));
    limits.set("maxBatchOps", Json::integer(lim.maxBatchOps));
    limits.set("maxStrokePoints", Json::integer(lim.maxStrokePoints));
    limits.set("maxImageSide", Json::integer(lim.maxImageSide));
    limits.set("maxSnapshots", Json::integer(static_cast<i64>(session.snapshots().limit())));
    limits.set("tileSize", Json::integer(lim.tileSize));
    limits.set("agentIdMaxLen", Json::integer(lim.agentIdMaxLen));
    out.set("limits", std::move(limits));

    // 🔴 출처는 협상 대상이 아니다. 무엇으로 고정돼 있는지 **밝히기만** 한다.
    Json origin = Json::object();
    origin.set("forced", Json::string(strokeOriginName(StrokeOrigin::Agent)));
    origin.set("agentId", Json::string(std::string(session.agentId().view())));
    origin.set("settable", Json::boolean(false));
    origin.set("note",
               Json::string("이 API 로 들어온 획은 무조건 Agent 다. origin 을 고르는 "
                            "파라미터는 어느 연산에도 없다(docs/05 3.1)"));
    out.set("origin", std::move(origin));

    out.set("headless", Json::boolean(true));
    return out;
}

} // namespace mari::agent
