// Mari Paint — 기록 규약의 인터페이스 (docs/06-recording-contract.md)
//
// 🔴 이 헤더가 존재하는 단 하나의 이유:
//    **`SiganPublisher::publish()` 를 부르는 곳을 리포 전체에서 하나로 못박기 위해서다.**
//    사람 획(WM_POINTER)이든 AI 획(agent-api·MCP·CLI)이든, 붓질이든 영역 연산이든
//    전부 이 인터페이스의 **유일한 구현체**를 지나간다.
//    두 번째 발행 경로가 생기면 기록이 갈라진다(docs/05 1절 원칙).
//
// 🔴 위조 방지: 여기에는 출처를 **고르는** 길이 없다.
//    모든 진입점이 `const StrokeSource&` 를 **받기만** 한다. 만들지 않는다.
//    `StrokeSource` 는 기본 생성자가 없고 공개 생성자도 없으므로(core/origin.hpp),
//    호출자는 게이트나 팩토리가 준 것을 그대로 넘기는 수밖에 없다.
//
// 🔴 경계선(docs/03 2절): 여기에 서명도, 해시체인 봉인도, 등급 판정도 없다.
//    세는 것은 **개수와 타일 수뿐**이다.
//
// 🔴 의존 방향: 이 헤더는 **core 밖을 모른다.** sigan 도 app 도 모른다.
//    그래서 app/ 도 agent/ 도 이 헤더만 보고 기록을 흘려보낼 수 있고,
//    실제 `SiganPublisher` 를 아는 곳은 record/ 의 구현체 하나뿐이다.
//    (docs/06 7절 표는 구현체를 agent/src/recording.cpp 에 두라고 적었지만,
//     그러면 agent 가 sigan 을 직접 알게 되어 의존 방향이 꼬인다 —
//     결정 ③ 이 요구한 것은 **구현체가 하나**라는 것이지 그 파일의 위치가 아니므로
//     구현체를 record/ 로 옮겼다. docs/06 7절에 이 정정을 적어 두었다.)
#ifndef MARI_AGENT_RECORDING_HPP
#define MARI_AGENT_RECORDING_HPP

#include <mari/core/origin.hpp>
#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace mari::agent {

/// 비-스트로크 연산의 종류(docs/06 결정 ①).
/// **와이어에 나가지 않는다** — 프레임에 나가는 것은 `Synthetic` 플래그와 영역뿐이고,
/// 이 이름은 사람이 읽을 부연(저널 Note)에만 쓴다. 값은 뒤에만 더해라.
enum class RegionOpKind : u8 {
    Fill = 1,      ///< `fill`
    Erase = 2,     ///< `erase`
    Gradient = 3,  ///< `gradient`
    /// `transform` — 픽셀을 옮겨 쓴다. 붓질이 아니고 영역을 직접 쓰므로 여기 속한다
    /// (docs/06 결정 ① "앞으로 생길 모든 영역 직접쓰기"). 값은 **꼬리에만** 더했다.
    Transform = 4,
    /// `adjust` — 색 보정. 값은 꼬리에만 더한다.
    Adjust = 5,
};

/// 안정 문자열. 등급이 아니라 연산 이름이다.
[[nodiscard]] inline const char* regionOpKindName(RegionOpKind k) noexcept {
    switch (k) {
    case RegionOpKind::Fill:
        return "fill";
    case RegionOpKind::Erase:
        return "erase";
    case RegionOpKind::Gradient:
        return "gradient";
    case RegionOpKind::Transform:
        return "transform";
    case RegionOpKind::Adjust:
        return "adjust";
    }
    return "unknown";
}

/// 프레임 플래그 비트.
///
/// 🔴 값은 `sigan::FrameFlag` 와 **같아야 한다.** 여기 따로 둔 이유는 하나 —
///    이 헤더가 sigan 을 모르게 하기 위해서다(의존 방향). 두 벌이 어긋나면
///    기록이 조용히 거짓말을 하므로, record/ 의 구현체가 `static_assert` 로
///    둘이 같은 값인지 컴파일 타임에 못박는다. 갈라지면 빌드가 깨진다.
enum class RecordFlag : u32 {
    Down = 1u << 0,
    Move = 1u << 1,
    Up = 1u << 2,
    Eraser = 1u << 3,
    Synthetic = 1u << 4, ///< 붓질이 아니다(docs/06 결정 ①)
};

constexpr u32 recordFlags(RecordFlag f) noexcept { return static_cast<u32>(f); }
constexpr u32 operator|(RecordFlag a, RecordFlag b) noexcept {
    return static_cast<u32>(a) | static_cast<u32>(b);
}
constexpr u32 operator|(u32 a, RecordFlag b) noexcept { return a | static_cast<u32>(b); }
constexpr bool hasRecordFlag(u32 flags, RecordFlag f) noexcept {
    return (flags & static_cast<u32>(f)) != 0u;
}

/// 붓질 한 점. 파이프라인이 레코더에 넘기는 것.
/// `seq` 와 `t` 는 **발행기가** 채운다 — 여기에 자리가 없는 이유가 그것이다.
struct StrokePointRecord {
    PointF pos{};          ///< 캔버스 좌표(줌·회전·팬 불변)
    f32 pressure = 0.0f;
    f32 tiltX = 0.0f;
    f32 tiltY = 0.0f;
    f32 rotation = 0.0f;
    f32 velocity = 0.0f;
    LayerId layerId = kInvalidLayerId;
    BrushId brushId = kInvalidBrushId;
    u32 flags = 0; ///< `RecordFlag` 비트합(Down/Move/Up/Eraser)
};

/// 영역을 직접 쓰는 연산 한 건(docs/06 결정 ①).
/// 레코더는 이걸 **합성 프레임 쌍**(Down|Synthetic, Up|Synthetic)으로 내보낸다 —
/// 좌상단·우하단 두 점이 영향 영역을 프레임 바이트 안에 담는다.
struct RegionOpRecord {
    RegionOpKind kind = RegionOpKind::Fill;
    Rect area{};                         ///< 영향 영역(캔버스 좌표)
    LayerId layerId = kInvalidLayerId;
    u32 changedTiles = 0;                ///< 실제로 바뀐 타일 수(docs/06 결정 ② 축 C)
    bool eraser = false;                 ///< 지우개 성격이면 Eraser 플래그도 선다
};

/// 출처별 집계. **세 축을 따로 센다**(docs/06 결정 ②).
///
/// 🔴 세 축을 하나로 합치지 마라. 합치는 순간 가중치를 고르는 것이고,
///    가중치를 고르는 것은 판정이다 — Mari 의 몫이 아니다(docs/03 2절).
/// 🔴 등급 문자열을 여기에 만들지 마라. 숫자까지가 전부다.
struct RecordingTally {
    StrokeOriginStats strokes{};   ///< 축 A: 붓질 수(Synthetic 아닌 획)
    StrokeOriginStats regionOps{}; ///< 축 B: 영역 연산 수(Synthetic 획)
    /// 축 C: 출처별 **변경된 타일 수**. 픽셀이 아니라 타일인 이유는 docs/06 결정 ② 참조
    /// (타일맵이 이미 정확히 추적하고, 정수라 가장자리로 장난칠 수 없다).
    u64 changedTiles[kStrokeOriginSlots]{};

    /// 출처 o 의 변경 타일 수를 n 만큼 더한다. 모르는 값은 "미지정" 칸으로 간다.
    constexpr void addTiles(StrokeOrigin o, u64 n) noexcept {
        const usize v = static_cast<usize>(static_cast<u8>(o));
        changedTiles[v < kStrokeOriginSlots ? v : 0] += n;
    }
    [[nodiscard]] constexpr u64 tiles(StrokeOrigin o) const noexcept {
        const usize v = static_cast<usize>(static_cast<u8>(o));
        return changedTiles[v < kStrokeOriginSlots ? v : 0];
    }
    [[nodiscard]] constexpr u64 humanTiles() const noexcept {
        return tiles(StrokeOrigin::HumanPen) + tiles(StrokeOrigin::HumanMouse);
    }
    [[nodiscard]] constexpr u64 agentTiles() const noexcept {
        return tiles(StrokeOrigin::Agent);
    }
};

/// 발행 지점. **구현체는 리포 전체에 하나뿐이어야 한다.**
///
/// 스레드 규약: 한 레코더는 한 문서(그리기 스레드 하나)가 독점한다 —
/// `SiganPublisher` 와 같은 규약이다.
///
/// 수명 규약: 레코더는 **문서에 붙는다. 세션에 붙지 않는다.**
/// 세션은 오고 가지만 문서 작업 구간은 이어져야 하기 때문이다(docs/06 결정 ⑤).
class IStrokeRecorder {
public:
    virtual ~IStrokeRecorder() = default;

    /// 붓질 한 점. **핫 패스** — noexcept, 할당 금지, 블로킹 금지.
    /// 실패는 던지지 않고 `recordingBroken()` 으로 드러난다.
    virtual void onStrokePoint(const StrokeSource& src, const StrokePointRecord& p) noexcept = 0;

    /// 붓질 하나가 끝났다(up). 저널을 flush 하고 축 A·C 를 올린다.
    virtual void onStrokeEnd(const StrokeSource& src, u32 changedTiles) noexcept = 0;

    /// 영역 연산 하나. 합성 프레임 쌍으로 나가고 축 B·C 를 올린다.
    ///
    /// 🔴 여기만 `Result` 를 돌려주는 이유(docs/06 결정 ④):
    ///    **저널이 고장 났으면 호출자가 그 연산을 롤백해야 하므로 알아야 한다.**
    ///    픽셀은 바뀌었는데 정본에 흔적이 없는 상태를 남기면 인증서가 거짓이 된다.
    ///    파이프(싱크) 실패는 실패가 아니다 — 저널에 스풀되고 Ok 를 돌려준다.
    [[nodiscard]] virtual Result<void> onRegionOp(const StrokeSource& src,
                                                  const RegionOpRecord& op) = 0;

    /// 저널이 고장 나 **기록이 남지 않는 상태**인가(docs/06 결정 ④).
    /// true 가 되면 그리기 연산은 전부 거절된다. 자동 복구하지 않는다 —
    /// 조용히 다시 그려지기 시작하면 그 사이가 증거의 구멍이 된다.
    [[nodiscard]] virtual bool recordingBroken() const noexcept = 0;

    /// 지금까지의 집계. **원자료다.** 비율 해석은 이 계층 밖의 일이다.
    [[nodiscard]] virtual const RecordingTally& tally() const noexcept = 0;

    /// 지금까지의 기록을 무서명 과정 로그(JSON 문자열)로 뽑는다.
    /// 저장하는 .ora 의 `mari/prooflog.json` 에 그대로 실린다(docs/03 6절).
    ///
    /// 🔴 서명하지 않는다. 해시체인도 만들지 않는다. 등급은 "unsigned" 하나뿐이다
    ///    (docs/03 2절 경계선). 문자열로 돌려주는 덕분에 app 도 agent 도
    ///    prooflog 를 **만드는** 코드를 갖지 않는다 — 만드는 곳은 record/ 하나다.
    [[nodiscard]] virtual Result<std::string> buildProofLog() const = 0;
};

/// 문서 하나의 기록 구간을 여는 공장. **주입은 위에서 한다**(app 이나 CLI 가 꽂는다).
///
/// 🔴 이게 있어야 agent-api 가 Sigan 을 모른 채로 돌 수 있다(docs/03 5.1 —
///    Sigan 미설치는 정상 상태다). 공장이 안 꽂혀 있으면 레코더는 nullptr 이고,
///    그때도 그리기는 전부 된다. 다만 기록이 남지 않는다는 사실을 숨기지 않는다.
///
/// 🔴 구간을 만드는 것은 **문서**다(docs/06 결정 ⑤). agent-api 세션이 열리고 닫히는 것은
///    구간 경계가 아니다 — 그래서 이 공장은 문서가 생길 때만 불린다.
class IRecorderFactory {
public:
    virtual ~IRecorderFactory() = default;

    /// 문서 작업 구간 하나를 연다. `hint` 는 저널 파일 이름에 쓸 힌트(경로일 수도 있다).
    /// nullptr 를 담아 Ok 를 돌려주면 "기록하지 않는다"는 뜻이고, 실패는 실패다 —
    /// 저널을 열 수 없는데 문서를 여는 것은 기록 없이 그리는 모드가 된다(docs/06 결정 ④).
    [[nodiscard]] virtual Result<std::unique_ptr<IStrokeRecorder>>
    openForDocument(std::string_view hint) = 0;
};

} // namespace mari::agent

#endif // MARI_AGENT_RECORDING_HPP
