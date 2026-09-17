// docs/03 4절 ② / 5.2 — 파이프(POSIX 는 유닉스 소켓) 싱크.
// 🔴 커널 버퍼가 차도 **막히지 않는다.** WouldBlock 을 돌려줄 뿐이다.
#include <mari/sigan/pipe_sink.hpp>
#include <mari/test/harness.hpp>

#include <filesystem>
#include <string>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace mari;
using namespace mari::sigan;

MARI_TEST(default_pipe_path_is_sigan_native) {
    const std::string path = defaultPipePath();
    CHECK(path.find("sigan-native") != std::string::npos);
}

MARI_TEST(connect_fails_quietly_when_sigan_is_absent) {
    // Sigan 미설치가 정상 상태다(docs/03 5.1) — 조용히 로컬 모드로 간다.
    PipeSink sink("/tmp/mari-sigan-does-not-exist.sock");
    auto r = sink.connect();
    CHECK(!r.ok());
    CHECK(!sink.connected());
    CHECK_EQ(sink.write(nullptr, 0) == SinkStatus::Disconnected, true);
}

#ifndef _WIN32
MARI_TEST(pipe_never_blocks_the_drawing_thread) {
    const std::string path = (std::filesystem::temp_directory_path() / "mari_pipe_test.sock")
                                 .string();
    std::filesystem::remove(path);

    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(server >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    CHECK_EQ(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    CHECK_EQ(::listen(server, 1), 0);

    PipeSink sink(path);
    CHECK(sink.connect().ok());
    CHECK(sink.connected());
    const int conn = ::accept(server, nullptr, nullptr);
    CHECK(conn >= 0);

    StrokeFrame f;
    f.pressure = 1.0f;
    u8 wire[kFrameSize];

    // 수신자가 한 바이트도 읽지 않는 상태로 계속 밀어넣는다.
    // 어느 시점에 WouldBlock 이 나와야 한다 — **영원히 멈추면 안 된다.**
    bool sawWouldBlock = false;
    u64 accepted = 0;
    for (u64 seq = 1; seq <= 200000 && !sawWouldBlock; ++seq) {
        f.seq = seq;
        encodeFrame(f, wire);
        const SinkStatus st = sink.write(wire, kFrameSize);
        if (st == SinkStatus::WouldBlock) {
            sawWouldBlock = true;
        } else if (st == SinkStatus::Disconnected) {
            CHECK_FAIL("살아 있는 소켓에서 끊김이 보고됐다");
            break;
        } else {
            ++accepted;
        }
    }
    CHECK(sawWouldBlock);
    CHECK(accepted > 0);

    // 수신자가 읽어가면 다시 흐른다.
    u8 drain[65536];
    ssize_t got = 0;
    do {
        got = ::recv(conn, drain, sizeof(drain), MSG_DONTWAIT);
    } while (got > 0);
    CHECK_EQ(sink.pump() == SinkStatus::Ok, true);
    f.seq = 999999;
    encodeFrame(f, wire);
    CHECK_EQ(sink.write(wire, kFrameSize) == SinkStatus::Ok, true);

    // 상대가 죽으면 끊김으로 보고한다(그리고 저널로 스풀된다).
    ::close(conn);
    for (int i = 0; i < 100000; ++i) {
        if (sink.write(wire, kFrameSize) == SinkStatus::Disconnected) {
            break;
        }
    }
    CHECK(!sink.connected());

    ::close(server);
    std::filesystem::remove(path);
}
#endif

MARI_TEST_MAIN()
