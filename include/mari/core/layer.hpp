// Mari Paint — 레이어와 레이어 트리 (선언만. 구현은 core 모듈이 소유한다)
//
// docs/02 2절: core/layer — 레이어 트리, 블렌드 모드, 마스크.
// 좌표는 전부 **캔버스 좌표**(좌상단 원점, y 아래로 증가).
#ifndef MARI_CORE_LAYER_HPP
#define MARI_CORE_LAYER_HPP

#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mari {

/// 레이어 종류. 값은 .ora 직렬화에 쓰이므로 뒤에만 더한다.
enum class LayerKind : u8 {
    Raster = 0, ///< 픽셀 레이어
    Group,      ///< 자식을 갖는 그룹(.ora 의 <stack>)
};

/// 모든 레이어의 공통 속성. 트리 노드 하나다.
class Layer {
public:
    virtual ~Layer() = default;

    [[nodiscard]] virtual LayerId id() const = 0;
    [[nodiscard]] virtual LayerKind kind() const = 0;

    [[nodiscard]] virtual const std::string& name() const = 0;
    virtual void setName(std::string name) = 0;

    /// 0.0(투명) ~ 1.0(불투명).
    [[nodiscard]] virtual f32 opacity() const = 0;
    virtual void setOpacity(f32 v) = 0;

    [[nodiscard]] virtual BlendMode blendMode() const = 0;
    virtual void setBlendMode(BlendMode m) = 0;

    /// 화면/합성에 나타나는지.
    [[nodiscard]] virtual bool visible() const = 0;
    virtual void setVisible(bool v) = 0;

    /// 편집 잠금. 잠긴 레이어에는 스탬프가 들어가지 않는다.
    [[nodiscard]] virtual bool locked() const = 0;
    virtual void setLocked(bool v) = 0;

    /// 알파 잠금 — 기존에 불투명한 픽셀만 고친다.
    [[nodiscard]] virtual bool alphaLocked() const = 0;
    virtual void setAlphaLocked(bool v) = 0;

    /// 클리핑 — 바로 아래 형제(클립되지 않은 첫 레이어)의 알파 안에서만 보인다(CSP 의 "아래 레이어에서
    /// 클리핑"). 연속된 클립 레이어들은 한 기준 레이어를 공유하고, 기준 레이어의 불투명도·블렌드가 묶음
    /// 전체에 적용된다. 합성기(core/src/compositor.cpp)가 이 규약을 구현한다.
    [[nodiscard]] virtual bool clipToBelow() const = 0;
    virtual void setClipToBelow(bool v) = 0;

    /// 픽셀 저장소. 그룹 레이어는 nullptr 다.
    [[nodiscard]] virtual TileMap* tiles() = 0;
    [[nodiscard]] virtual const TileMap* tiles() const = 0;

    /// 레이어 마스크(Gray8 타일맵). 없으면 nullptr.
    [[nodiscard]] virtual const TileMap* mask() const = 0;
    virtual void setMask(TileMapPtr mask) = 0;

    /// 내용이 실제로 차지하는 캔버스 영역. 그룹은 자식의 합집합.
    [[nodiscard]] virtual Rect bounds() const = 0;

    /// 그룹의 자식들. 아래(뒤)부터 위(앞) 순서다 — 인덱스 0이 가장 아래.
    /// 래스터 레이어는 빈 목록을 돌려준다.
    [[nodiscard]] virtual const std::vector<std::shared_ptr<Layer>>& children() const = 0;
};

using LayerPtr = std::shared_ptr<Layer>;

/// 문서 한 벌의 레이어 트리. 루트는 암묵적 그룹이다.
/// 순서 규약: **인덱스 0 = 가장 아래**. 합성은 0부터 위로 올라간다.
class LayerTree {
public:
    virtual ~LayerTree() = default;

    /// 캔버스 크기(px). 레이어는 이보다 클 수 있다(타일이 밖으로 나갈 수 있다).
    [[nodiscard]] virtual Size canvasSize() const = 0;
    virtual void setCanvasSize(Size s) = 0;

    /// 루트의 자식들(가장 아래부터).
    [[nodiscard]] virtual const std::vector<LayerPtr>& roots() const = 0;

    /// id 로 찾는다. 없으면 nullptr.
    [[nodiscard]] virtual LayerPtr find(LayerId id) const = 0;

    /// 새 래스터 레이어를 parent 아래 index 위치에 만든다.
    /// parent 가 kInvalidLayerId 면 루트에 넣는다. index 가 음수면 맨 위에 넣는다.
    [[nodiscard]] virtual Result<LayerPtr> addRaster(std::string name,
                                                     LayerId parent = kInvalidLayerId,
                                                     int index = -1) = 0;
    /// 새 그룹 레이어.
    [[nodiscard]] virtual Result<LayerPtr> addGroup(std::string name,
                                                    LayerId parent = kInvalidLayerId,
                                                    int index = -1) = 0;
    /// 레이어와 그 자식을 지운다.
    [[nodiscard]] virtual Result<void> remove(LayerId id) = 0;
    /// 레이어를 다른 부모/위치로 옮긴다.
    [[nodiscard]] virtual Result<void> move(LayerId id, LayerId newParent, int index) = 0;

    /// 현재 편집 대상.
    [[nodiscard]] virtual LayerId activeLayer() const = 0;
    [[nodiscard]] virtual Result<void> setActiveLayer(LayerId id) = 0;

    /// 주어진 캔버스 영역을 RGBA8 로 평탄화한다. dst 는 width*height*4 바이트,
    /// 행 stride 는 dstStride 바이트다. 더티 영역만 넘기는 게 정상 사용이다.
    [[nodiscard]] virtual Result<void> flatten(const Rect& area, u8* dst, usize dstStride) const = 0;
};

using LayerTreePtr = std::shared_ptr<LayerTree>;

/// 빈 레이어 트리 생성. core 모듈이 구현한다.
[[nodiscard]] Result<LayerTreePtr> makeLayerTree(Size canvasSize);

} // namespace mari

#endif // MARI_CORE_LAYER_HPP
