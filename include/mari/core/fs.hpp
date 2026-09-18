// Mari Paint — 경로 규약: **std::string 경로는 언제나 UTF-8 이다.**
//
// 🔴 Windows 의 `std::filesystem::path(std::string)` 과 `std::fopen(const char*)` 은 그 문자열을
//    ANSI 코드페이지(CP949)로 읽는다. 사용자 이름에 한글이 있으면 LOCALAPPDATA 경로가
//    "No mapping for the Unicode character" 로 터진다(실측 2026-09-18, GUI 첫 실행).
//    매니페스트로 프로세스 코드페이지를 UTF-8 로 바꾸는 우회도 있지만, 그건 실행파일마다
//    달아야 하고 테스트 exe·호스트 exe 는 빠뜨리기 쉽다. 그래서 여는 쪽에서 고친다.
//
// 리포 안에서 파일을 여는 코드는 std::fopen / std::filesystem::path(std::string) 을 직접 쓰지 말고
// 여기 것을 쓴다. POSIX 에서는 그대로 통과한다(이미 UTF-8 이다).
#ifndef MARI_CORE_FS_HPP
#define MARI_CORE_FS_HPP

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <cwchar>
#include <share.h>
#endif

namespace mari {

/// UTF-8 문자열 → std::filesystem::path. Windows 에서는 UTF-16 으로 변환해 만든다.
[[nodiscard]] inline std::filesystem::path fsPath(std::string_view utf8) {
#if defined(_WIN32)
    // C++20: u8string 으로 만들면 표준이 UTF-8 로 해석한다(코드페이지 무관).
    try {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()),
                                                   utf8.size()));
    } catch (const std::exception&) {
        // ⚠️ 과도기 폴백. 유효한 UTF-8 이 아니면(예: temp_directory_path().string() 이 낸 CP949
        //    바이트열) ANSI 로 해석한다. CP949 한글은 유효한 UTF-8 이 아니라 둘이 섞이지 않는다.
        //    새 코드는 pathToUtf8() 로 문자열을 만들어야 한다 — 이 분기는 옛 호출자를 살리는 용도다.
        return std::filesystem::path(std::string(utf8));
    }
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

/// std::filesystem::path → UTF-8 문자열. `path::string()` 은 Windows 에서 ANSI 로 나오므로 쓰지 않는다.
[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& p) {
#if defined(_WIN32)
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
#else
    return p.string();
#endif
}

/// UTF-8 경로로 fopen. 실패하면 nullptr(std::fopen 과 같다).
[[nodiscard]] inline std::FILE* fopenUtf8(std::string_view utf8Path, const char* mode) {
#if defined(_WIN32)
    const std::filesystem::path p = fsPath(utf8Path);
    std::wstring wmode;
    for (const char* m = mode; *m != '\0'; ++m) {
        wmode.push_back(static_cast<wchar_t>(*m));
    }
    // ⚠️ _wfopen_s 는 배타 모드(_SH_DENYRW)로 연다 — 저널을 쓰는 중에 scan() 이 같은 파일을 못 연다
    //    (실측: sigan_journal 테스트 실패). std::fopen 과 같은 공유 의미론을 쓰려면 _wfsopen 이다.
    return ::_wfsopen(p.c_str(), wmode.c_str(), _SH_DENYNO);
#else
    return std::fopen(std::string(utf8Path).c_str(), mode);
#endif
}

} // namespace mari

#endif // MARI_CORE_FS_HPP
