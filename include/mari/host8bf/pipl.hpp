// Mari Paint — PIPL 리소스 파서 (.8bf 가 자기를 광고하는 방식)
//
// .8bf 는 그냥 DLL 이다. 하지만 "내 이름이 뭐고, 메뉴 어디에 붙고, 엔트리포인트
// 심볼이 뭔지" 는 `PiPL` 이라는 커스텀 리소스에 들어 있다. 그걸 읽어야 메뉴를 만든다.
//
// 레이아웃(Windows 리소스):
//   int16 version           [추정] 실측 플러그인에서 0 또는 1
//   int16 count             프로퍼티 개수
//   count 번 반복:
//     char[4] vendorId      '8BIM'
//     char[4] key           'catg' / 'Nm  ' / 'wx86' / '8664' / 'mode' ...
//     int32   id            보통 0
//     int32   length        데이터 바이트 수
//     byte[length]          데이터. **다음 프로퍼티는 4바이트 경계로 정렬된다**
//
// 신뢰도: 프로퍼티 반복 구조는 [확정](Mac/Windows 공통, 공개 문서와 여러 독립
// 구현이 일치). 맨 앞 int16 두 개는 [추정] — Windows 전용 프리픽스라 SDK 문서가
// 얇다. 그래서 파서는 프리픽스가 말이 안 되면 **조용히 넘어가지 않고 note 를 남긴다.**
//
// 이 헤더는 **Win32 를 포함하지 않는다.** 바이트 버퍼만 받는다 →
// Linux 에서도 컴파일·테스트된다(tests/win/test_pipl.cpp).
#ifndef MARI_HOST8BF_PIPL_HPP
#define MARI_HOST8BF_PIPL_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>
#include <mari/host8bf/protocol.hpp>

#include <string>
#include <vector>

namespace mari::host8bf {

/// Windows 리소스에서 PIPL 을 찾을 때 쓰는 타입 이름.
inline constexpr const char* kPiplResourceType = "PiPL";

/// 프로퍼티 하나(키는 4글자 그대로 둔다 — 엔디안 헷갈릴 일이 없다).
struct PiplProperty {
    std::string vendorId; ///< 보통 "8BIM"
    std::string key;      ///< "catg", "Nm  ", "wx86", "8664", "mode", "kind" ...
    i32 id = 0;
    std::vector<u8> data;
};

/// 리소스 한 덩어리를 프로퍼티 목록으로 푼다.
/// 잘린 리소스·말도 안 되는 길이는 **거절한다**(신뢰할 수 없는 파일이다).
[[nodiscard]] inline Result<std::vector<PiplProperty>> parsePipl(const u8* data, usize size,
                                                                 std::vector<std::string>& notes) {
    if (data == nullptr || size < 4) {
        return Err("PiPL 리소스가 너무 짧다", ErrorCode::ParseError);
    }
    auto rd16 = [data](usize off) -> u16 {
        return static_cast<u16>(static_cast<u16>(data[off]) |
                                static_cast<u16>(static_cast<u16>(data[off + 1]) << 8));
    };
    auto rd32 = [data](usize off) -> u32 {
        return static_cast<u32>(data[off]) | (static_cast<u32>(data[off + 1]) << 8) |
               (static_cast<u32>(data[off + 2]) << 16) | (static_cast<u32>(data[off + 3]) << 24);
    };

    const u16 version = rd16(0);
    const u16 count = rd16(2);
    if (version > 1u) {
        // 조용히 버리지 않는다. 계속 시도하되 사용자에게 알린다.
        notes.push_back("PiPL 버전 " + std::to_string(version) +
                        " 는 우리가 아는 값(0/1)이 아니다. 그래도 읽어 본다");
    }
    if (count == 0u) {
        return Err("PiPL 에 프로퍼티가 하나도 없다", ErrorCode::ParseError);
    }
    if (static_cast<usize>(count) * 16u > size) {
        return Err("PiPL 프로퍼티 개수가 리소스 크기와 맞지 않는다", ErrorCode::ParseError);
    }

    std::vector<PiplProperty> out;
    out.reserve(count);

    usize off = 4;
    for (u16 i = 0; i < count; ++i) {
        if (off + 16u > size) {
            notes.push_back("PiPL 이 프로퍼티 " + std::to_string(i) + " 중간에서 잘렸다");
            break;
        }
        PiplProperty p;
        p.vendorId.assign(reinterpret_cast<const char*>(data + off), 4);
        p.key.assign(reinterpret_cast<const char*>(data + off + 4), 4);
        p.id = static_cast<i32>(rd32(off + 8));
        const u32 len = rd32(off + 12);
        off += 16;
        if (len > size || off + len > size) {
            notes.push_back("PiPL 프로퍼티 '" + p.key + "' 의 길이가 리소스를 넘는다 — 버린다");
            break;
        }
        p.data.assign(data + off, data + off + len);
        // 다음 프로퍼티는 4바이트 경계로 정렬된다. [확정]
        const u32 padded = (len + 3u) & ~3u;
        if (off + padded < off) { // 산술 오버플로 방어
            break;
        }
        off += padded;
        out.push_back(std::move(p));
    }

    if (out.empty()) {
        return Err("PiPL 에서 읽어 낸 프로퍼티가 없다", ErrorCode::ParseError);
    }
    return out;
}

/// 길이 제한이 있는 데이터에서 C 문자열을 안전하게 꺼낸다(널이 없어도 끊어 준다).
[[nodiscard]] inline std::string cstrFrom(const std::vector<u8>& d) {
    usize n = 0;
    while (n < d.size() && d[n] != 0u) {
        ++n;
    }
    return std::string(reinterpret_cast<const char*>(d.data()), n);
}

/// Pascal 문자열(첫 바이트가 길이)에서 문자열을 꺼낸다.
/// PIPL 의 'Nm  '/'catg' 는 **Pascal 문자열인 플러그인과 C 문자열인 플러그인이 섞여 있다.**
/// 그래서 둘 다 시도하고, 판단 근거를 note 로 남긴다.
[[nodiscard]] inline std::string pstrOrCstrFrom(const std::vector<u8>& d) {
    if (d.empty()) {
        return {};
    }
    const usize plen = d[0];
    // 첫 바이트가 나머지 길이에 딱 맞고, 그 뒤가 인쇄 가능한 문자면 Pascal 로 본다.
    if (plen > 0 && plen + 1u <= d.size()) {
        bool printable = true;
        for (usize i = 1; i <= plen; ++i) {
            if (d[i] < 0x20u) {
                printable = false;
                break;
            }
        }
        if (printable) {
            return std::string(reinterpret_cast<const char*>(d.data() + 1), plen);
        }
    }
    return cstrFrom(d);
}

/// 프로퍼티 목록 → 메뉴에 걸 항목 하나.
/// 못 알아본 프로퍼티는 **전부 note 에 남긴다.** 조용히 버리지 않는다(docs/02 5절).
[[nodiscard]] inline Result<PluginEntry> piplToEntry(const std::vector<PiplProperty>& props,
                                                     std::vector<std::string>& notes) {
    PluginEntry e;
    bool sawCode = false;

    for (const PiplProperty& p : props) {
        if (p.key == "catg") {
            e.category = pstrOrCstrFrom(p.data);
        } else if (p.key == "Nm  ") {
            e.name = pstrOrCstrFrom(p.data);
        } else if (p.key == "wx86") {
            // 32비트 Windows 코드. 데이터는 엔트리포인트 심볼 이름이다. [확정]
            if (!sawCode || !e.is64Bit) {
                e.entryName = cstrFrom(p.data);
            }
            sawCode = true;
        } else if (p.key == "8664") {
            // 64비트 Windows 코드. 있으면 이쪽을 우선한다. [확정]
            e.entryName = cstrFrom(p.data);
            e.is64Bit = true;
            sawCode = true;
        } else if (p.key == "mode") {
            // 지원 이미지 모드 비트마스크. 비트 index = kPlugInMode* 값. [확정]
            if (p.data.size() >= 4) {
                const u32 mask = static_cast<u32>(p.data[0]) |
                                 (static_cast<u32>(p.data[1]) << 8) |
                                 (static_cast<u32>(p.data[2]) << 16) |
                                 (static_cast<u32>(p.data[3]) << 24);
                e.supportsRGB = (mask & (1u << 3)) != 0u;  ///< kPlugInModeRGBColor
                e.supportsGray = (mask & (1u << 1)) != 0u; ///< kPlugInModeGrayScale
            } else {
                notes.push_back("'mode' 프로퍼티가 4바이트가 아니다 — 모드 정보를 못 읽었다");
            }
        } else if (p.key == "kind" || p.key == "vers" || p.key == "ms  " || p.key == "fici" ||
                   p.key == "prty" || p.key == "flag") {
            // 알고는 있지만 아직 안 쓰는 것들. 굳이 시끄럽게 굴지 않는다.
        } else {
            notes.push_back("PiPL 프로퍼티 '" + p.key + "' 는 번역하지 않았다");
        }
    }

    if (!sawCode) {
        return Err("PiPL 에 Windows 코드 프로퍼티('wx86'/'8664')가 없다 — "
                   "Mac 전용 플러그인이거나 손상됐다",
                   ErrorCode::Unsupported);
    }
    if (e.entryName.empty()) {
        notes.push_back("엔트리포인트 이름이 비어 있다 — 'PluginMain' 으로 시도한다");
        e.entryName = "PluginMain";
    }
    if (e.name.empty()) {
        notes.push_back("플러그인 이름을 못 읽었다 — 파일 이름으로 대신한다");
    }
    return e;
}

/// 이 항목을 지금 호스트(비트수)에서 돌릴 수 있는가.
/// x86 호스트가 'wx86' 만 쓰고, x64 호스트가 '8664' 만 쓴다. 섞으면 LoadLibrary 가
/// 실패하거나, 더 나쁘게는 이상하게 성공한다.
[[nodiscard]] inline bool entryMatchesHost(const PluginEntry& e, bool host64) noexcept {
    return e.is64Bit == host64;
}

} // namespace mari::host8bf

#endif // MARI_HOST8BF_PIPL_HPP
