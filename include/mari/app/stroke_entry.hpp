// Mari Paint — 획이 기록으로 흘러가는 **유일한 입구** (docs/06 결정 ③)
//
// 🔴 이 파일이 존재하는 단 하나의 이유:
//    사람 획(WM_POINTER)과 AI 획(agent-api · MCP · CLI)이 **같은 코드**를 지나게 하는 것.
//    GUI 가 붙는 날 여기 말고 다른 곳에 발행 코드를 한 줄이라도 새로 쓰게 된다면
//    그건 기록이 두 갈래로 갈라졌다는 뜻이고, 설계가 어긋난 신호다(docs/05 1절).
//
//        [사람: WM_POINTER]            [AI: agent-api / MCP / CLI]
//              │                                │
//              │  StrokeSource::humanPen()      │  AgentStrokeGate::source()
//              ▼                                ▼
//          ┌──────────────────────────────────────────┐
//          │  app::StrokeEntry(doc, source, ...)      │  ← 출처는 **받기만** 한다
//          └──────────────────┬───────────────────────┘
//                             ▼
//               agent::IStrokeRecorder  (구현체 1개: record/)
//                             ▼
//                      sigan::SiganPublisher
//
// 🔴 위조 방지: 여기에 출처를 만드는 길이 없다. `const StrokeSource&` 를 받아 그대로
//    들고 다닐 뿐이다. `StrokeSource` 는 기본 생성자도 공개 생성자도 없으므로
//    (core/origin.hpp) 호출자는 팩토리나 게이트가 준 것을 넘기는 수밖에 없다.
//
// 🔴 경계선(docs/03 2절): 서명도 해시체인도 등급도 없다. 개수와 타일 수뿐이다.
#ifndef MARI_APP_STROKE_ENTRY_HPP
#define MARI_APP_STROKE_ENTRY_HPP

#include <mari/agent/recording.hpp>
#include <mari/app/document.hpp>
#include <mari/core/origin.hpp>

namespace mari::app {

/// 펜(또는 마우스, 또는 AI)이 낸 점 하나. WM_POINTER 가 붙으면 그쪽이 이 모양으로 정규화해
/// 넘긴다 — 그때 새로 쓸 발행 코드는 **0줄**이다(docs/06 결정 ③ 규약 3).
struct PenSample {
    PointF pos{};          ///< 캔버스 좌표(줌·회전·팬 불변)
    f32 pressure = 0.0f;   ///< 0..1. 마우스라면 1.0 을 넣지 말고 실제 값을 넣어라
    f32 tiltX = 0.0f;
    f32 tiltY = 0.0f;
    f32 rotation = 0.0f;
    f32 velocity = 0.0f;
};

/// 획 하나를 기록으로 흘려보내는 입구.
///
/// 수명: 획 하나 동안만 산다. 문서보다 오래 살면 안 된다.
/// 스레드: 그리기 스레드 하나가 독점한다(`SiganPublisher` 와 같은 규약).
///
/// 레코더가 없으면(= Sigan 도 로컬 저널도 안 쓰기로 한 빌드) 전부 조용한 no-op 이다.
/// **그래도 그리기는 된다** — Sigan 미설치는 정상 상태다(docs/03 5.1).
class StrokeEntry {
public:
    /// 🔴 출처를 **인자로 받는다. 고르지 않는다.**
    StrokeEntry(Document& doc, const StrokeSource& src, LayerId layerId, BrushId brushId,
                bool eraser) noexcept;

    StrokeEntry(const StrokeEntry&) = delete;
    StrokeEntry& operator=(const StrokeEntry&) = delete;

    void down(const PenSample& s) noexcept; ///< 획 시작
    void move(const PenSample& s) noexcept; ///< 획 진행
    void up(const PenSample& s) noexcept;   ///< 획 끝(프레임까지만)

    /// 획이 실제로 바꾼 타일 수를 알리고 저널을 flush 한다(축 A·C 가 여기서 오른다).
    /// `up()` 뒤에 **정확히 한 번** 부른다.
    void finish(u32 changedTiles) noexcept;

    /// 기록이 붙어 있나(널 레코더가 아닌가).
    [[nodiscard]] bool recording() const noexcept { return rec_ != nullptr; }
    /// 기록하기로 해 놓고 **못 하고 있는가**(docs/06 결정 ④). 호출자는 롤백해야 한다.
    [[nodiscard]] bool broken() const noexcept;
    /// 이 입구가 들고 있는 출처. 보고용이다 — 바꾸는 길은 없다.
    [[nodiscard]] const StrokeSource& source() const noexcept { return src_; }

private:
    void emit(const PenSample& s, u32 flags) noexcept;

    agent::IStrokeRecorder* rec_ = nullptr;
    StrokeSource src_;
    LayerId layerId_ = kInvalidLayerId;
    BrushId brushId_ = kInvalidBrushId;
    bool eraser_ = false;
    bool ended_ = false;
};

/// 영역을 직접 쓰는 연산 하나를 기록한다(docs/06 결정 ①) — 합성 프레임 **쌍**으로 나간다.
///
/// 🔴 돌려주는 값을 무시하지 마라. 실패는 "저널이 고장 났다"는 뜻이고,
///    그때 호출자는 그 연산을 **undo 로 롤백해야 한다**(docs/06 결정 ④).
///    파이프(싱크)가 끊긴 것은 실패가 아니다 — 저널에 스풀되고 Ok 가 온다.
[[nodiscard]] Result<void> recordRegionOp(Document& doc, const StrokeSource& src,
                                          agent::RegionOpKind kind, const Rect& area,
                                          LayerId layerId, bool eraser, u32 changedTiles);

} // namespace mari::app

#endif // MARI_APP_STROKE_ENTRY_HPP
