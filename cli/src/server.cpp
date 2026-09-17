// Mari Paint — 줄 단위 JSON-RPC 서버 구현
#include <mari/cli/server.hpp>

#include <mari/agent/json.hpp>

#include <ostream>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cerrno>
#include <cstring>

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
        << " (한 줄 = 요청 하나. {\"op\":\"quit\"} 으로 멈춘다)\n";
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
        std::string buf;
        char chunk[4096];
        bool peerGone = false;
        while (!peerGone && !quit) {
            const ssize_t got = ::read(fd, chunk, sizeof(chunk));
            if (got <= 0) {
                break;
            }
            buf.append(chunk, static_cast<usize>(got));
            for (;;) {
                const usize nl = buf.find('\n');
                if (nl == std::string::npos) {
                    break;
                }
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.find_first_not_of(" \t\r") == std::string::npos) {
                    continue;
                }
                std::string reply = handleRpcLine(session, line, &quit);
                reply.push_back('\n');
                usize sent = 0;
                while (sent < reply.size()) {
                    const ssize_t w = ::write(fd, reply.data() + sent, reply.size() - sent);
                    if (w <= 0) {
                        peerGone = true;
                        break;
                    }
                    sent += static_cast<usize>(w);
                }
                if (peerGone || quit) {
                    break;
                }
            }
        }
        closeFd(fd);
    }
    closeFd(listenFd);
    return Ok();
}

#endif

} // namespace mari::cli
