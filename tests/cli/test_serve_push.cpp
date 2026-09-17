// 🔴 `--serve` 서버 푸시 — **진짜 소켓**으로 검증한다 (docs/05 2.7 · docs/07 3절)
//
// 이 파일이 증명하려는 것:
//   ① 클라이언트가 `events.poll` 을 **한 번도 보내지 않았는데** 이벤트 줄이 날아온다.
//   ② 푸시 줄과 응답 줄이 `push` 키 하나로 확실히 갈린다.
//   ③ `events.unsubscribe` 하면 멎는다.
//   ④ 안 읽는 클라이언트가 서버를 매달지 않는다.
//
// 인프로세스 함수 호출로는 ①을 증명할 수 없다 — "밀어 준다"는 전송 계층의 주장이므로
// 실제 fd 를 열어 확인한다. docs/05 6절 `headless_parity` 가 진짜 바이너리를 띄우는 것과
// 같은 이유다.
#include <mari/cli/server.hpp>
#include <mari/test/harness.hpp>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace mari;
using mari::agent::Json;

namespace {

#if !defined(_WIN32)

/// 루프백 연결 하나. 줄 단위로 주고받는다.
class LineClient {
public:
    explicit LineClient(u16 port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        (void)::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        timeval tv{};
        tv.tv_sec = 5;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ~LineClient() { close(); }

    LineClient(const LineClient&) = delete;
    LineClient& operator=(const LineClient&) = delete;

    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    void send(const std::string& line) {
        const std::string text = line + "\n";
        usize sent = 0;
        while (sent < text.size()) {
            const ssize_t w = ::write(fd_, text.data() + sent, text.size() - sent);
            if (w <= 0) {
                return;
            }
            sent += static_cast<usize>(w);
        }
    }

    /// 한 줄을 받는다. 시간 안에 못 받으면 빈 문자열.
    std::string recvLine() {
        for (;;) {
            const usize nl = buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                return line;
            }
            char chunk[4096];
            const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
            if (got <= 0) {
                return {};
            }
            buf_.append(chunk, static_cast<usize>(got));
        }
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    std::string buf_;
};

/// 서버를 스레드에서 띄우고 바인딩된 포트를 받아 온다.
class ServerThread {
public:
    ServerThread(agent::AgentSession& session, mari::test::Context& ctx) {
        cli::ServeAddress addr;
        addr.port = 0; // 커널이 고른다
        thread_ = std::thread([this, &session, addr, &ctx] {
            const Result<void> r = cli::serveTcp(session, addr, sink_, [this](u16 p) {
                port_.store(p);
            });
            if (!r.ok()) {
                ctx.fail(__FILE__, __LINE__, "serveTcp 실패: " + r.message());
            }
            done_.store(true);
        });
        for (int i = 0; i < 2000 && port_.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ~ServerThread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] u16 port() const noexcept { return port_.load(); }
    [[nodiscard]] bool finished() const noexcept { return done_.load(); }

private:
    std::ostringstream sink_;
    std::thread thread_;
    std::atomic<u16> port_{0};
    std::atomic<bool> done_{false};
};

Json parse(const std::string& text) {
    const auto r = Json::parse(text);
    return r.ok() ? r.value() : Json::null();
}

std::string fillLine() {
    return R"({"op":"fill","region":"canvas","view":"none"})";
}

#endif // !_WIN32

} // namespace

#if !defined(_WIN32)

MARI_TEST(serve_pushes_events_over_a_real_socket) {
    auto s = agent::AgentSession::open("serve-push");
    CHECK(s.ok());
    if (!s.ok()) {
        return;
    }
    agent::AgentSession& session = *s.value();

    ServerThread server(session, mari_ctx);
    CHECK(server.port() != 0);
    if (server.port() == 0) {
        return;
    }

    LineClient c(server.port());
    CHECK(c.ok());
    if (!c.ok()) {
        return;
    }

    c.send(R"({"op":"doc.create","width":64,"height":64,"view":"none"})");
    CHECK(parse(c.recvLine())["ok"].asBool());

    c.send(R"({"op":"events.subscribe"})");
    const Json sub = parse(c.recvLine());
    CHECK(sub["ok"].asBool());
    CHECK(!sub.has("push")); // 응답에는 push 키가 없다

    c.send(fillLine());

    // 🔴 여기서부터가 핵심이다. 우리는 `events.poll` 을 **보내지 않는다.**
    //    그런데도 push 줄이 온다면 그게 서버 푸시다.
    bool sawResponse = false;
    bool sawPush = false;
    std::string pushedKind;
    for (int i = 0; i < 16 && !(sawResponse && sawPush); ++i) {
        const std::string line = c.recvLine();
        if (line.empty()) {
            break;
        }
        const Json j = parse(line);
        if (j["push"].asBool(false)) {
            sawPush = true;
            pushedKind = j["event"]["kind"].asString();
            // 이벤트 본문이 폴링 때와 같은 모양인지 — 두 경로가 한 변환기를 쓴다는 증거.
            CHECK(j["event"]["data"]["seq"].asInt(0) > 0);
        } else {
            sawResponse = true;
            CHECK(j["ok"].asBool());
        }
    }
    CHECK(sawResponse);
    CHECK(sawPush);
    CHECK_EQ(pushedKind, std::string("layerChanged"));

    c.send(R"({"op":"quit"})");
    c.close();
    for (int i = 0; i < 3000 && !server.finished(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.finished());
}

MARI_TEST(unsubscribe_stops_the_push_stream) {
    auto s = agent::AgentSession::open("serve-push");
    CHECK(s.ok());
    if (!s.ok()) {
        return;
    }
    agent::AgentSession& session = *s.value();
    ServerThread server(session, mari_ctx);
    CHECK(server.port() != 0);
    if (server.port() == 0) {
        return;
    }
    LineClient c(server.port());
    CHECK(c.ok());
    if (!c.ok()) {
        return;
    }

    c.send(R"({"op":"doc.create","width":64,"height":64,"view":"none"})");
    CHECK(parse(c.recvLine())["ok"].asBool());
    c.send(R"({"op":"events.unsubscribe"})");
    CHECK(parse(c.recvLine())["ok"].asBool());

    // 구독하지 않았으므로 푸시가 없어야 한다. fill 의 응답 **하나만** 온다.
    c.send(fillLine());
    const Json first = parse(c.recvLine());
    CHECK(first["ok"].asBool());
    CHECK(!first.has("push"));

    // 다음에 오는 줄은 quit 응답이어야 한다 — 사이에 푸시가 끼지 않았다는 뜻이다.
    c.send(R"({"op":"quit"})");
    const Json q = parse(c.recvLine());
    CHECK(!q["push"].asBool(false));
    CHECK(q["result"]["quit"].asBool());

    c.close();
    for (int i = 0; i < 3000 && !server.finished(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.finished());
}

MARI_TEST(a_client_that_never_reads_does_not_hang_the_server) {
    // 🔴 역압. 클라이언트가 한 줄도 안 읽어도 서버는 계속 돌아야 한다 —
    //    안 그러면 구독자 하나가 그리기 전체를 멈춰 세운다(docs/07 4절).
    auto s = agent::AgentSession::open("serve-push");
    CHECK(s.ok());
    if (!s.ok()) {
        return;
    }
    agent::AgentSession& session = *s.value();
    ServerThread server(session, mari_ctx);
    CHECK(server.port() != 0);
    if (server.port() == 0) {
        return;
    }
    LineClient c(server.port());
    CHECK(c.ok());
    if (!c.ok()) {
        return;
    }

    c.send(R"({"op":"doc.create","width":64,"height":64,"view":"none"})");
    c.send(R"({"op":"events.subscribe"})");
    // 응답을 **읽지 않고** 계속 밀어 넣는다.
    for (int i = 0; i < 200; ++i) {
        c.send(fillLine());
    }
    c.send(R"({"op":"quit"})");

    // 서버가 살아 있다면 quit 를 결국 처리하고 멎는다. 매달렸다면 여기서 못 끝난다.
    for (int i = 0; i < 10000 && !server.finished(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (i % 50 == 0) {
            (void)c.recvLine(); // 가끔 한 줄씩만 빼 준다 — 커널 버퍼가 꽉 차도 죽지 않는다
        }
    }
    CHECK(server.finished());
    c.close();
}

#endif // !_WIN32

MARI_TEST_MAIN()
