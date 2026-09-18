// Mari Paint — 테스트용 OS 유틸 (헤더온리)
//
// 테스트가 OS 에 기대는 두 가지를 한곳에 모았다:
//   1. runProcess()  — 바이너리를 띄우고 stdout/stderr 를 **파일로 따로** 받는다.
//   2. rssBytes()    — 현재 프로세스의 RSS.
//
// 🔴 POSIX 쪽은 예전 그대로 std::system 이다(셸이 128+signal 을 돌려주면 그대로 드러난다).
//    Windows 쪽은 cmd.exe 를 거치지 않는다. 이유 세 가지:
//      - std::system 의 반환값 규약이 다르다(리눅스 rc*256, Windows rc 그대로)
//      - cmd.exe 는 작은따옴표를 모른다. 테스트가 '{"op":...}' 처럼 넘긴다.
//      - `env -u DISPLAY` 같은 접두어가 없다. Windows 에는 DISPLAY 개념 자체가 없으니 무시한다.
//    그래서 인자 문자열을 POSIX 셸 규칙으로 토큰화한 뒤 MSVC CRT 규칙으로 다시 인용해
//    CreateProcessW 에 넘긴다. 테스트 소스는 한 벌로 유지된다.
#ifndef MARI_TEST_SYS_HPP
#define MARI_TEST_SYS_HPP

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#endif

namespace mari::test {

struct ProcessResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

inline std::string slurpFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
namespace detail {

/// POSIX 셸 규칙(작은따옴표 = 그대로, 큰따옴표 안 \" = ", 공백 = 구분)으로 토큰화.
inline std::vector<std::string> shellSplit(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inTok = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'') {
            inTok = true;
            for (++i; i < s.size() && s[i] != '\''; ++i) cur += s[i];
        } else if (c == '"') {
            inTok = true;
            for (++i; i < s.size() && s[i] != '"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) ++i;
                cur += s[i];
            }
        } else if (c == ' ' || c == '\t') {
            if (inTok) { out.push_back(cur); cur.clear(); inTok = false; }
        } else {
            inTok = true;
            cur += c;
        }
    }
    if (inTok) out.push_back(cur);
    return out;
}

/// MSVC CRT 가 argv 로 되돌릴 수 있게 인용한다(백슬래시-따옴표 규칙).
inline std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    std::size_t bs = 0;
    for (const wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += c; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0;
        r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline HANDLE openRedirect(const std::filesystem::path& p, bool forWrite) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return ::CreateFileW(p.c_str(),
                         forWrite ? GENERIC_WRITE : GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         forWrite ? CREATE_ALWAYS : OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace detail
#endif

/// `exe` 를 `cwd` 에서 `args`(POSIX 셸 문법 한 줄) 로 띄운다.
/// stdout/stderr 는 `cwd/stdout.txt`, `cwd/stderr.txt` 로 받는다.
/// `envPrefix` 는 POSIX 에서만 명령 앞에 그대로 붙는다(`env -u DISPLAY ` 등).
/// `stdinFile` 이 비어 있지 않으면 표준 입력으로 준다.
inline ProcessResult runProcess(const std::string& exe,
                                const std::filesystem::path& cwd,
                                const std::string& args,
                                const std::string& envPrefix = std::string{},
                                const std::string& stdinFile = std::string{}) {
    const std::filesystem::path outPath = cwd / "stdout.txt";
    const std::filesystem::path errPath = cwd / "stderr.txt";
    ProcessResult r;
#if defined(_WIN32)
    (void)envPrefix;
    std::wstring cmdline = detail::quoteArg(detail::widen(exe));
    for (const std::string& a : detail::shellSplit(args)) {
        cmdline += L' ';
        cmdline += detail::quoteArg(detail::widen(a));
    }

    HANDLE hOut = detail::openRedirect(outPath, true);
    HANDLE hErr = detail::openRedirect(errPath, true);
    HANDLE hIn = INVALID_HANDLE_VALUE;
    if (!stdinFile.empty()) {
        std::filesystem::path inPath(stdinFile);
        if (inPath.is_relative()) inPath = cwd / inPath;
        hIn = detail::openRedirect(inPath, false);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    si.hStdInput = hIn != INVALID_HANDLE_VALUE ? hIn : ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    // CreateProcessW 는 명령줄을 고칠 수 있어 쓰기 가능한 버퍼가 필요하다.
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');
    const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                     cwd.c_str(), &si, &pi);
    if (ok) {
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = static_cast<int>(code);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    if (hOut != INVALID_HANDLE_VALUE) ::CloseHandle(hOut);
    if (hErr != INVALID_HANDLE_VALUE) ::CloseHandle(hErr);
    if (hIn != INVALID_HANDLE_VALUE) ::CloseHandle(hIn);
#else
    std::string cmd = "cd " + cwd.string() + " && " + envPrefix + "\"" + exe + "\" " + args;
    if (!stdinFile.empty()) {
        cmd += " < " + stdinFile;
    }
    cmd += " > " + outPath.string() + " 2> " + errPath.string();
    const int rc = std::system(cmd.c_str());
    // WEXITSTATUS 를 쓰지 않는 이유: 이식성. 셸이 128+signal 로 돌려주면 그대로 드러난다.
    r.exitCode = (rc == -1) ? -1 : (rc / 256);
#endif
    r.out = slurpFile(outPath);
    r.err = slurpFile(errPath);
    return r;
}

/// 현재 프로세스의 RSS(바이트). 못 읽으면 0.
inline std::size_t rssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return 0;
    }
    return static_cast<std::size_t>(pmc.WorkingSetSize);
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long long total = 0;
    unsigned long long resident = 0;
    const int n = std::fscanf(f, "%llu %llu", &total, &resident);
    (void)std::fclose(f);
    if (n != 2) {
        return 0;
    }
    return static_cast<std::size_t>(resident) * 4096u; // 리눅스 x86-64 페이지 크기
#endif
}

} // namespace mari::test

#endif // MARI_TEST_SYS_HPP
