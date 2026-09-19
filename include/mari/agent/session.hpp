// Mari Paint — 에이전트 세션: 능력 계층의 입구 (docs/05 1절)
//
//                   mari-core  (타일 캔버스 · 스트로크 · 레이어)
//                         │
//               ┌─────────▼──────────┐
//               │   mari-agent-api   │   ← 여기다. **진짜 자산은 이 계층이다**
//               │  세션·스냅샷·시각   │
//               │  피드백·시맨틱주소  │
//               └─────────┬──────────┘
//         ┌───────┬───────┼────────┬─────────┐
//      MCP 서버  COM    CLI      WS/JSON-RPC  UI
//
// MCP·CLI·JSON-RPC 는 전부 이 클래스를 감싸는 **얇은 어댑터**다. 능력을 갖고 있으면 안 된다.
// 사람 UI 도 같은 계층을 쓴다 — AI 전용 뒷문을 만들면 "사람은 되는데 AI 는 안 되는 것"이
// 생기고 기록도 두 갈래가 된다(docs/05 1절).
//
// 🔴 출처(origin) 규약
//    세션은 `AgentStrokeGate` 하나를 들고 있고, 이 세션이 만드는 **모든** 획은
//    그 게이트의 출처를 단다. 그래서 origin 은 **무조건 Agent** 다.
//    API 어디에도 origin 파라미터가 없다 — 고를 수 없으니 거짓말도 못 한다(docs/05 3.1).
//
// 🔴 경계선(docs/03 2절): 여기에 서명도, 해시체인 봉인도, 등급 판정도 없다.
//    출처별 **숫자**까지가 Mari 의 몫이다.
#ifndef MARI_AGENT_SESSION_HPP
#define MARI_AGENT_SESSION_HPP

#include <mari/agent/address.hpp>
#include <mari/agent/capabilities.hpp>
#include <mari/agent/json.hpp>
#include <mari/agent/snapshot.hpp>
#include <mari/agent/stroke_gate.hpp>
#include <mari/agent/view.hpp>
#include <mari/app/application.hpp>
#include <mari/brush/preset.hpp>
#include <mari/core/origin.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mari::agent {

/// 설치된 브러시 한 자루. `brush.list` 가 이걸 그대로 내보낸다.
struct BrushEntry {
    BrushId id = kInvalidBrushId;
    brush::MariBrushPreset preset;
    /// "native" | "abr" | "sut" — 어디서 왔는지. 런타임 발견의 핵심이다(docs/05 2.6).
    std::string source = "native";
    /// 임포트할 때 번역 못 한 것들. 조용히 버리지 않는다.
    std::vector<std::string> notes;
};

/// 필압 프리셋 (docs/05 2.3 — "필압 커브를 AI 가 직접 못 정하면 프리셋도 준다").
enum class PressureProfile : u8 {
    Flat = 0,   ///< 처음부터 끝까지 일정
    TaperIn,    ///< 얇게 시작해 굵게
    TaperOut,   ///< 굵게 시작해 얇게
    TaperInOut, ///< 양 끝이 얇은 붓질(가장 사람 같다)
    Pulse,      ///< 중간이 눌린 강약
};

[[nodiscard]] const char* pressureProfileName(PressureProfile p) noexcept;
[[nodiscard]] Result<PressureProfile> pressureProfileFromName(std::string_view s);
/// 진행도 t(0..1) 에서의 필압 배율 0..1.
[[nodiscard]] f32 pressureProfileAt(PressureProfile p, f32 t) noexcept;

/// 앱 이벤트 값 하나를 JSON 으로. **폴링도 푸시도 이 함수 하나를 쓴다** —
/// 두 벌이 되면 "poll 로는 보이는데 push 로는 안 보이는 필드"가 생긴다.
[[nodiscard]] Json appEventToJson(const app::AppEvent& ev);

/// 세션이 모아 두는 이벤트 한 건(docs/05 2.7 — 같은 이벤트 버스를 쓴다).
struct QueuedEvent {
    std::string kind; ///< "documentOpened" | "paste" | "layerChanged" | ...
    Json data;
};

/// 에이전트 하나의 작업 세션.
///
/// 스레드 규약: 한 세션은 한 스레드가 쓴다(문서·파이프라인과 같은 규약).
class AgentSession {
public:
    /// 🔴 에이전트 식별자 없이는 세션이 열리지 않는다. 익명 AI 획은 만들지 않는다.
    [[nodiscard]] static Result<std::unique_ptr<AgentSession>> open(std::string_view agentId);

    ~AgentSession();
    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;

    // ── 입구 ─────────────────────────────────────────────────────────────

    /// 요청 하나를 처리하고 **봉투까지 씌운** 응답을 돌려준다.
    /// 🔴 절대 던지지 않는다. 실패도 `{"ok":false,"error":{...}}` 라는 **응답**이다.
    [[nodiscard]] Json execute(const Json& request);
    /// JSON 문자열로 받고 문자열로 돌려준다(CLI·stdio 어댑터용).
    [[nodiscard]] std::string executeText(std::string_view requestJson);

    /// 연산 하나를 실행한다(봉투 없음). batch 가 이걸 쓴다.
    [[nodiscard]] Result<Json> runOp(const OpSpec& spec, const Json& request);

    // ── ops 구현이 쓰는 것 ───────────────────────────────────────────────

    /// 🔴 이번 세션의 획 출처. **인자가 없다.** 언제나 Agent 다.
    [[nodiscard]] StrokeSource strokeSource() const noexcept { return gate_.source(); }
    [[nodiscard]] const AgentId& agentId() const noexcept { return gate_.agentId(); }

    [[nodiscard]] app::Application& application() noexcept { return app_; }
    /// 활성 문서. 없으면 nullptr.
    [[nodiscard]] app::Document* document() noexcept;
    [[nodiscard]] const app::Document* document() const noexcept;
    /// 활성 문서가 없으면 NotFound 를 돌려준다.
    [[nodiscard]] Result<app::Document*> requireDocument();

    /// 이번 연산이 바꾼 영역을 알린다(여러 번 부르면 합집합).
    void noteDirty(const Rect& r) noexcept { dirty_ = dirty_.united(r); }
    [[nodiscard]] Rect pendingDirty() const noexcept { return dirty_; }

    // ── 집계: **세 축을 따로 센다**(docs/06 결정 ②) ───────────────────────
    //
    // 🔴 붓질과 영역 연산을 같은 칸에 세지 않는다(docs/06 6절 H3).
    //    캔버스 전체를 칠한 `fill` 한 번을 붓질 한 번으로 세면 "AI 0.05%" 가 되고,
    //    그건 참인 숫자 하나로 만든 거짓이다.
    // 🔴 세 축을 가중합한 단일 "기여도"는 만들지 않는다 — 가중치를 고르는 순간
    //    그것이 판정이고, 판정은 Sigan 의 몫이다(docs/03 2절).

    /// 축 A — 붓질 하나. 출처는 고를 수 없으므로 언제나 Agent 칸이다.
    void countStroke() noexcept { origins_.add(StrokeOrigin::Agent); }
    /// 축 B — 영역 연산 하나(fill·erase·gradient·transform).
    void countRegionOp() noexcept { regionOps_.add(StrokeOrigin::Agent); }
    /// 축 C — 이번 연산이 **실제로 바꾼 타일 수**(픽셀이 아니다).
    void countChangedTiles(u64 n) noexcept { changedTiles_ += n; }

    [[nodiscard]] const StrokeOriginStats& originStats() const noexcept { return origins_; }
    [[nodiscard]] const StrokeOriginStats& regionOpStats() const noexcept { return regionOps_; }
    [[nodiscard]] u64 changedTiles() const noexcept { return changedTiles_; }

    [[nodiscard]] SnapshotStore& snapshots() noexcept { return snaps_; }
    [[nodiscard]] const SnapshotStore& snapshots() const noexcept { return snaps_; }
    [[nodiscard]] RoleTags& roles() noexcept { return roles_; }
    [[nodiscard]] const RoleTags& roles() const noexcept { return roles_; }

    [[nodiscard]] const std::vector<BrushEntry>& brushes() const noexcept { return brushes_; }
    /// 주소(문자열 이름 · 정수 id · null=현재 브러시)로 찾는다.
    [[nodiscard]] Result<const BrushEntry*> resolveBrush(const Json& addr) const;
    [[nodiscard]] BrushId currentBrush() const noexcept { return currentBrush_; }
    void setCurrentBrush(BrushId id) noexcept { currentBrush_ = id; }
    /// 임포트한 브러시를 등록하고 새 id 를 준다.
    BrushId addBrush(brush::MariBrushPreset preset, std::string source,
                     std::vector<std::string> notes);
    /// 등록된 브러시를 지운다(내장은 못 지운다).
    [[nodiscard]] Result<void> removeBrush(BrushId id);
    /// 등록된 브러시의 프리셋을 바꾼다.
    [[nodiscard]] Result<void> updateBrush(BrushId id, brush::MariBrushPreset preset);
    /// 사용자 브러시 폴더(.mbp). 세션이 열릴 때 내장 뒤에 이어서 읽는다.
    [[nodiscard]] const std::string& brushDir() const noexcept { return brushDir_; }
    /// 폴더를 다시 읽어 사용자 브러시를 갱신한다(GUI 가 파일을 바꿨을 때).
    void reloadUserBrushes();

    // ── 이벤트 (docs/05 2.7 · docs/07) ───────────────────────────────────

    /// 폴링 경로(`events.subscribe` → `events.poll`).
    void subscribeEvents(std::vector<std::string> kinds);
    void unsubscribeEvents();
    [[nodiscard]] bool subscribed() const noexcept { return subscribed_; }
    /// 큐에서 최대 max 건을 꺼낸다.
    [[nodiscard]] std::vector<QueuedEvent> drainEvents(usize max);
    [[nodiscard]] usize queuedEventCount() const noexcept { return events_.size(); }
    /// 리스너가 부른다(공개인 이유: EventHub 가 콜백을 이 세션으로 보낸다).
    void pushEvent(std::string kind, Json data);

    // ── 푸시 경로 — 폴링이 아니다 (docs/07) ───────────────────────────────

    /// 이벤트를 **밀어받는** 콜백. `kind` 는 `app::appEventKindName()` 문자열 그대로다.
    using EventPushFn = std::function<void(const std::string& kind, const Json& data)>;

    /// 콜백을 건다. `kinds` 가 비면 전부 받는다. 돌아온 id 로 떼거나 통계를 본다.
    ///
    /// 🔴 콜백은 **다른 스레드에서** 불린다(app/events.hpp ② 관찰 경로).
    ///    세션·문서를 만지지 마라. 느려도 된다 — 느린 만큼 자기 큐만 차고,
    ///    넘치면 **그 구독자만** 요약당한다. 그리기도, Sigan 기록도 멀쩡하다.
    /// 🔴 `events.subscribe`(폴링 큐)와 **독립이다.** 둘을 엮으면 한쪽을 끄는 것이
    ///    다른 쪽을 조용히 끄게 된다.
    [[nodiscard]] u64 subscribePush(std::vector<std::string> kinds, EventPushFn fn,
                                    usize capacity = 256);
    /// 그 구독자의 스레드를 세우고 합류시킨 뒤 돌아온다. 모르는 id 면 false.
    bool unsubscribePush(u64 id);
    [[nodiscard]] usize pushSubscriberCount() const noexcept;
    /// 정직한 숫자: 몇 건 받았고 몇 건이 **이 구독자에게만** 버려졌나.
    [[nodiscard]] app::SubscriberStats pushStats(u64 id) const;

    /// 세션이 처리한 요청 수(벤치·회귀 감시용).
    [[nodiscard]] u64 requestCount() const noexcept { return requests_; }

private:
    class Listener;

    explicit AgentSession(agent::AgentStrokeGate gate);
    void installBuiltinBrushes();
    /// 요청의 키를 표와 대조한다. 모르는 키는 거절이다.
    [[nodiscard]] Result<void> validateParams(const OpSpec& spec, const Json& request) const;

    agent::AgentStrokeGate gate_;
    app::Application app_;
    std::unique_ptr<Listener> listener_;
    SnapshotStore snaps_{256};
    RoleTags roles_;
    std::vector<BrushEntry> brushes_;
    std::string brushDir_;
    std::deque<QueuedEvent> events_;
    std::vector<std::string> eventFilter_;
    std::vector<u64> pushSubs_; ///< 이 세션이 건 푸시 구독. 소멸자가 전부 뗀다
    StrokeOriginStats origins_;
    StrokeOriginStats regionOps_;
    u64 changedTiles_ = 0;
    Rect dirty_{};
    BrushId currentBrush_ = kInvalidBrushId;
    BrushId nextBrushId_ = 1;
    u64 requests_ = 0;
    bool subscribed_ = false;
};

// ── ops 구현들이 나눠 쓰는 도우미 ────────────────────────────────────────

/// 색 표기를 읽는다. `"#RRGGBB"` `"#RRGGBBAA"` `[r,g,b]` `[r,g,b,a]` `{"r":..}`.
[[nodiscard]] Result<Color8> colorFromJson(const Json& v, Color8 fallback);
/// 블렌드 모드 이름 → enum. 모르면 실패(조용히 normal 로 떨어뜨리지 않는다).
[[nodiscard]] Result<BlendMode> blendModeFromName(std::string_view s);
/// 레이어 하나를 JSON 으로(목록·describe 공용).
[[nodiscard]] Json layerToJson(const Layer& layer, const RoleTags& roles, const Size& canvas);
/// 레이어가 캔버스를 얼마나 덮고 있나 0..1. 타일을 훑어 알파>0 픽셀을 센다.
[[nodiscard]] f64 layerCoverage(const Layer& layer, const Size& canvas);

// ── ops 구현 (각 .cpp 가 자기 몫을 정의한다) ─────────────────────────────
namespace ops {
Result<Json> docCreate(AgentSession&, const Json&);
Result<Json> docOpen(AgentSession&, const Json&);
Result<Json> docSave(AgentSession&, const Json&);
Result<Json> docClose(AgentSession&, const Json&);
Result<Json> docDescribe(AgentSession&, const Json&);
Result<Json> docOrigins(AgentSession&, const Json&);
Result<Json> capabilities(AgentSession&, const Json&);

Result<Json> snapshotTake(AgentSession&, const Json&);
Result<Json> snapshotRestore(AgentSession&, const Json&);
Result<Json> snapshotBranch(AgentSession&, const Json&);
Result<Json> snapshotDiff(AgentSession&, const Json&);

Result<Json> layerList(AgentSession&, const Json&);
Result<Json> layerAdd(AgentSession&, const Json&);
Result<Json> layerRemove(AgentSession&, const Json&);
Result<Json> layerMove(AgentSession&, const Json&);
Result<Json> layerDuplicate(AgentSession&, const Json&);
Result<Json> layerMerge(AgentSession&, const Json&);
Result<Json> layerSetProps(AgentSession&, const Json&);
Result<Json> layerFlatten(AgentSession&, const Json&);
Result<Json> layerMask(AgentSession&, const Json&);
Result<Json> layerGroup(AgentSession&, const Json&);
Result<Json> layerUngroup(AgentSession&, const Json&);

Result<Json> stroke(AgentSession&, const Json&);
Result<Json> fill(AgentSession&, const Json&);
Result<Json> bucket(AgentSession&, const Json&);
Result<Json> erase(AgentSession&, const Json&);
Result<Json> gradient(AgentSession&, const Json&);
Result<Json> transform(AgentSession&, const Json&);
Result<Json> adjust(AgentSession&, const Json&);
Result<Json> canvasOp(AgentSession&, const Json&);

Result<Json> select(AgentSession&, const Json&);
Result<Json> selectInvert(AgentSession&, const Json&);
Result<Json> selectExpand(AgentSession&, const Json&);
Result<Json> selectFeather(AgentSession&, const Json&);

Result<Json> brushList(AgentSession&, const Json&);
Result<Json> brushImport(AgentSession&, const Json&);
Result<Json> brushSave(AgentSession&, const Json&);
Result<Json> brushRemove(AgentSession&, const Json&);
Result<Json> brushExport(AgentSession&, const Json&);
Result<Json> brushSet(AgentSession&, const Json&);
Result<Json> brushDescribe(AgentSession&, const Json&);

Result<Json> render(AgentSession&, const Json&);
Result<Json> thumbnail(AgentSession&, const Json&);
Result<Json> compare(AgentSession&, const Json&);

Result<Json> batch(AgentSession&, const Json&);

Result<Json> eventsSubscribe(AgentSession&, const Json&);
Result<Json> eventsUnsubscribe(AgentSession&, const Json&);
Result<Json> eventsPoll(AgentSession&, const Json&);
} // namespace ops

} // namespace mari::agent

#endif // MARI_AGENT_SESSION_HPP
