// Mari Paint — O(1) 스냅샷 구현. 선언은 include/mari/agent/snapshot.hpp.
//
// 🔴 이 파일에는 **픽셀을 복사하는 줄이 없다.** 있으면 그게 버그다.
//    TileMap::snapshot() 의 COW 가 전부를 한다.
#include <mari/agent/snapshot.hpp>

#include <mari/core/layer_ops.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace mari::agent {
namespace {

/// 타일 포인터 하나가 잡아먹는 대략적인 바이트(해시 노드 + shared_ptr).
/// **픽셀은 공유되므로 여기 안 들어간다.** 정확한 값이 아니라 자릿수를 위한 값이다.
/// (핸들 하나 = 타일 객체 + 제어 블록 + 해시 노드. 픽셀 버퍼는 공유된다.)
constexpr usize kApproxBytesPerTileRef = 160;

f64 nowMs() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::milli>(t).count();
}

void collectNodes(const std::vector<LayerPtr>& list, LayerId parent,
                  std::vector<SnapshotNode>& out) {
    for (usize i = 0; i < list.size(); ++i) {
        const LayerPtr& l = list[i];
        SnapshotNode n;
        n.id = l->id();
        n.parent = parent;
        n.index = static_cast<int>(i);
        n.kind = l->kind();
        n.name = l->name();
        n.opacity = l->opacity();
        n.blend = l->blendMode();
        n.visible = l->visible();
        n.locked = l->locked();
        n.alphaLocked = l->alphaLocked();
        n.layer = l;
        if (const TileMap* tiles = l->tiles(); tiles != nullptr) {
            // 🔴 여기가 O(1) 의 전부다. 타일 포인터만 공유한다.
            n.tiles = tiles->snapshot();
            n.tileCount = tiles->tileCount();
        }
        out.push_back(std::move(n));
        collectNodes(l->children(), l->id(), out); // 부모 먼저(복원 순서가 여기에 달렸다)
    }
}

void collectIds(const std::vector<LayerPtr>& list, std::vector<LayerId>& out) {
    for (const LayerPtr& l : list) {
        out.push_back(l->id());
        collectIds(l->children(), out);
    }
}

/// 타일맵이 실제로 들고 있는 타일 좌표를 모은다.
void allTiles(const TileMap& map, DirtyTiles& out) {
    const Rect b = map.bounds();
    if (b.isEmpty()) {
        return;
    }
    map.collectTiles(b, out);
}

bool sameProps(const SnapshotNode& a, const SnapshotNode& b) {
    return a.parent == b.parent && a.index == b.index && a.name == b.name &&
           a.opacity == b.opacity && a.blend == b.blend && a.visible == b.visible &&
           a.locked == b.locked && a.alphaLocked == b.alphaLocked;
}

} // namespace

usize DocSnapshot::totalTiles() const {
    usize n = 0;
    for (const auto& node : nodes) {
        n += node.tileCount;
    }
    return n;
}

usize DocSnapshot::approxOverheadBytes() const {
    usize n = sizeof(DocSnapshot) + nodes.size() * sizeof(SnapshotNode);
    for (const auto& node : nodes) {
        n += node.name.capacity();
        n += node.tileCount * kApproxBytesPerTileRef;
    }
    return n;
}

Result<DocSnapshot> takeSnapshot(const LayerTree& tree, std::string label,
                                 const SelectionMask& selection) {
    DocSnapshot snap;
    snap.label = std::move(label);
    snap.canvasSize = tree.canvasSize();
    snap.activeLayer = tree.activeLayer();
    snap.selection = selection;
    snap.tMs = nowMs();
    collectNodes(tree.roots(), kInvalidLayerId, snap.nodes);
    return Ok(std::move(snap));
}

Result<void> restoreSnapshot(LayerTree& tree, const DocSnapshot& snap,
                             SelectionMask* outSelection) {
    std::unordered_set<LayerId> wanted;
    wanted.reserve(snap.nodes.size() * 2);
    for (const auto& n : snap.nodes) {
        wanted.insert(n.id);
    }

    // 1. 스냅샷 이후에 생긴 레이어를 지운다. 부모를 지우면 자식도 함께 사라지므로
    //    이미 사라진 id 는 건너뛴다.
    std::vector<LayerId> present;
    collectIds(tree.roots(), present);
    for (const LayerId id : present) {
        if (wanted.count(id) != 0) {
            continue;
        }
        if (!tree.find(id)) {
            continue;
        }
        const Result<void> r = tree.remove(id);
        if (!r.ok()) {
            return r;
        }
    }

    // 2. 스냅샷 순서(부모 먼저)대로 제자리에 놓는다.
    for (const auto& n : snap.nodes) {
        LayerPtr live = tree.find(n.id);
        if (!live) {
            // 🔴 지워졌던 레이어를 **같은 id 로** 되살린다. 새로 만들면 주소가 바뀐다.
            const Result<void> att = reattachLayer(tree, n.layer, n.parent, n.index);
            if (!att.ok()) {
                return att;
            }
            live = tree.find(n.id);
            if (!live) {
                return Err("레이어를 되살리지 못했다: " + std::to_string(n.id),
                           ErrorCode::Unknown);
            }
        } else {
            const Result<void> mv = tree.move(n.id, n.parent, n.index);
            if (!mv.ok()) {
                return mv;
            }
        }

        live->setName(n.name);
        live->setOpacity(n.opacity);
        live->setBlendMode(n.blend);
        live->setVisible(n.visible);
        live->setLocked(n.locked);
        live->setAlphaLocked(n.alphaLocked);

        if (n.tiles && live->tiles() != nullptr) {
            // 스냅샷이 들고 있는 맵을 **그대로 꽂지 않는다.** 또 한 겹 snapshot() 을 떠서
            // 준다 — 안 그러면 복원 뒤의 붓질이 스냅샷 자체를 고쳐 버린다.
            const Result<void> st = setLayerTiles(*live, n.tiles->snapshot());
            if (!st.ok()) {
                return st;
            }
        }
    }

    tree.setCanvasSize(snap.canvasSize);
    if (snap.activeLayer != kInvalidLayerId && tree.find(snap.activeLayer)) {
        const Result<void> a = tree.setActiveLayer(snap.activeLayer);
        if (!a.ok()) {
            return a;
        }
    }
    if (outSelection != nullptr) {
        *outSelection = snap.selection;
    }
    return Ok();
}

SnapshotDiff diffSnapshots(const DocSnapshot& a, const DocSnapshot& b) {
    SnapshotDiff d;
    std::unordered_map<LayerId, const SnapshotNode*> an;
    std::unordered_map<LayerId, const SnapshotNode*> bn;
    for (const auto& n : a.nodes) {
        an.emplace(n.id, &n);
    }
    for (const auto& n : b.nodes) {
        bn.emplace(n.id, &n);
    }

    for (const auto& n : b.nodes) {
        if (an.find(n.id) == an.end()) {
            d.addedLayers.push_back(n.id);
        }
    }
    for (const auto& n : a.nodes) {
        if (bn.find(n.id) == bn.end()) {
            d.removedLayers.push_back(n.id);
        }
    }

    DirtyTiles ta;
    DirtyTiles tb;
    for (const auto& na : a.nodes) {
        const auto it = bn.find(na.id);
        if (it == bn.end()) {
            continue;
        }
        const SnapshotNode& nb = *it->second;
        if (!sameProps(na, nb)) {
            d.propChangedLayers.push_back(na.id);
        }
        if (!na.tiles || !nb.tiles) {
            continue;
        }
        // 🔴 픽셀이 아니라 **타일 포인터**를 비교한다. COW 라서 안 바뀐 타일은 같은 포인터다.
        ta.clear();
        tb.clear();
        allTiles(*na.tiles, ta);
        allTiles(*nb.tiles, tb);
        std::sort(ta.begin(), ta.end(), [](const TileCoord& x, const TileCoord& y) {
            return x.ty != y.ty ? x.ty < y.ty : x.tx < y.tx;
        });
        tb.insert(tb.end(), ta.begin(), ta.end());
        std::sort(tb.begin(), tb.end(), [](const TileCoord& x, const TileCoord& y) {
            return x.ty != y.ty ? x.ty < y.ty : x.tx < y.tx;
        });
        tb.erase(std::unique(tb.begin(), tb.end()), tb.end());

        bool layerChanged = false;
        for (const TileCoord& c : tb) {
            const ConstTilePtr pa = na.tiles->at(c);
            const ConstTilePtr pb = nb.tiles->at(c);
            // 🔴 타일 **객체**가 아니라 픽셀 **버퍼 주소**를 견준다.
            //    snapshot() 은 타일 핸들을 새로 만들되 버퍼는 공유한다(core/tile.cpp).
            //    객체 포인터로 비교하면 안 바뀐 타일까지 전부 "바뀜"이 된다.
            //    양쪽 타일을 우리가 붙잡고 있으므로 주소가 같으면 같은 버퍼가 맞다.
            if (pa && pb && pa->pixels() == pb->pixels()) {
                continue;
            }
            if (!pa && !pb) {
                continue;
            }
            // 둘 다 비어 있으면(= 없음/빈 타일) 보이는 결과가 같다. 차이로 세지 않는다.
            const bool aBlank = !pa || pa->isBlank();
            const bool bBlank = !pb || pb->isBlank();
            if (aBlank && bBlank) {
                continue;
            }
            ++d.changedTiles;
            layerChanged = true;
            d.area = d.area.united(c.canvasRect());
        }
        if (layerChanged) {
            d.changedLayers.push_back(na.id);
        }
    }
    return d;
}

SnapshotId SnapshotStore::put(DocSnapshot snap) {
    snap.id = next_++;
    items_.push_back(std::move(snap));
    while (items_.size() > limit_) {
        items_.erase(items_.begin());
        ++evicted_;
    }
    return items_.back().id;
}

const DocSnapshot* SnapshotStore::find(SnapshotId id) const {
    for (const auto& s : items_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const DocSnapshot* SnapshotStore::findByLabel(std::string_view label) const {
    for (usize i = items_.size(); i > 0; --i) {
        if (items_[i - 1].label == label) {
            return &items_[i - 1];
        }
    }
    return nullptr;
}

void SnapshotStore::clear() { items_.clear(); }

} // namespace mari::agent
