// Mari Paint — 선택 마스크 (docs/04 4절 3번 · docs/05 8.3 이 적어 둔 한계를 메운다)
//
// 여기까지의 한계는 이랬다: "선택은 사각형 하나뿐. 마스크 저장소가 없어
// `select.invert`·`select.feather` 는 미지원 표시 + MCP 도구 미노출."
// 이 헤더가 그 마스크 저장소다.
//
// ── 저장 구조 ────────────────────────────────────────────────────────────
// 캔버스와 **같은 구조**를 쓴다 — 64×64 Gray8 타일의 희소 맵(COW).
// 새 자료구조를 만들지 않는다. `TileMap` 이 이미 희소·COW·O(1) 스냅샷을 준다.
//
// 🔴 타일이 **없는** 자리의 값은 0 이 아니라 `outsideValue()` 다. 이 한 칸 덕분에
//    두 극단이 **둘 다 타일 0개**가 된다:
//
//      전체 선택 : tileCount()==0 · outsideValue()==255   → 메모리 0
//      빈 선택   : tileCount()==0 · outsideValue()==0     → 메모리 0
//
//    반전(invert)은 `outside` 를 뒤집고 있는 타일만 뒤집으면 끝이라 O(타일 수)다.
//    사각형 선택을 반전해도 타일이 몇 장 생길 뿐 캔버스 전체가 할당되지 않는다.
//
// 🔴 캔버스 **밖은 언제나 선택되지 않는다.** 그래서 `outside==255` 여도 선택은
//    무한하지 않고 캔버스에 갇힌다 — 반전·팽창이 잘 정의된다.
//
// ── 핫 패스 규약 ─────────────────────────────────────────────────────────
// 🔴 선택이 없으면(= 전체 선택) 그리기 비용이 **0 만큼** 늘어야 한다.
//    그래서 엔진은 `beginStroke()` 에서 `isAll()` 을 한 번 보고 포인터를 꺼 둔다.
//    스탬프마다 마스크를 묻지 않는다. `tests/stroke/test_bench.cpp` 가 수치를 찍는다.
#ifndef MARI_CORE_SELECTION_HPP
#define MARI_CORE_SELECTION_HPP

#include <mari/core/result.hpp>
#include <mari/core/tile.hpp>
#include <mari/core/types.hpp>

#include <functional>
#include <string_view>
#include <vector>

namespace mari {

/// 불린 결합 방식. 문자열 이름이 정본이다(agent-api 가 그대로 받는다).
enum class SelectionOp : u8 {
    Replace = 0, ///< 기존 선택을 버리고 새것으로
    Union,       ///< 합집합 (add)
    Subtract,    ///< 차집합 (subtract)
    Intersect,   ///< 교집합 (intersect)
    Xor,         ///< 대칭차 (xor)
};

[[nodiscard]] const char* selectionOpName(SelectionOp op) noexcept;
/// 문자열 → 결합 방식. `"add"`/`"union"` 처럼 별칭도 받는다. 모르면 실패.
[[nodiscard]] Result<SelectionOp> selectionOpFromName(std::string_view s) noexcept;

/// 선택 마스크 한 장. 8비트 알파 — 0 은 선택 안 됨, 255 는 완전 선택,
/// 그 사이는 **부분 선택**(페더·안티에일리어싱 가장자리)이다.
///
/// 값 의미론이다. 복사는 `TileMap::snapshot()` 을 타므로 **픽셀을 복사하지 않는다**
/// (타일 핸들만 복제하고, 쓸 때 갈라선다).
class SelectionMask {
public:
    /// 0×0 캔버스의 빈 선택. 아무 것도 가리키지 않는다.
    SelectionMask() = default;

    SelectionMask(const SelectionMask& o);
    SelectionMask& operator=(const SelectionMask& o);
    SelectionMask(SelectionMask&&) noexcept = default;
    SelectionMask& operator=(SelectionMask&&) noexcept = default;
    ~SelectionMask() = default;

    /// 캔버스 전체가 선택된 마스크. **타일 0개.**
    [[nodiscard]] static SelectionMask all(Size canvas) noexcept;
    /// 아무 것도 선택되지 않은 마스크. **타일 0개.**
    [[nodiscard]] static SelectionMask empty(Size canvas) noexcept;

    [[nodiscard]] Size canvasSize() const noexcept { return canvas_; }
    /// 캔버스 크기를 바꾼다(문서 크기 변경용). 새 범위 밖 타일은 버린다.
    void setCanvasSize(Size canvas);

    /// 타일이 없는 자리의 값. 0(선택 안 됨) 또는 255(선택됨) 뿐이다.
    [[nodiscard]] u8 outsideValue() const noexcept { return outside_; }
    /// 할당된 타일 수. **전체 선택·빈 선택은 0 이다.**
    [[nodiscard]] usize tileCount() const noexcept;
    /// 캔버스 전체가 완전히 선택됐나(= 그리기 제한이 없다).
    [[nodiscard]] bool isAll() const noexcept { return outside_ == 255 && tileCount() == 0; }
    /// 아무 것도 선택되지 않았나.
    [[nodiscard]] bool isEmpty() const noexcept { return outside_ == 0 && tileCount() == 0; }

    /// 한 픽셀의 선택 정도. 캔버스 밖은 언제나 0.
    [[nodiscard]] u8 valueAt(i32 x, i32 y) const noexcept;

    /// 🔴 핫 패스용. 그 타일의 Gray8 픽셀(64×64, stride 64) 또는 nullptr.
    ///    nullptr 이면 그 타일 전체가 `outsideValue()` 다.
    ///    포인터는 **다음 변경 전까지만** 유효하다.
    [[nodiscard]] const u8* tilePixels(TileCoord c) const noexcept;

    /// 선택된 픽셀을 덮는 경계 상자(캔버스 안으로 자른다). 없으면 빈 Rect.
    [[nodiscard]] Rect bounds() const;
    /// 선택 총량. 부분 선택은 그 값만큼만 센다(합 / 255). 정수라 부풀릴 수 없다.
    [[nodiscard]] u64 selectedPixels() const;

    /// 할당된 타일 좌표를 모은다.
    void collectTiles(DirtyTiles& out) const;

    // ── 변형 ─────────────────────────────────────────────────────────────

    /// 반전. O(타일 수) — 캔버스를 할당하지 않는다.
    void invert();

    /// 팽창(by > 0) / 수축(by < 0). 단위는 px, **원형**이다.
    /// 부분 선택은 128 을 기준으로 이분화한 뒤 처리한다(가장자리 정보는 사라진다).
    [[nodiscard]] Result<void> expand(i32 by);

    /// 가우시안 페더. `radius` px, σ = radius/2.
    /// 캔버스 경계 밖은 **가장자리 값을 연장**해서 샘플한다 — 그래서 전체 선택을
    /// 페더해도 테두리가 깎이지 않는다.
    [[nodiscard]] Result<void> feather(f32 radius);

    /// 다른 마스크와 결합한다. 캔버스 크기가 다르면 거절한다.
    [[nodiscard]] Result<void> combine(const SelectionMask& other, SelectionOp op);

    // ── 만들기 ───────────────────────────────────────────────────────────

    /// 사각형. 경계는 딱 떨어진다(안티에일리어싱 없음 — 정수 격자니까).
    [[nodiscard]] static Result<SelectionMask> fromRect(Size canvas, const Rect& r);
    /// 타원(주어진 사각형에 내접). `antialias` 면 4×4 초과표본으로 가장자리를 만든다.
    [[nodiscard]] static Result<SelectionMask> fromEllipse(Size canvas, const Rect& box,
                                                           bool antialias = true);
    /// 올가미 — 닫힌 폴리곤. 짝수-홀수(even-odd) 규칙. 점이 3개 미만이면 거절한다.
    [[nodiscard]] static Result<SelectionMask> fromPolygon(Size canvas,
                                                           const std::vector<PointF>& points,
                                                           bool antialias = true);
    /// 색상 범위. `tolerance` 는 채널당 최대 차이(0..255)다.
    /// 🔴 기준색이 "완전 투명"에 걸리면 **할당되지 않은 타일도 선택된다** —
    ///    그때는 `outside` 가 255 가 되고 타일은 여전히 0개다.
    [[nodiscard]] static Result<SelectionMask> fromColorRange(Size canvas, const TileMap& src,
                                                              Color8 ref, i32 tolerance);
    /// 내용 기반 — 알파가 `threshold` 이상인 픽셀.
    [[nodiscard]] static Result<SelectionMask> fromContent(Size canvas, const TileMap& src,
                                                           u8 threshold = 1);
    /// 레이어 알파 그대로. 반투명은 **반투명한 선택**이 된다.
    [[nodiscard]] static Result<SelectionMask> fromLayerAlpha(Size canvas, const TileMap& src);

private:
    /// 원본 타일맵의 **할당된 타일만** 훑어 마스크를 만든다(희소성을 그대로 물려받는다).
    /// `outsideValue` 는 "할당되지 않은 타일"(= 완전 투명)에 대한 판정 결과다.
    [[nodiscard]] static Result<SelectionMask> fromTiles(Size canvas, const TileMap& src,
                                                         u8 outsideValue,
                                                         const std::function<u8(const u8*)>& judge);

    [[nodiscard]] Result<u8*> writableTile(TileCoord c);
    /// `area`(캔버스 안) 를 한 값으로 채운다. dense 임시 버퍼를 만들지 않는다.
    [[nodiscard]] Result<void> fillRect(const Rect& area, u8 v);
    /// 값이 전부 `outside_` 인 타일을 버린다. 모든 변경 끝에 부른다 —
    /// "전체 선택 = 타일 0개" 가 이 함수 때문에 성립한다.
    void normalize();
    /// `area`(캔버스 안) 에 dense 버퍼를 그대로 써 넣는다.
    [[nodiscard]] Result<void> writeRegion(const Rect& area, const u8* src, usize srcStride);
    /// 캔버스 안으로 좌표를 물린 뒤의 값(페더가 쓰는 clamp 샘플).
    [[nodiscard]] u8 clampedValueAt(i32 x, i32 y) const noexcept;

    TileMapPtr tiles_; ///< Gray8 희소 맵. 없을 수도 있다(그때는 타일 0개).
    Size canvas_{};
    u8 outside_ = 0;
    /// `bounds()` 는 최악의 경우 캔버스를 훑는다. 그리기 경로가 매번 물으므로 캐시한다.
    mutable Rect boundsCache_{};
    mutable bool boundsValid_ = false;
};

} // namespace mari

#endif // MARI_CORE_SELECTION_HPP
