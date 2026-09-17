// `--serve` — 줄 단위 프로토콜과 실제 루프백 TCP 왕복.
//
// 🔴 여기서 증명하려는 것: **전송 규약이 능력을 바꾸지 않는다**(docs/05 1절).
//    평문이든 JSON-RPC 든 같은 AgentSession 을 지나고 같은 결과가 나온다.
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

Json parseOk(const std::string& text, mari::test::Context& ctx) {
    const auto r = Json::parse(text);
    if (!r.ok()) {
        ctx.fail(__FILE__, __LINE__, "응답이 JSON 이 아니다: " + text);
        return Json::null();
    }
    return r.value();
}

} // namespace

// ── 주소 해석 ────────────────────────────────────────────────────────────

MARI_TEST(serve_address_defaults_to_loopback) {
    const auto a = cli::parseServeAddress(":7777");
    CHECK(a.ok());
    // 🔴 호스트를 생략하면 루프백이다. 실수로 0.0.0.0 에 열리면 인증 없는 서버가 밖으로 난다.
    CHECK_EQ(a.value().host, std::string("127.0.0.1"));
    CHECK_EQ(static_cast<int>(a.value().port), 7777);

    const auto b = cli::parseServeAddress("127.0.0.1:1234");
    CHECK(b.ok());
    CHECK_EQ(static_cast<int>(b.value().port), 1234);

    const auto c = cli::parseServeAddress("8080");
    CHECK(c.ok());
    CHECK_EQ(c.value().host, std::string("127.0.0.1"));
    CHECK_EQ(static_cast<int>(c.value().port), 8080);

    CHECK(!cli::parseServeAddress("").ok());
    CHECK(!cli::parseServeAddress(":").ok());
    CHECK(!cli::parseServeAddress(":nope").ok());
    CHECK(!cli::parseServeAddress(":70000").ok());
}

// ── 줄 프로토콜 ──────────────────────────────────────────────────────────

MARI_TEST(plain_and_jsonrpc_forms_reach_the_same_session) {
    auto session = agent::AgentSession::open("rpc-test");
    CHECK(session.ok());
    agent::AgentSession& s = *session.value();

    const Json plain = parseOk(cli::handleRpcLine(s, R"({"op":"doc.create","width":32,"height":32})"),
                               mari_ctx);
    CHECK(plain["ok"].asBool());
    CHECK_EQ(plain["op"].asString(), std::string("doc.create"));

    const Json rpc = parseOk(
        cli::handleRpcLine(
            s, R"({"jsonrpc":"2.0","id":7,"method":"doc.describe","params":{"view":"none"}})"),
        mari_ctx);
    CHECK_EQ(rpc["jsonrpc"].asString(), std::string("2.0"));
    CHECK_EQ(rpc["id"].asInt(), 7);
    // 같은 문서를 본다 — 두 전송이 같은 세션을 지났다는 증거다.
    CHECK(rpc["result"]["ok"].asBool());
    CHECK_EQ(rpc["result"]["result"]["canvas"]["width"].asInt(32), 32);
}

MARI_TEST(broken_input_answers_with_one_error_line) {
    auto session = agent::AgentSession::open("rpc-test");
    CHECK(session.ok());
    agent::AgentSession& s = *session.value();

    // 🔴 서버는 던지지 않는다. 깨진 입력도 한 줄짜리 응답이다.
    for (const char* bad : {"{oops", "[]", "\"just a string\"", "42"}) {
        const std::string reply = cli::handleRpcLine(s, bad);
        CHECK(reply.find('\n') == std::string::npos); // 한 줄
        const Json j = parseOk(reply, mari_ctx);
        CHECK(j.isObject());
        CHECK(!j["ok"].asBool(true));
    }

    // 모르는 연산은 JSON-RPC 쪽에서 표준 코드로 접힌다.
    const Json rpc = parseOk(
        cli::handleRpcLine(s, R"({"jsonrpc":"2.0","id":1,"method":"no.such.op"})"), mari_ctx);
    CHECK(rpc.has("error"));
    CHECK_EQ(rpc["error"]["code"].asInt(), -32601);
}

MARI_TEST(quit_is_a_transport_command_not_a_capability) {
    // 🔴 능력 계층에 `quit` 이라는 연산은 없다. 서버가 자기 선에서 끝낸다 —
    //    전송 규약이 능력 표를 늘리지 않는다(docs/05 1절).
    auto session = agent::AgentSession::open("rpc-test");
    CHECK(session.ok());
    CHECK(agent::findOp("quit") == nullptr);

    bool quit = false;
    const Json j = parseOk(cli::handleRpcLine(*session.value(), R"({"op":"quit"})", &quit), mari_ctx);
    CHECK(quit);
    CHECK(j["ok"].asBool());
    CHECK(j["result"]["quit"].asBool());

    bool quit2 = false;
    const Json r = parseOk(
        cli::handleRpcLine(*session.value(), R"({"jsonrpc":"2.0","id":3,"method":"quit"})", &quit2),
        mari_ctx);
    CHECK(quit2);
    CHECK_EQ(r["id"].asInt(), 3);
}

MARI_TEST(rpc_replies_never_contain_a_raw_newline) {
    // 프레이밍이 "한 줄 = 응답 하나"라서, 응답 안에 날 개행이 들어가면 규약이 깨진다.
    auto session = agent::AgentSession::open("rpc-test");
    CHECK(session.ok());
    agent::AgentSession& s = *session.value();
    (void)cli::handleRpcLine(s, R"({"op":"doc.create","width":16,"height":16})");
    const std::string reply = cli::handleRpcLine(s, R"({"op":"capabilities","view":"none"})");
    CHECK(reply.find('\n') == std::string::npos);
    CHECK(reply.find('\r') == std::string::npos);
}

// ── 진짜 소켓 왕복 ───────────────────────────────────────────────────────

#if !defined(_WIN32)

MARI_TEST(tcp_round_trip_on_loopback) {
    auto session = agent::AgentSession::open("tcp-test");
    CHECK(session.ok());

    cli::ServeAddress addr;
    addr.host = "127.0.0.1";
    addr.port = 0; // 커널이 고른다 — 고정 포트를 잡으면 CI 에서 충돌한다

    std::atomic<u16> boundPort{0};
    std::atomic<bool> ready{false};
    std::ostringstream serverErr;
    Result<void> serveResult = Ok();
    std::thread server([&] {
        serveResult = cli::serveTcp(*session.value(), addr, serverErr, [&](u16 p) {
            boundPort.store(p);
            ready.store(true);
        });
    });

    for (int spin = 0; spin < 2000 && !ready.load(); ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(ready.load());
    CHECK(boundPort.load() != 0);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(boundPort.load());
    CHECK(::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) == 1);
    CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0);

    const auto send = [&](const std::string& line) {
        const std::string out = line + "\n";
        CHECK(::write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size()));
    };
    std::string inbox;
    const auto readLine = [&]() {
        for (;;) {
            const usize nl = inbox.find('\n');
            if (nl != std::string::npos) {
                const std::string line = inbox.substr(0, nl);
                inbox.erase(0, nl + 1);
                return line;
            }
            char buf[4096];
            const ssize_t got = ::read(fd, buf, sizeof(buf));
            if (got <= 0) {
                return std::string();
            }
            inbox.append(buf, static_cast<usize>(got));
        }
    };

    send(R"({"op":"doc.create","width":48,"height":48})");
    const Json created = parseOk(readLine(), mari_ctx);
    CHECK(created["ok"].asBool());

    // 🔴 같은 연결로 그린 획도 agent 다. 전송 규약이 출처를 바꾸지 않는다.
    send(R"({"op":"stroke","points":[[4,4],[40,40]],"size":8,"color":"#112233","view":"none"})");
    const Json stroked = parseOk(readLine(), mari_ctx);
    CHECK(stroked["ok"].asBool());

    send(R"({"jsonrpc":"2.0","id":9,"method":"doc.describe","params":{"view":"none"}})");
    const Json described = parseOk(readLine(), mari_ctx);
    CHECK_EQ(described["id"].asInt(), 9);
    CHECK(described["result"]["ok"].asBool());

    send(R"({"op":"quit"})");
    const Json bye = parseOk(readLine(), mari_ctx);
    CHECK(bye["result"]["quit"].asBool());

    ::close(fd);
    server.join();
    CHECK(serveResult.ok());
    // 배너조차 stderr 로 간다(서버는 stdout 을 건드리지 않는다).
    CHECK(serverErr.str().find("listening 127.0.0.1:") != std::string::npos);

    CHECK_EQ(session.value()->originStats().agent(), 1u);
    CHECK_EQ(session.value()->originStats().human(), 0u);
}

#endif

MARI_TEST_MAIN()
