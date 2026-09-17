// Mari Paint — 줄 단위 JSON-RPC 서버 구현
#include <mari/cli/server.hpp>

#include <mari/agent/json.hpp>

#include <ostream>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace mari::cli {
namespace {

using agent::Json;

/// 응답 봉투의 오류 코드를 JSON-RPC 2.0 코드로 접는다.
/// 🔴 이름은 agent-api 가 내는 문자열 그대로다(`mari::agent` 의 errorCodeName).
///    여기서 따로 정의하지 않는다 — 두 벌이 되면 언젠가 어긋난다.
i64 rpcCodeOf(const std::string& code) {
    if (code == "InvalidArgument") {
        return -32602; // Invalid params
    }
    if (code == "Unsupported" || code == "NotFound") {
        return -32601; // Method not found / 대상 없음
    }
    if (code == "ParseError") {
        return -32700;
    }
    return -32603; // Internal error
}

std::string rpcError(const Json& id, i64 code, const std::string& message) {
    Json root = Json::object();
    root.set("jsonrpc", Json::string("2.0"));
    root.set("id", id);
    Json e = Json::object();
    e.set("code", Json::integer(code));
    e.set("message", Json::string(message));
    root.set("error", std::move(e));
    return root.dump();
}

std::string plainError(const std::string& message) {
    Json root = Json::object();
    root.set("ok", Json::boolean(false));
    Json e = Json::object();
    e.set("code", Json::string("ParseError"));
    e.set("message", Json::string(message));
    root.set("error", std::move(e));
    return root.dump();
}

} // namespace

std::string pushLine(const std::string& kind, const Json& data, u64 dropped) {
    Json root = Json::object();
    // 🔴 이 키 하나가 "요청하지 않았는데 온 줄"이라는 표시다. 응답에는 절대 붙지 않는다.
    root.set("push", Json::boolean(true));
    Json e = Json::object();
    e.set("kind", Json::string(kind));
    e.set("data", data);
    root.set("event", std::move(e));
    if (dropped != 0) {
        // 🔴 놓친 건수를 숨기지 않는다. 이벤트 안의 seq 와 함께 보면
        //    클라이언트가 자기가 뭘 못 봤는지 정확히 안다(docs/07 4절).
        root.set("dropped", Json::integer(static_cast<i64>(dropped)));
    }
    return root.dump();
}

Result<ServeAddress> parseServeAddress(std::string_view spec) {
    ServeAddress a;
    if (spec.empty()) {
        return Err("--serve 주소가 비었다 (예: :7777)", ErrorCode::InvalidArgument);
    }
    std::string_view portPart = spec;
    const usize colon = spec.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view host = spec.substr(0, colon);
        portPart = spec.substr(colon + 1);
        if (!host.empty()) {
            a.host = std::string(host);
        }
    }
    if (portPart.empty()) {
        return Err("--serve 에 포트가 없다 (예: :7777)", ErrorCode::InvalidArgument);
    }
    i64 port = 0;
    for (const char c : portPart) {
        if (c < '0' || c > '9') {
            return Err("--serve 포트가 숫자가 아니다: " + std::string(portPart),
                       ErrorCode::InvalidArgument);
        }
        port = port * 10 + (c - '0');
        if (port > 65535) {
            return Err("--serve 포트 범위를 넘었다(0..65535)", ErrorCode::InvalidArgument);
        }
    }
    a.port = static_cast<u16>(port);
    return Ok(a);
}

std::string handleRpcLine(agent::AgentSession& session, std::string_view line, bool* quit) {
    // 끝의 CR/LF 를 떼어 준다(telnet·윈도우 클라이언트 대비).
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    Result<Json> parsed = Json::parse(line);
    if (!parsed.ok()) {
        return plainError("JSON 을 읽을 수 없다: " + parsed.message());
    }
    const Json& req = parsed.value();
    if (!req.isObject()) {
        return plainError("요청은 JSON 객체여야 한다");
    }

    const bool jsonRpc = req.has("jsonrpc") || req.has("method");
    const std::string method = jsonRpc ? req["method"].asString() : req["op"].asString();

    // 🔴 quit 은 전송 계층의 명령이다. 능력 계층에 그런 연산은 없다.
    if (method == "quit") {
        if (quit != nullptr) {
            *quit = true;
        }
        if (jsonRpc) {
            Json root = Json::object();
            root.set("jsonrpc", Json::string("2.0"));
            root.set("id", req["id"]);
            Json r = Json::object();
            r.set("quit", Json::boolean(true));
            root.set("result", std::move(r));
            return root.dump();
        }
        Json root = Json::object();
        root.set("ok", Json::boolean(true));
        root.set("op", Json::string("quit"));
        Json r = Json::object();
        r.set("quit", Json::boolean(true));
        root.set("result", std::move(r));
        return root.dump();
    }

    if (!jsonRpc) {
        // 평문 형태: 요청이 곧 연산이다. 봉투는 세션이 씌워서 돌려준다.
        return session.execute(req).dump();
    }

    const Json id = req["id"];
    if (method.empty()) {
        return rpcError(id, -32600, "\"method\" 가 없다");
    }
    // params 를 연산 객체로 접는다. 세션은 `{"op": ...}` 하나만 안다 —
    // 전송 규약이 늘어도 능력 계층은 그대로다(docs/05 1절).
    Json op = req["params"].isObject() ? req["params"] : Json::object();
    op.set("op", Json::string(method));

    Json env = session.execute(op);
    if (!env["ok"].asBool(false)) {
        return rpcError(id, rpcCodeOf(env["error"]["code"].asString()),
                        env["error"]["message"].asString());
    }
    Json root = Json::object();
    root.set("jsonrpc", Json::string("2.0"));
    root.set("id", id);
    // 봉투에서 본문만 꺼내지 않는다 — 이미지·dirtyRect 도 에이전트에게 필요한 값이다.
    root.set("result", env);
    return root.dump();
}

#if defined(_WIN32)

Result<void> serveTcp(agent::AgentSession&, const ServeAddress&, std::ostream&,
                      const std::function<void(u16)>&) {
    // Windows 소켓 경로는 이 빌드에서 한 번도 컴파일된 적이 없다(docs/04 2절).
    // 있는 척하지 않는다.
    return Err("--serve 는 아직 POSIX 빌드에서만 돈다", ErrorCode::Unsupported);
}

#else

namespace {

/// 연결 하나의 푸시 대기열. **구독자 스레드가 넣고, 서브 루프가 꺼낸다.**
///
/// 🔴 `offer()` 는 절대 기다리지 않는다. 기다리면 구독자 스레드가 소켓에 묶이고,
///    그 스레드가 묶이면 버스의 역압 정책(요약)이 무의미해진다.
/// 🔴 정책은 관찰 경로 것 하나뿐이다 — **이 연결만 요약하고 센다.**
///    Sigan 기록은 이 파일을 지나가지도 않는다(app/events.hpp ① 기록 경로).
class PushOutbox {
public:
    explicit PushOutbox(int wakeFd) noexcept : wake_(wakeFd) {}

    void offer(std::string line) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (q_.size() >= kCap) {
                q_.pop_front();
                ++dropped_; // 조용히 넘어가지 않는다. 다음 푸시 줄이 이 숫자를 싣는다
            }
            q_.push_back(std::move(line));
        }
        // 서브 루프를 깨운다. 파이프가 꽉 차 있어도 상관없다 —
        // 이미 깨울 바이트가 들어 있다는 뜻이라 어차피 깨어난다.
        const u8 one = 1;
        (void)!::write(wake_, &one, 1);
    }

    /// 쌓인 줄을 통째로 가져간다. 놓친 건수도 같이 꺼내 0 으로 되돌린다.
    std::deque<std::string> take(u64& droppedOut) {
        std::lock_guard<std::mutex> g(m_);
        droppedOut = dropped_;
        dropped_ = 0;
        std::deque<std::string> out;
        out.swap(q_);
        return out;
    }

private:
    static constexpr usize kCap = 1024;

    std::mutex m_;
    std::deque<std::string> q_;
    u64 dropped_ = 0;
    int wake_ = -1;
};

bool setNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// 연결 하나를 끝까지 본다. **읽기와 쓰기를 같이 기다린다**(poll) —
/// 그래야 요청이 없는 동안에도 서버가 먼저 줄을 밀어낼 수 있다(docs/07 3절).
void serveConnection(agent::AgentSession& session, int fd, bool& quit, std::ostream& err) {
    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) {
        err << "serve: 깨움 파이프를 만들 수 없다 — 이 연결은 푸시 없이 돈다\n";
    }
    (void)setNonBlocking(fd);
    if (wake[0] >= 0) {
        (void)setNonBlocking(wake[0]);
        (void)setNonBlocking(wake[1]);
    }

    PushOutbox outbox(wake[1]);
    // 🔴 `events.subscribe` 전에는 밀지 않는다. 구독은 능력 계층의 상태이고,
    //    그것을 읽는 것은 서브 루프 스레드다 — 구독자 스레드가 세션을 읽으면 경주가 된다.
    //    그래서 원자 플래그 한 칸으로 넘긴다.
    std::atomic<bool> pushOn{false};
    u64 subId = 0;
    if (wake[0] >= 0) {
        subId = session.subscribePush({}, [&outbox, &pushOn](const std::string& kind,
                                                             const Json& data) {
            if (!pushOn.load(std::memory_order_relaxed)) {
                return;
            }
            outbox.offer(pushLine(kind, data));
        });
    }

    std::string inbuf;
    std::string outbuf; ///< 아직 못 보낸 바이트(응답 + 푸시가 섞인다)
    u64 pendingDropped = 0;
    bool peerGone = false;
    // 상대가 안 읽고 있는데 요청만 계속 밀어 넣으면 outbuf 가 무한히 자란다.
    // 그 선을 넘으면 **읽기를 쉰다** — 요청을 안 받으면 응답도 안 늘어난다.
    constexpr usize kMaxPending = 4u * 1024u * 1024u;

    while (!(peerGone || (quit && outbuf.empty()))) {
        pollfd fds[2];
        fds[0].fd = fd;
        fds[0].events = 0;
        if (!quit && outbuf.size() < kMaxPending) {
            fds[0].events |= POLLIN;
        }
        if (!outbuf.empty()) {
            fds[0].events |= POLLOUT;
        }
        fds[0].revents = 0;
        fds[1].fd = wake[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        const nfds_t count = wake[0] >= 0 ? 2 : 1;

        if (::poll(fds, count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        // ① 밀린 푸시를 꺼내 나갈 줄에 붙인다.
        if (count == 2 && (fds[1].revents & POLLIN) != 0) {
            char sink[256];
            while (::read(wake[0], sink, sizeof(sink)) > 0) {
                // 깨움 바이트는 세지 않는다. 몇 번 깨웠는지는 정보가 아니다
            }
            u64 dropped = 0;
            std::deque<std::string> lines = outbox.take(dropped);
            pendingDropped += dropped;
            for (std::string& line : lines) {
                if (pendingDropped != 0) {
                    // 놓친 게 있으면 **다음에 나가는 줄에 그 숫자를 달아 보낸다.**
                    // 줄을 다시 만드는 게 아니라 이미 만든 줄에 키를 얹는다.
                    Result<Json> parsed = Json::parse(line);
                    if (parsed.ok() && parsed.value().isObject()) {
                        Json j = parsed.value();
                        j.set("dropped", Json::integer(static_cast<i64>(pendingDropped)));
                        line = j.dump();
                        pendingDropped = 0;
                    }
                }
                outbuf += line;
                outbuf.push_back('\n');
            }
        }

        // ② 요청을 읽어 응답을 만든다.
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char chunk[4096];
            for (;;) {
                const ssize_t got = ::read(fd, chunk, sizeof(chunk));
                if (got > 0) {
                    inbuf.append(chunk, static_cast<usize>(got));
                    continue;
                }
                if (got == 0) {
                    peerGone = true; // EOF
                }
                if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break; // 지금 읽을 게 없다
                }
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                if (got < 0) {
                    peerGone = true;
                }
                break;
            }
            for (;;) {
                const usize nl = inbuf.find('\n');
                if (nl == std::string::npos) {
                    break;
                }
                const std::string line = inbuf.substr(0, nl);
                inbuf.erase(0, nl + 1);
                if (line.find_first_not_of(" \t\r") == std::string::npos) {
                    continue;
                }
                outbuf += handleRpcLine(session, line, &quit);
                outbuf.push_back('\n');
                // 🔴 구독 상태는 **요청을 처리한 이 스레드가** 읽어서 넘긴다.
                pushOn.store(session.subscribed(), std::memory_order_relaxed);
                if (quit) {
                    break;
                }
            }
        }

        // ③ 나갈 바이트를 보낸다. 상대가 안 읽으면 남겨 두고 다음 poll 을 기다린다.
        while (!outbuf.empty()) {
            const ssize_t w = ::write(fd, outbuf.data(), outbuf.size());
            if (w > 0) {
                outbuf.erase(0, static_cast<usize>(w));
                continue;
            }
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            peerGone = true;
            break;
        }
    }

    // 🔴 구독자 스레드를 **먼저** 합류시킨다. outbox 와 pushOn 이 이 스코프의 값이므로,
    //    떼기 전에 돌아가면 죽은 객체를 건드리게 된다.
    if (subId != 0) {
        (void)session.unsubscribePush(subId);
    }
    if (wake[0] >= 0) {
        ::close(wake[0]);
        ::close(wake[1]);
    }
}

} // namespace

Result<void> serveTcp(agent::AgentSession& session, const ServeAddress& addr, std::ostream& err,
                      const std::function<void(u16)>& onReady) {
    const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        return Err(std::string("소켓을 만들 수 없다: ") + std::strerror(errno), ErrorCode::IoError);
    }
    const auto closeFd = [](int fd) {
        if (fd >= 0) {
            ::close(fd);
        }
    };

    int yes = 1;
    (void)::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr.port);
    if (::inet_pton(AF_INET, addr.host.c_str(), &sa.sin_addr) != 1) {
        closeFd(listenFd);
        return Err("주소를 해석할 수 없다: " + addr.host + " (IPv4 리터럴만 받는다)",
                   ErrorCode::InvalidArgument);
    }
    if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        const std::string why = std::strerror(errno);
        closeFd(listenFd);
        return Err("바인딩 실패: " + addr.host + ":" + std::to_string(addr.port) + " (" + why + ")",
                   ErrorCode::IoError);
    }
    if (::listen(listenFd, 4) != 0) {
        const std::string why = std::strerror(errno);
        closeFd(listenFd);
        return Err(std::string("listen 실패: ") + why, ErrorCode::IoError);
    }

    // 실제로 바인딩된 포트를 알려준다(port 0 이면 커널이 골랐다).
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    u16 boundPort = addr.port;
    if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        boundPort = ntohs(bound.sin_port);
    }
    // 🔴 배너도 stderr 로 간다. stdout 은 끝까지 순수 JSON 이다.
    err << "listening " << addr.host << ":" << boundPort
        << " (한 줄 = 요청 하나. events.subscribe 하면 push:true 줄이 밀려온다. "
           "{\"op\":\"quit\"} 으로 멈춘다)\n";
    if (onReady) {
        onReady(boundPort);
    }

    bool quit = false;
    while (!quit) {
        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string why = std::strerror(errno);
            closeFd(listenFd);
            return Err("accept 실패: " + why, ErrorCode::IoError);
        }
        serveConnection(session, fd, quit, err);
        closeFd(fd);
    }
    closeFd(listenFd);
    return Ok();
}

#endif

} // namespace mari::cli
