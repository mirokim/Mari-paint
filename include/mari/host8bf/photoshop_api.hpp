// Mari Paint — Photoshop 플러그인 ABI 최소 선언 (Adobe SDK 헤더 없이)
//
// ════════════════════════════════════════════════════════════════════════════
// 🔴 이 파일의 신뢰도에 대한 정직한 고지 (docs/02 5절 "정직하게 실패한다")
//
// Adobe Photoshop SDK 는 EULA 때문에 리포에 넣을 수 없다. 그래서 공개 문서
// (Photoshop API Guide, PIFilter.h / PIGeneral.h / PIAbout.h 에 대한 공개 설명)
// 을 근거로 **다시 선언**했다.
//
// 신뢰도 등급을 필드마다 표시한다:
//   [확정] 공개 문서가 값·순서를 명시한다. 여러 오픈소스 구현이 일치한다.
//   [추정] 순서는 문서를 따랐지만 **바이트 오프셋을 실물과 대조하지 않았다.**
//
// 🔴 `FilterRecord` 는 통째로 [추정] 이다. 필드가 100개 가까이 되고 버전마다
//    자랐다. 오프셋이 하나만 틀려도 남의 코드가 우리 힙을 밟는다.
//    → 그래서 **기본적으로 플러그인 호출을 막아 두었다.** 실물 `PIFilter.h` 와
//      대조한 사람이 빌드에 `MARI_8BF_FILTERRECORD_VERIFIED` 를 정의해야 풀린다.
//      정의되지 않으면 호스트는 `HostStatus::PluginLoadFailed` 와 함께
//      "FilterRecord 레이아웃이 아직 검증되지 않았다" 를 돌려준다.
//
//    Adobe SDK 를 가진 사람은 `MARI_8BF_USE_ADOBE_SDK` 를 켜라. 그러면 아래
//    선언 대신 진짜 헤더를 쓴다. 우리 코드는 전부 **필드 이름으로만** 접근하므로
//    그대로 컴파일된다 — 그게 이 선언의 존재 이유다.
// ════════════════════════════════════════════════════════════════════════════
#ifndef MARI_HOST8BF_PHOTOSHOP_API_HPP
#define MARI_HOST8BF_PHOTOSHOP_API_HPP

#include <cstdint>

#if defined(MARI_8BF_USE_ADOBE_SDK)
// 진짜 SDK 를 쓴다. 인클루드 경로는 빌드 쪽에서 준다.
#include <PIFilter.h>
#include <PIGeneral.h>
#else

namespace mari::host8bf::ps {

// ── 기본 타입 [확정] ────────────────────────────────────────────────────────
// Adobe 는 옛 Mac Toolbox 타입을 그대로 물려받았다. Windows 빌드에서도 같다.
using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using OSErr = std::int16_t;
using OSType = std::uint32_t; ///< 4CC. 'PiPL' 같은 것
using Ptr = char*;
using Handle = Ptr*; ///< [확정] 핸들은 포인터의 포인터다
using Boolean = unsigned char;

/// [확정] Mac Toolbox 순서 그대로다 — **top, left, bottom, right**.
/// 🔴 Win32 RECT(left, top, right, bottom) 와 순서가 다르다. 섞으면 그림이 뒤집힌다.
struct PsRect {
    int16 top;
    int16 left;
    int16 bottom;
    int16 right;
};

/// [확정] Mac Toolbox 순서 — **v(세로), h(가로)**. 역시 뒤집혀 있다.
struct PsPoint {
    int16 v;
    int16 h;
};

/// [확정] 큰 문서(30000px 초과)용 32비트 사각형.
struct VRect {
    int32 top;
    int32 left;
    int32 bottom;
    int32 right;
};

/// [확정] 0..65535 채널.
struct RGBColor {
    uint16 red;
    uint16 green;
    uint16 blue;
};

// ── 셀렉터 [확정] ───────────────────────────────────────────────────────────
// 엔트리포인트가 받는 값. 호출 순서는 About / Parameters → Prepare → Start →
// Continue* → Finish 다.
inline constexpr int16 kFilterSelectorAbout = 0;
inline constexpr int16 kFilterSelectorParameters = 1;
inline constexpr int16 kFilterSelectorPrepare = 2;
inline constexpr int16 kFilterSelectorStart = 3;
inline constexpr int16 kFilterSelectorContinue = 4;
inline constexpr int16 kFilterSelectorFinish = 5;

// ── 결과 코드 [확정] ────────────────────────────────────────────────────────
inline constexpr OSErr kNoErr = 0;
inline constexpr OSErr kUserCanceledErr = -128;    ///< 옛 Mac memFullErr 이웃. 사용자 취소
inline constexpr OSErr kFilterBadParameters = -30100;
inline constexpr OSErr kFilterBadMode = -30101;
/// [확정] 플러그인이 "내가 직접 에러를 보고했다"고 알리는 값. 호스트는 조용히 끝낸다.
inline constexpr OSErr kErrReportString = -30101 - 1; ///< ⚠️ [추정] 실제 값은 SDK 대조 필요

// ── 이미지 모드 [확정] ──────────────────────────────────────────────────────
inline constexpr int16 kPlugInModeBitmap = 0;
inline constexpr int16 kPlugInModeGrayScale = 1;
inline constexpr int16 kPlugInModeIndexedColor = 2;
inline constexpr int16 kPlugInModeRGBColor = 3;
inline constexpr int16 kPlugInModeCMYKColor = 4;
inline constexpr int16 kPlugInModeGray16 = 11;
inline constexpr int16 kPlugInModeRGB48 = 12;

// ── 콜백 타입 [확정] ────────────────────────────────────────────────────────
/// 호스트가 "취소 눌렀나?" 에 답한다. **플러그인이 자주 부른다 — 여기서 타임아웃도 본다.**
using TestAbortProc = Boolean (*)();
/// 진행률 보고. done/total.
using ProgressProc = void (*)(int32 done, int32 total);
/// 호스트 확장 호출. 우리는 거의 전부 거절한다.
using HostProc = OSErr (*)(int16 selector, int32* data);

using BufferID = int32; ///< [추정] 64비트 호스트에서 intptr_t 인 버전이 있다

// ── BufferProcs [확정] ──────────────────────────────────────────────────────
// 플러그인이 임시 버퍼를 우리한테 달라고 하는 통로. **여기를 안 주면 대부분의
// 필터가 즉시 죽는다.** 최소 제공 4종 중 첫 번째.
inline constexpr int16 kCurrentBufferProcsVersion = 2;
inline constexpr int16 kCurrentBufferProcsCount = 5;

struct BufferProcs {
    int16 bufferProcsVersion;
    int16 numBufferProcs;
    OSErr (*allocateProc)(int32 size, BufferID* bufferID);
    Ptr (*lockProc)(BufferID bufferID, Boolean moveHigh);
    void (*unlockProc)(BufferID bufferID);
    void (*freeProc)(BufferID bufferID);
    int32 (*spaceProc)();
};

// ── HandleProcs [확정] ──────────────────────────────────────────────────────
// Mac 스타일 핸들(이중 포인터). 파라미터 저장·리소스 반환에 쓴다.
inline constexpr int16 kCurrentHandleProcsVersion = 1;
inline constexpr int16 kCurrentHandleProcsCount = 8;

struct HandleProcs {
    int16 handleProcsVersion;
    int16 numHandleProcs;
    Handle (*newProc)(int32 size);
    void (*disposeProc)(Handle h);
    int32 (*getSizeProc)(Handle h);
    OSErr (*setSizeProc)(Handle h, int32 newSize);
    Ptr (*lockProc)(Handle h, Boolean moveHigh);
    void (*unlockProc)(Handle h);
    void (*recoverSpaceProc)(int32 size);
    void (*disposeRegularHandleProc)(Handle h);
};

// ── PropertyProcs [확정] ────────────────────────────────────────────────────
// 문서 제목·해상도·채널수 같은 것을 묻는다. 없으면 필터가 기본값으로 헤매거나 죽는다.
inline constexpr int16 kCurrentPropertyProcsVersion = 1;
inline constexpr int16 kCurrentPropertyProcsCount = 2;

struct PropertyProcs {
    int16 propertyProcsVersion;
    int16 numPropertyProcs;
    OSErr (*getPropertyProc)(OSType signature, OSType key, int32 index, std::intptr_t* simple,
                             Handle* complex);
    OSErr (*setPropertyProc)(OSType signature, OSType key, int32 index, std::intptr_t simple,
                             Handle complex);
};

/// [확정] 우리가 답해 주는 프로퍼티 키(4CC). 나머지는 errPlugInPropertyUndefined 로 거절한다.
inline constexpr OSType kPsSignature = 0x3842494Du;   ///< '8BIM'
inline constexpr OSType kPropTitle = 0x7469746Cu;     ///< 'titl'
inline constexpr OSType kPropImageMode = 0x6D6F6465u; ///< 'mode'
inline constexpr OSType kPropNumberOfChannels = 0x6E756368u; ///< 'nuch'
inline constexpr OSType kPropBigNudgeH = 0x626E6448u;        ///< 'bndH'
inline constexpr OSType kPropBigNudgeV = 0x626E6456u;        ///< 'bndV'
/// [확정] 정의하지 않은 프로퍼티에 대한 표준 거절 코드.
inline constexpr OSErr kErrPlugInPropertyUndefined = -30900;

// ── ResourceProcs [확정] ────────────────────────────────────────────────────
// 플러그인이 호스트가 들고 있는 리소스(패스·채널 등)를 뒤진다.
// 우리는 **count 0 으로 정직하게 비었다고 답한다.** 거짓 데이터를 주지 않는다.
inline constexpr int16 kCurrentResourceProcsVersion = 3;
inline constexpr int16 kCurrentResourceProcsCount = 4;

struct ResourceProcs {
    int16 resourceProcsVersion;
    int16 numResourceProcs;
    int32 (*countProc)(OSType type);
    Handle (*getProc)(OSType type, int16 index);
    void (*deleteProc)(OSType type, int16 index);
    OSErr (*addProc)(OSType type, Handle data);
};

// ── FilterRecord ⚠️ 전체가 [추정] ───────────────────────────────────────────
//
// 🔴 아래 필드 **순서**는 공개 문서를 따랐지만, 바이트 오프셋을 실물과 대조하지
//    않았다. 특히 이런 것들이 위험하다:
//      · 옛 16비트 필드 사이 정렬 패딩 — 컴파일러가 어디에 넣을지
//      · 64비트 빌드에서 포인터가 4→8 바이트로 자라며 생기는 정렬 구멍
//      · 버전별로 뒤에 덧붙은 필드(`bigDocumentData`, `channelPortProcs` 등)
//    → 검증 전에는 호스트가 플러그인 호출 자체를 거부한다(파일 상단 고지 참조).
//
// 우리 코드는 이 구조체를 **이름으로만** 읽고 쓴다. 진짜 PIFilter.h 로 바꿔 끼우면
// 재컴파일만으로 맞는다.
struct FilterRecord {
    int32 serialNumber;     ///< [추정]
    TestAbortProc abortProc;   ///< [확정] 있다는 것은 확실. 위치는 [추정]
    ProgressProc progressProc; ///< [확정] 있다는 것은 확실. 위치는 [추정]

    Handle parameters; ///< 플러그인이 만든 파라미터 핸들. 우리가 보관·재전달한다

    PsPoint imageSize; ///< 30000px 초과 문서에서는 무효. bigDocumentData 를 봐야 한다
    int16 planes;      ///< 채널 수
    PsRect filterRect; ///< 필터가 손댈 영역

    RGBColor background;
    RGBColor foreground;

    int32 maxSpace;    ///< 플러그인이 쓸 수 있다고 우리가 약속한 최대 바이트
    int32 bufferSpace; ///< 플러그인이 Prepare 에서 "이만큼 필요하다"고 적는다

    // ── 타일 왕복. Start/Continue 에서 이걸로 주고받는다 ──
    PsRect inRect;   ///< 플러그인이 "이 영역을 읽고 싶다"고 적는다 → 우리가 채워 준다
    int16 inLoPlane; ///< 읽고 싶은 채널 범위
    int16 inHiPlane;
    PsRect outRect; ///< 플러그인이 "이 영역에 쓰겠다"고 적는다
    int16 outLoPlane;
    int16 outHiPlane;

    void* inData;     ///< 우리가 채워 주는 읽기 버퍼
    int32 inRowBytes; ///< 그 스트라이드
    void* outData;    ///< 우리가 주는 쓰기 버퍼. **필터의 결과가 여기 들어온다**
    int32 outRowBytes;

    Boolean isFloating; ///< 떠 있는 선택 영역인가
    Boolean haveMask;   ///< 마스크가 있나
    Boolean autoMask;   ///< 호스트가 마스크를 자동 적용하나
    Boolean pad0;       ///< [추정] 정렬 패딩. 실물에는 다른 Boolean 이 있을 수 있다
    PsRect maskRect;
    void* maskData;
    int32 maskRowBytes;

    int32 hostSig;   ///< 우리 서명. 'MARI'
    HostProc hostProc; ///< 호스트 확장. 우리는 거의 다 거절한다
    int16 imageMode; ///< kPlugInModeRGBColor 등
    int16 pad1;      ///< [추정] 정렬 패딩

    int32 imageHRes; ///< 16.16 고정소수 DPI
    int32 imageVRes;

    // ── suite 포인터들. 여기가 남의 코드와 우리가 만나는 지점이다 ──
    BufferProcs* bufferProcs;
    ResourceProcs* resourceProcs;
    HandleProcs* handleProcs;
    PropertyProcs* propertyProcs;

    /// [추정] 우리가 구현하지 않는 suite 는 **전부 nullptr 로 둔다.**
    /// 플러그인 규약상 nullptr 은 "호스트가 지원 안 함"이고, 잘 만든 필터는 확인 후
    /// 우아하게 물러난다. 못 만든 필터는 죽는다 — 그래서 별도 프로세스에서 돌린다.
    void* descriptorParameters;
    void* channelPortProcs;
    void* imageServicesProcs;
    void* advanceStateProc;
    void* colorServicesProc;
    void* errorString; ///< 플러그인이 에러 메시지를 Pascal 문자열로 적는 자리

    /// [추정] 30000px 초과 문서용. 버전이 올라가며 뒤에 붙었다.
    void* bigDocumentData;

    /// [추정] 뒤에 더 있는 필드를 위한 여유. 실물과 대조 전까지 0 으로 채워 둔다.
    void* reserved[64];
};

/// 플러그인 엔트리포인트. [확정] 시그니처는 이게 맞다.
///   `void PluginMain(int16 selector, void* filterRecord, intptr_t* data, int16* result)`
/// `data` 는 플러그인이 호출 사이에 상태를 보관하는 칸이다 — **우리가 유지해 준다.**
using EntryPointProc = void (*)(int16 selector, void* filterRecord, std::intptr_t* data,
                                int16* result);

/// 흔한 엔트리포인트 심볼 이름. PIPL 이 없거나 못 읽었을 때 순서대로 시도한다.
inline constexpr const char* kFallbackEntryNames[] = {"PluginMain", "ENTRYPOINT", "main"};

} // namespace mari::host8bf::ps

#endif // MARI_8BF_USE_ADOBE_SDK

#endif // MARI_HOST8BF_PHOTOSHOP_API_HPP
