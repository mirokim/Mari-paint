// Mari Paint — 명명 파이프 싱크 (docs/03 4절 ②, 5.2)
//
// Windows 는 Sigan 의 WinTabBridge 와 같은 명명 파이프를 쓴다.
// POSIX 는 같은 이름의 유닉스 도메인 소켓으로 대체한다 — 개발·CI 가 Linux 라서
// **양쪽 다 컴파일되고 양쪽 다 동작해야 한다.**
#include <mari/sigan/pipe_sink.hpp>

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace mari::sigan {
namespace {

#ifndef _WIN32
/// 핸드셰이크(저빈도)용 타임아웃. 그리기 스레드는 여기 오지 않는다.
constexpr int kHandshakeTimeoutMs = 2000;
#endif

} // namespace

const char* sinkStatusName(SinkStatus s) noexcept {
    switch (s) {
    case SinkStatus::Ok: return "ok";
    case SinkStatus::WouldBlock: return "would-block";
    case SinkStatus::Disconnected: return "disconnected";
    }
    return "unknown";
}

std::string defaultPipePath() {
#ifdef _WIN32
    return std::string("\\\\.\\pipe\\") + kPipeName;
#else
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string dir = (runtime != nullptr && runtime[0] != '\0') ? runtime : "/tmp";
    return dir + "/" + kPipeName + ".sock";
#endif
}

PipeSink::PipeSink(std::string path, usize residualLimit)
    : path_(std::move(path)), residualLimit_(residualLimit) {}

PipeSink::~PipeSink() { close(); }

void PipeSink::close() noexcept {
#ifdef _WIN32
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif
    fd_ = -1;
    residual_.clear();
    residualHead_ = 0;
}

Result<void> PipeSink::connect() {
    close();
#ifdef _WIN32
    HANDLE h = ::CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return Err("Sigan 파이프에 붙지 못했다: " + path_, ErrorCode::IoError);
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT; // 🔴 논블로킹 — 그리기 스레드를 막지 않는다
    if (::SetNamedPipeHandleState(h, &mode, nullptr, nullptr) == 0) {
        ::CloseHandle(h);
        return Err("파이프를 논블로킹으로 바꾸지 못했다", ErrorCode::IoError);
    }
    handle_ = h;
    fd_ = 0; // connected() 용 표식
    return Ok();
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Err("소켓을 만들지 못했다", ErrorCode::IoError);
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() + 1 > sizeof(addr.sun_path)) {
        ::close(fd);
        return Err("소켓 경로가 너무 길다: " + path_, ErrorCode::InvalidArgument);
    }
    std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        // Sigan 이 안 떠 있는 게 정상 상태다 — 조용히 로컬 모드로 간다(docs/03 5.1).
        return Err("Sigan 채널에 붙지 못했다: " + path_, ErrorCode::NotFound);
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) { // 🔴 논블로킹 고정
        ::close(fd);
        return Err("소켓을 논블로킹으로 바꾸지 못했다", ErrorCode::IoError);
    }
    fd_ = fd;
    return Ok();
#endif
}

long PipeSink::rawWrite(const u8* data, usize len) noexcept {
    if (!connected() || len == 0) {
        return connected() ? 0 : -1;
    }
#ifdef _WIN32
    DWORD written = 0;
    if (::WriteFile(static_cast<HANDLE>(handle_), data, static_cast<DWORD>(len), &written,
                    nullptr) == 0) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) {
            return -1; // 끊겼다
        }
        return 0; // 지금은 못 받는다
    }
    return static_cast<long>(written);
#else
    for (;;) {
        const ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
        if (n >= 0) {
            return static_cast<long>(n);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
#endif
}

bool PipeSink::drainResidual() noexcept {
    while (residualHead_ < residual_.size()) {
        const long n = rawWrite(residual_.data() + residualHead_,
                                residual_.size() - residualHead_);
        if (n < 0) {
            close();
            return false;
        }
        if (n == 0) {
            return false; // 아직 못 민다
        }
        residualHead_ += static_cast<usize>(n);
    }
    residual_.clear();
    residualHead_ = 0;
    return true;
}

SinkStatus PipeSink::write(const u8* data, usize len) noexcept {
    if (!connected()) {
        return SinkStatus::Disconnected;
    }
    if (!drainResidual()) {
        return connected() ? SinkStatus::WouldBlock : SinkStatus::Disconnected;
    }
    usize done = 0;
    while (done < len) {
        const long n = rawWrite(data + done, len - done);
        if (n < 0) {
            close();
            return SinkStatus::Disconnected;
        }
        if (n == 0) {
            break;
        }
        done += static_cast<usize>(n);
    }
    if (done == len) {
        return SinkStatus::Ok;
    }
    // 부분 쓰기: 와이어에 반토막 프레임을 남기지 않는다. 나머지는 잔여 버퍼로.
    const usize rest = len - done;
    if (rest > residualLimit_) {
        close();
        return SinkStatus::Disconnected;
    }
    try {
        residual_.assign(data + done, data + len);
    } catch (...) {
        close();
        return SinkStatus::Disconnected;
    }
    residualHead_ = 0;
    // 프레임 자체는 전부 책임졌다(일부는 잔여 버퍼에). 호출자 입장에서는 Ok 다.
    return SinkStatus::Ok;
}

SinkStatus PipeSink::pump() noexcept {
    if (!connected()) {
        return SinkStatus::Disconnected;
    }
    if (!drainResidual()) {
        return connected() ? SinkStatus::WouldBlock : SinkStatus::Disconnected;
    }
    return SinkStatus::Ok;
}

Result<Handshake> PipeSink::handshake(const Handshake& local) {
    if (!connected()) {
        return Err("연결되지 않은 채널에 핸드셰이크할 수 없다", ErrorCode::IoError);
    }
    const std::string line = encodeHandshake(local);
#ifdef _WIN32
    DWORD written = 0;
    if (::WriteFile(static_cast<HANDLE>(handle_), line.data(),
                    static_cast<DWORD>(line.size()), &written, nullptr) == 0) {
        return Err("핸드셰이크를 보내지 못했다", ErrorCode::IoError);
    }
    std::string reply;
    char buf[512];
    for (int tries = 0; tries < 200 && reply.find('\n') == std::string::npos; ++tries) {
        DWORD got = 0;
        if (::ReadFile(static_cast<HANDLE>(handle_), buf, sizeof(buf), &got, nullptr) != 0 &&
            got > 0) {
            reply.append(buf, got);
        } else {
            ::Sleep(10);
        }
    }
#else
    usize sent = 0;
    while (sent < line.size()) {
        const long n = rawWrite(reinterpret_cast<const u8*>(line.data()) + sent,
                                line.size() - sent);
        if (n < 0) {
            close();
            return Err("핸드셰이크를 보내지 못했다", ErrorCode::IoError);
        }
        if (n == 0) {
            pollfd pfd{fd_, POLLOUT, 0};
            if (::poll(&pfd, 1, kHandshakeTimeoutMs) <= 0) {
                return Err("핸드셰이크 전송이 시간 초과됐다", ErrorCode::IoError);
            }
            continue;
        }
        sent += static_cast<usize>(n);
    }
    std::string reply;
    char buf[512];
    while (reply.find('\n') == std::string::npos) {
        pollfd pfd{fd_, POLLIN, 0};
        if (::poll(&pfd, 1, kHandshakeTimeoutMs) <= 0) {
            break;
        }
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        reply.append(buf, static_cast<usize>(n));
    }
#endif
    if (reply.empty()) {
        // 답이 없어도 연결은 끊지 않는다 — 상대를 "능력 미상"으로 보고 최소로 간다.
        return Err("상대가 핸드셰이크에 답하지 않았다", ErrorCode::Unsupported);
    }
    return decodeHandshake(reply.substr(0, reply.find('\n')));
}

} // namespace mari::sigan
