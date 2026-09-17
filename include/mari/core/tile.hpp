// Mari Paint — 타일 기반 캔버스 저장 (선언만. 구현은 core 모듈이 소유한다)
//
// docs/02 3절: 캔버스는 타일의 희소 맵이다.
//   · 타일 크기 64×64 (kTileSize)
//   · 빈 타일은 메모리를 안 쓴다 — 공유되는 null 타일 하나로 대신한다
//   · Copy-on-Write — 레이어 복제/실행취소 스냅샷이 O(1)
//
// 좌표 규약: 모든 좌표는 **캔버스 좌표**(좌상단 원점, y 아래로 증가)다.
// 타일 인덱스는 tileIndexFor() 로 얻는다. 음수 타일 인덱스도 유효하다
// (캔버스 밖으로 확장되는 레이어를 허용하기 위해).
#ifndef MARI_CORE_TILE_HPP
#define MARI_CORE_TILE_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <memory>
#include <vector>

namespace mari {

/// 타일 좌표 (타일 단위 인덱스. 픽셀 좌표가 아니다).
struct TileCoord {
    i32 tx = 0;
    i32 ty = 0;

    /// 이 타일이 덮는 캔버스 좌표 영역.
    [[nodiscard]] constexpr Rect canvasRect() const {
        return Rect{tileOrigin(tx), tileOrigin(ty), kTileSize, kTileSize};
    }
    friend constexpr bool operator==(const TileCoord&, const TileCoord&) = default;
};

/// TileCoord 를 해시 컨테이너 키로 쓰기 위한 해시.
struct TileCoordHash {
    [[nodiscard]] usize operator()(const TileCoord& c) const noexcept {
        const u64 k = (static_cast<u64>(static_cast<u32>(c.tx)) << 32) ^
                      static_cast<u64>(static_cast<u32>(c.ty));
        return static_cast<usize>(k * 0x9E3779B97F4A7C15ull);
    }
};

/// 64×64 픽셀 타일 한 장. **불변(immutable) 취급**한다.
/// 고치려면 TileMap::writable() 로 COW 사본을 받아 그 사본을 고친다.
/// 공유는 shared_ptr 의 참조 계수로 한다 — 참조 계수가 1이면 제자리 수정이 안전하다.
class Tile {
public:
    virtual ~Tile() = default;

    /// 픽셀 형식. 타일맵 전체가 같은 형식을 쓴다.
    [[nodiscard]] virtual PixelFormat format() const = 0;
    /// 읽기 전용 픽셀 시작 주소. 행 단위로 stride() 바이트씩 떨어져 있다.
    [[nodiscard]] virtual const u8* pixels() const = 0;
    /// 쓰기 가능한 픽셀 시작 주소. **단독 소유일 때만** 부른다.
    [[nodiscard]] virtual u8* mutablePixels() = 0;
    /// 한 행의 바이트 수.
    [[nodiscard]] virtual usize stride() const = 0;
    /// 전부 투명/0 이면 true. 합성에서 건너뛰는 최적화에 쓴다.
    [[nodiscard]] virtual bool isBlank() const = 0;
    /// 독립 사본 하나. COW 분리에 쓴다.
    [[nodiscard]] virtual std::shared_ptr<Tile> clone() const = 0;
};

using TilePtr = std::shared_ptr<Tile>;
using ConstTilePtr = std::shared_ptr<const Tile>;

/// 더티 타일 목록. 스트로크 엔진이 반환하고 합성기가 소비한다.
/// 핫 패스라 할당을 줄이려고 벡터를 재사용하는 것을 전제로 한다.
using DirtyTiles = std::vector<TileCoord>;

/// 희소 타일 맵 — 레이어 한 장의 픽셀 저장소.
/// 없는 타일은 nullptr 로 돌려준다(= 완전 투명). 이게 "빈 타일은 메모리를 안 쓴다"의 실체다.
class TileMap {
public:
    virtual ~TileMap() = default;

    [[nodiscard]] virtual PixelFormat format() const = 0;
    /// 할당된 타일 수.
    [[nodiscard]] virtual usize tileCount() const = 0;
    /// 할당된 타일 전체를 덮는 캔버스 좌표 경계 상자. 비어 있으면 빈 Rect.
    [[nodiscard]] virtual Rect bounds() const = 0;

    /// 읽기용 타일. 없으면 nullptr (완전 투명으로 취급한다).
    [[nodiscard]] virtual ConstTilePtr at(TileCoord c) const = 0;
    /// 쓰기용 타일. 없으면 새로 만들고, 공유 중이면 COW 로 분리한 뒤 돌려준다.
    /// 실패(메모리 부족 등) 시 오류를 담아 돌려준다 — **던지지 않는다**.
    [[nodiscard]] virtual Result<TilePtr> writable(TileCoord c) = 0;

    /// 타일을 비운다(메모리 해제). 없는 타일이면 아무 일도 안 한다.
    virtual void erase(TileCoord c) = 0;
    /// 전부 비운다.
    virtual void clear() = 0;

    /// 주어진 캔버스 영역과 겹치는, **할당된** 타일 좌표를 out 에 덧붙인다.
    virtual void collectTiles(const Rect& canvasArea, DirtyTiles& out) const = 0;

    /// O(1) 스냅샷. 타일을 복사하지 않고 공유한다(COW). 실행취소가 이걸 쓴다.
    [[nodiscard]] virtual std::shared_ptr<TileMap> snapshot() const = 0;
};

using TileMapPtr = std::shared_ptr<TileMap>;

/// 기본 타일맵 구현 생성. core 모듈이 구현한다.
[[nodiscard]] Result<TileMapPtr> makeTileMap(PixelFormat format);

} // namespace mari

#endif // MARI_CORE_TILE_HPP
