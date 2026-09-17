// Mari Paint — 🔴 O(1) 스냅샷과 분기 (docs/05 2.2)
//
// **COW 타일 구조에서 공짜로 나온다.** 이건 다른 툴이 흉내내기 어렵다 —
// 포토샵 스크립팅으로 하려면 파일을 복사해야 한다.
//
//     snap = doc.snapshot("러프 완성")   // O(1). 픽셀을 한 바이트도 복사하지 않는다
//     for 시도 in [A, B, C]:
//         doc.restore(snap)             // O(1)
//         doc.stroke(시도)
//         후보.append(doc.render())     // 보고
//     doc.restore(snap)
//
// AI 에이전트의 작업 방식 자체가 **시도 → 평가 → 되돌리기**다.
// 그걸 저렴하게 만들어 주는 게 "AI 가 쉽게 쓴다"의 실체다.
//
// 무엇을 담나:
//   · 레이어별 `TileMap::snapshot()` — 타일 포인터만 공유한다(픽셀 복사 0)
//   · 트리 구조(부모·순서)와 레이어 속성, 캔버스 크기, 활성 레이어, **선택 마스크**
//   · 지워진 레이어의 `LayerPtr` 를 **붙잡아 둔다** → 되돌릴 때 id 가 살아 돌아온다
//     (core 의 `reattachLayer()`. 새로 만들면 id 가 바뀌어 에이전트 주소가 무효가 된다)
//
// 비용: 스냅샷 하나 = 레이어 수 × (타일맵 인덱스 복사). 4096² 캔버스에서도
// 수백 KB 수준이고, `tests/agent/test_snapshot.cpp` 가 **실제로 재서 출력한다.**
#ifndef MARI_AGENT_SNAPSHOT_HPP
#define MARI_AGENT_SNAPSHOT_HPP

#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/selection.hpp>
#include <mari/core/types.hpp>

#include <string>
#include <vector>

namespace mari::agent {

/// 스냅샷 식별자. 1부터. 0 은 "없음".
using SnapshotId = u64;
inline constexpr SnapshotId kInvalidSnapshotId = 0;

/// 스냅샷 안의 레이어 한 줄.
struct SnapshotNode {
    LayerId id = kInvalidLayerId;
    LayerId parent = kInvalidLayerId;
    int index = 0; ///< 형제 중 위치(0 = 가장 아래)
    LayerKind kind = LayerKind::Raster;
    std::string name;
    f32 opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool visible = true;
    bool locked = false;
    bool alphaLocked = false;
    /// COW 스냅샷. 그룹 레이어는 nullptr.
    TileMapPtr tiles;
    /// 🔴 레이어 객체를 붙잡아 둔다 — 트리에서 지워져도 id 째로 되살릴 수 있다.
    LayerPtr layer;
    /// 스냅샷 시점의 할당 타일 수(메모리 보고용).
    usize tileCount = 0;
};

/// 문서 한 벌의 상태 사본. **픽셀을 복사하지 않는다.**
struct DocSnapshot {
    SnapshotId id = kInvalidSnapshotId;
    /// 사람이 붙인 이름. `snapshot("러프 완성")`.
    std::string label;
    /// 어느 스냅샷에서 갈라져 나왔나(branch). 없으면 0.
    SnapshotId parentSnapshot = kInvalidSnapshotId;
    Size canvasSize{};
    LayerId activeLayer = kInvalidLayerId;
    /// 🔴 선택도 되돌린다. 마스크 복사는 타일 핸들만 복제하므로 여전히 픽셀 복사 0이다.
    SelectionMask selection;
    std::vector<SnapshotNode> nodes;
    /// 만든 시각(단조 시계 기준 ms). 벽시계가 아니다 — 순서만 보장한다.
    f64 tMs = 0.0;

    /// 담긴 타일 포인터의 총 개수. 메모리 보고와 diff 에 쓴다.
    [[nodiscard]] usize totalTiles() const;
    /// 스냅샷 자체가 붙잡고 있는 대략적인 바이트(**공유 픽셀은 빼고** 인덱스만).
    [[nodiscard]] usize approxOverheadBytes() const;
};

/// 트리를 그대로 담는다. O(레이어 수). 픽셀 복사 없음.
[[nodiscard]] Result<DocSnapshot> takeSnapshot(const LayerTree& tree, std::string label,
                                               const SelectionMask& selection);

/// 스냅샷을 트리에 되돌린다. O(레이어 수). 픽셀 복사 없음.
///
/// · 스냅샷 이후 만들어진 레이어는 지운다.
/// · 스냅샷 이후 지워진 레이어는 **같은 id 로** 되살린다.
/// · 픽셀·속성·순서·활성 레이어를 스냅샷 시점으로 되돌린다.
[[nodiscard]] Result<void> restoreSnapshot(LayerTree& tree, const DocSnapshot& snap,
                                           SelectionMask* outSelection = nullptr);

/// 두 상태의 차이. 픽셀을 비교하지 않고 **타일 포인터**를 비교한다 — COW 라서
/// 안 바뀐 타일은 포인터가 같다. 그래서 diff 도 O(타일 수)지 O(픽셀 수)가 아니다.
struct SnapshotDiff {
    /// 내용이 달라진 타일 수.
    usize changedTiles = 0;
    /// 달라진 타일들의 캔버스 경계 상자.
    Rect area{};
    /// 한쪽에만 있는 레이어.
    std::vector<LayerId> addedLayers;
    std::vector<LayerId> removedLayers;
    /// 픽셀이 달라진 레이어.
    std::vector<LayerId> changedLayers;
    /// 속성(이름·불투명도·블렌드·가시성·순서)만 달라진 레이어.
    std::vector<LayerId> propChangedLayers;

    [[nodiscard]] bool identical() const {
        return changedTiles == 0 && addedLayers.empty() && removedLayers.empty() &&
               changedLayers.empty() && propChangedLayers.empty();
    }
};

/// a → b 의 차이. 순서가 중요하다(added 는 b 에만 있는 것).
[[nodiscard]] SnapshotDiff diffSnapshots(const DocSnapshot& a, const DocSnapshot& b);

/// 스냅샷 보관함. 한도를 넘으면 **가장 오래된 것부터** 버린다.
/// 이름표가 붙은 스냅샷도 예외가 아니다 — 조용히 무한정 쌓이면 그게 누수다.
class SnapshotStore {
public:
    explicit SnapshotStore(usize limit = 256) : limit_(limit == 0 ? 1 : limit) {}

    /// 새 스냅샷을 넣고 id 를 매긴다.
    SnapshotId put(DocSnapshot snap);
    /// 없으면 nullptr.
    [[nodiscard]] const DocSnapshot* find(SnapshotId id) const;
    /// 이름으로 찾는다(가장 최근 것). 없으면 nullptr.
    [[nodiscard]] const DocSnapshot* findByLabel(std::string_view label) const;
    [[nodiscard]] usize size() const noexcept { return items_.size(); }
    [[nodiscard]] usize limit() const noexcept { return limit_; }
    [[nodiscard]] const std::vector<DocSnapshot>& items() const noexcept { return items_; }
    /// 버린 스냅샷 수(한도 초과). 숨기지 않고 센다.
    [[nodiscard]] u64 evicted() const noexcept { return evicted_; }
    void clear();

private:
    std::vector<DocSnapshot> items_;
    SnapshotId next_ = 1;
    usize limit_;
    u64 evicted_ = 0;
};

} // namespace mari::agent

#endif // MARI_AGENT_SNAPSHOT_HPP
