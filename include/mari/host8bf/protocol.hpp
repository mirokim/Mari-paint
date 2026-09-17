// Mari Paint — .8bf 격리 호스트 프로토콜 (docs/02 6.2)
//
// 본체(mari-paint.exe) ↔ 호스트(mari-8bf-host-{x86,x64}.exe) 사이의 계약이다.
//
// 🔴 설계 원칙 3: **남의 코드는 내 프로세스에 들이지 않는다.**
//    .8bf 는 남이 만든 네이티브 DLL 이다. 우리 주소 공간에서 죽으면 그림이 날아간다.
//    그래서 별도 프로세스에서 돌리고, 죽으면 호스트만 죽는다.
//
// 🔴 픽셀은 COM 마샬링을 타지 않는다. **파일 매핑(공유 메모리)** 으로 넘긴다.
//    4096×4096 RGBA 한 장이 64MB 다. 이걸 SAFEARRAY 로 마샬링하면 왕복 복사 4번이다.
//
// 이 헤더는 **Win32 를 포함하지 않는다.** 레이아웃 계산은 순수 산술이라
// Linux 에서도 컴파일·테스트된다(tests/win/test_shm_layout.cpp).
#ifndef MARI_HOST8BF_PROTOCOL_HPP
#define MARI_HOST8BF_PROTOCOL_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace mari::host8bf {

/// 공유 메모리 헤더 매직. "M8BF" 리틀엔디안.
inline constexpr u32 kShmMagic = 0x4642384du;

/// 프로토콜 버전. 본체와 호스트가 **따로 배포될 수 있다** — 안 맞으면 거절한다.
/// 필드는 뒤에만 더하고, 더할 때 버전을 올린다.
inline constexpr u32 kShmVersion = 1u;

/// 공유 메모리 이름 접두사. 뒤에 세션 GUID 문자열이 붙는다.
/// `Local\` 네임스페이스 — 세션 격리를 유지한다(서비스 계정이 못 본다).
inline constexpr const char* kShmNamePrefix = "Local\\MariPaint.8bf.";

/// 호스트가 한 번의 Apply() 안에서 살아 있을 수 있는 최대 시간(ms).
/// 무한루프 도는 플러그인은 여기서 잘린다(docs/02 6.2).
inline constexpr u32 kDefaultTimeoutMs = 30000u;

/// 픽셀 포맷. 8bf 필터가 실제로 다루는 것만 둔다. **뒤에만 더해라.**
enum class ShmPixelFormat : u32 {
    RGBA8 = 0, ///< 인터리브 8비트 RGBA, straight alpha
    RGB8 = 1,  ///< 인터리브 8비트 RGB (알파 없음)
    Gray8 = 2,
    RGBA16 = 3, ///< 채널당 16비트
};

/// 채널 수.
[[nodiscard]] constexpr u32 channelCountOf(ShmPixelFormat f) noexcept {
    switch (f) {
    case ShmPixelFormat::RGBA8:
        return 4u;
    case ShmPixelFormat::RGB8:
        return 3u;
    case ShmPixelFormat::Gray8:
        return 1u;
    case ShmPixelFormat::RGBA16:
        return 4u;
    }
    return 0u;
}

/// 채널 하나의 바이트 수.
[[nodiscard]] constexpr u32 bytesPerChannelOf(ShmPixelFormat f) noexcept {
    return f == ShmPixelFormat::RGBA16 ? 2u : 1u;
}

/// 픽셀 하나의 바이트 수.
[[nodiscard]] constexpr u32 bytesPerPixelOf(ShmPixelFormat f) noexcept {
    return channelCountOf(f) * bytesPerChannelOf(f);
}

/// 호스트가 돌려주는 상태. **뒤에만 더해라** — 본체가 값으로 분기한다.
enum class HostStatus : u32 {
    Ok = 0,
    PluginLoadFailed = 1,   ///< LoadLibrary 실패 또는 엔트리포인트 없음
    PluginRefused = 2,      ///< 필터가 filterBadParameters 등으로 거절
    PluginCrashed = 3,      ///< 호스트 프로세스가 비정상 종료(본체가 판정한다)
    TimedOut = 4,           ///< kDefaultTimeoutMs 초과 → 강제 종료
    UnsupportedFormat = 5,  ///< 이 픽셀 포맷을 플러그인이 못 받는다
    ShmError = 6,           ///< 매핑 실패·버전 불일치
    Cancelled = 7,          ///< 사용자가 취소
    ArchMismatch = 8,       ///< x86 플러그인을 x64 호스트에 넘겼다(또는 반대)
};

/// 상태 → 사람이 읽는 한국어 문자열. 리포트·로그용 안정 문자열.
[[nodiscard]] inline const char* hostStatusName(HostStatus s) noexcept {
    switch (s) {
    case HostStatus::Ok:
        return "성공";
    case HostStatus::PluginLoadFailed:
        return "플러그인을 열지 못했다";
    case HostStatus::PluginRefused:
        return "플러그인이 요청을 거절했다";
    case HostStatus::PluginCrashed:
        return "플러그인이 호스트 프로세스와 함께 죽었다";
    case HostStatus::TimedOut:
        return "시간 초과로 강제 종료했다";
    case HostStatus::UnsupportedFormat:
        return "플러그인이 이 픽셀 포맷을 지원하지 않는다";
    case HostStatus::ShmError:
        return "공유 메모리 오류";
    case HostStatus::Cancelled:
        return "사용자가 취소했다";
    case HostStatus::ArchMismatch:
        return "플러그인 아키텍처가 호스트와 맞지 않는다";
    }
    return "알 수 없음";
}

/// 공유 메모리 첫 블록. 본체가 쓰고 호스트가 읽는다.
/// 🔴 **x86 호스트와 x64 본체가 같은 바이트를 봐야 한다.** 포인터·size_t 를 넣지 마라.
///    전부 고정 폭 정수다. 패딩도 명시적으로 박는다.
#pragma pack(push, 4)
struct ShmHeader {
    u32 magic;       ///< kShmMagic
    u32 version;     ///< kShmVersion
    u32 format;      ///< ShmPixelFormat
    u32 width;       ///< px
    u32 height;      ///< px
    u32 rowBytes;    ///< 행 스트라이드. width*bpp 이상
    u64 pixelOffset; ///< 헤더 시작점 기준 픽셀 데이터 오프셋
    u64 pixelBytes;  ///< 픽셀 데이터 바이트 수 = rowBytes*height
    // 필터가 볼 선택 영역(캔버스 좌표 기준이 아니라 **이 버퍼 기준**이다).
    i32 selLeft;
    i32 selTop;
    i32 selRight;  ///< 배타 경계 (core 의 Rect 규약과 같다)
    i32 selBottom; ///< 배타 경계
    u32 status;    ///< HostStatus. 호스트가 끝내면서 쓴다
    u32 progress;  ///< 0..100. 호스트가 갱신, 본체가 읽는다
    u32 cancel;    ///< 본체가 1 로 쓰면 호스트가 필터를 중단시킨다
    /// 🔴 호스트가 Apply 시작 직후 자기 PID 를 여기 적는다.
    ///    타임아웃이 걸렸을 때 본체가 이 PID 로 **강제 종료**한다(docs/02 6.2).
    ///    COM 에는 서버 PID 를 알아내는 공개 API 가 없어서 이 경로가 필요하다.
    u32 hostPid;
    u32 reserved1; ///< 0 으로 채운다. 뒤에 필드를 더할 자리
};
#pragma pack(pop)

static_assert(sizeof(ShmHeader) == 76, "ShmHeader 는 x86/x64 에서 같은 크기여야 한다");
static_assert(offsetof(ShmHeader, pixelOffset) == 24, "pixelOffset 오프셋 고정");
static_assert(offsetof(ShmHeader, status) == 56, "status 오프셋 고정");

/// 픽셀 데이터를 헤더 뒤 이 경계에 맞춘다. SSE/AVX 로 읽는 필터가 있다.
inline constexpr u64 kPixelAlign = 64u;

/// 헤더 + 정렬을 고려한 픽셀 시작 오프셋.
[[nodiscard]] constexpr u64 pixelOffsetFor() noexcept {
    const u64 raw = sizeof(ShmHeader);
    return (raw + kPixelAlign - 1u) / kPixelAlign * kPixelAlign;
}

/// 매핑 레이아웃 계산 결과.
struct ShmLayout {
    u32 rowBytes = 0;
    u64 pixelOffset = 0;
    u64 pixelBytes = 0;
    u64 totalBytes = 0;
};

/// 공유 메모리에 얼마를 잡아야 하는지 계산한다.
///
/// 🔴 여기서 **오버플로를 반드시 잡는다.** width*height*bpp 는 u32 를 우습게 넘긴다.
///    8bf 플러그인은 신뢰할 수 없는 코드다 — 크기 계산이 한 번 뚫리면 힙이 뚫린다.
[[nodiscard]] inline Result<ShmLayout> computeLayout(ShmPixelFormat fmt, u32 width,
                                                     u32 height) noexcept {
    if (width == 0u || height == 0u) {
        return Err("8bf 버퍼 크기가 0 이다", ErrorCode::InvalidArgument);
    }
    const u32 bpp = bytesPerPixelOf(fmt);
    if (bpp == 0u) {
        return Err("알 수 없는 픽셀 포맷이다", ErrorCode::Unsupported);
    }
    // 4GB 를 넘는 단일 필터 버퍼는 거절한다. x86 호스트가 매핑할 수 없다.
    constexpr u64 kMaxPixelBytes = 0xC0000000ull; ///< 3GB — x86 유저 주소공간 여유를 남긴다
    const u64 rowBytes64 = static_cast<u64>(width) * static_cast<u64>(bpp);
    if (rowBytes64 > 0xFFFFFFFFull) {
        return Err("행 스트라이드가 32비트를 넘는다", ErrorCode::InvalidArgument);
    }
    const u64 pixelBytes = rowBytes64 * static_cast<u64>(height);
    if (pixelBytes / static_cast<u64>(height) != rowBytes64) {
        return Err("픽셀 크기 계산이 오버플로했다", ErrorCode::InvalidArgument);
    }
    if (pixelBytes > kMaxPixelBytes) {
        return Err("8bf 버퍼가 너무 크다(3GB 초과). 타일을 나눠서 넘겨라",
                   ErrorCode::InvalidArgument);
    }
    ShmLayout l;
    l.rowBytes = static_cast<u32>(rowBytes64);
    l.pixelOffset = pixelOffsetFor();
    l.pixelBytes = pixelBytes;
    l.totalBytes = l.pixelOffset + pixelBytes;
    return l;
}

/// 헤더가 우리가 아는 것인지 검사한다. **호스트는 본체를 신뢰하지 않고, 본체도 호스트를
/// 신뢰하지 않는다.** 둘 다 읽기 전에 이걸 부른다.
[[nodiscard]] inline Result<void> validateHeader(const ShmHeader& h, u64 mappedBytes) noexcept {
    if (h.magic != kShmMagic) {
        return Err("공유 메모리 매직이 틀렸다", ErrorCode::ParseError);
    }
    if (h.version != kShmVersion) {
        return Err("8bf 호스트 프로토콜 버전이 다르다 — 본체와 호스트를 같이 갱신해라",
                   ErrorCode::Unsupported);
    }
    if (h.width == 0u || h.height == 0u) {
        return Err("버퍼 크기가 0 이다", ErrorCode::ParseError);
    }
    const u32 bpp = bytesPerPixelOf(static_cast<ShmPixelFormat>(h.format));
    if (bpp == 0u) {
        return Err("알 수 없는 픽셀 포맷이다", ErrorCode::Unsupported);
    }
    if (h.rowBytes < static_cast<u64>(h.width) * bpp) {
        return Err("행 스트라이드가 폭보다 작다", ErrorCode::ParseError);
    }
    const u64 need = static_cast<u64>(h.rowBytes) * static_cast<u64>(h.height);
    if (h.pixelBytes != need) {
        return Err("픽셀 바이트 수가 스트라이드·높이와 맞지 않는다", ErrorCode::ParseError);
    }
    if (h.pixelOffset < sizeof(ShmHeader)) {
        return Err("픽셀 오프셋이 헤더를 침범한다", ErrorCode::ParseError);
    }
    if (h.pixelOffset + h.pixelBytes > mappedBytes) {
        return Err("픽셀 영역이 매핑 크기를 넘는다", ErrorCode::ParseError);
    }
    // 선택 영역은 버퍼 안이어야 한다. 반열림 [left,right) × [top,bottom).
    if (h.selLeft < 0 || h.selTop < 0 || h.selRight < h.selLeft || h.selBottom < h.selTop ||
        static_cast<u32>(h.selRight) > h.width || static_cast<u32>(h.selBottom) > h.height) {
        return Err("선택 영역이 버퍼 밖이다", ErrorCode::ParseError);
    }
    return Ok();
}

/// 플러그인 한 개가 광고하는 필터 하나(PIPL 에서 뽑는다).
struct PluginEntry {
    std::string category;  ///< 메뉴 상위 항목 (PiPL 'catg')
    std::string name;      ///< 메뉴 이름 (PiPL 'nmae'/'Nm  ')
    std::string entryName; ///< 엔트리포인트 심볼 (PiPL 'wx86'/'8664' → "PluginMain")
    u32 wantsBits = 0;     ///< 지원 비트 심도 마스크(1<<8, 1<<16, 1<<32)
    bool supportsRGB = true;
    bool supportsGray = false;
    bool is64Bit = false; ///< 'x64 ' 코드 리소스가 있었나
};

/// 한 .8bf 파일을 훑은 결과.
struct PluginManifest {
    std::string path;
    std::vector<PluginEntry> entries;
    /// 번역·해석 못 한 것을 정직하게 남긴다. 조용히 버리지 않는다(docs/02 5절 원칙).
    std::vector<std::string> notes;
};

} // namespace mari::host8bf

#endif // MARI_HOST8BF_PROTOCOL_HPP
