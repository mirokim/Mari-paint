// 🔴 `single_publish_path` — 발행 지점이 **하나**임을 CI 로 못박는다 (docs/06 결정 ③)
//
// 왜 이런 테스트가 필요한가:
//   사람 획(WM_POINTER)이 붙는 날, 급한 사람은 반드시 지름길을 뚫으려 한다.
//   "여기서 바로 publish() 하면 되잖아" — 그 한 줄이 기록을 두 갈래로 가른다.
//   갈라진 기록은 어느 쪽도 캔버스 전체를 설명하지 못하고,
//   설명하지 못하는 인증서는 거짓이다(docs/06 6절 H1).
//   그래서 **그날 빌드가 빨개지게** 만들어 둔다.
//
// 수법은 `no_origin_override` 와 같다 — 소스를 텍스트로 훑는다.
// 찾는 것은 멤버 호출 문법(`->publish(` · `.publish(`)이다.
// `SiganPublisher::publish()` 는 정적 함수가 아니므로 부르려면 반드시 이 모양이 된다.
#include <mari/core/types.hpp>
#include <mari/test/harness.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace mari;

namespace {

/// 저장소 루트를 __FILE__ 에서 되짚는다(.../tests/record/이_파일 → 3단계 위).
std::filesystem::path repoRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string readFile(const std::filesystem::path& p) {
    std::FILE* fp = std::fopen(p.string().c_str(), "rb");
    if (fp == nullptr) {
        return {};
    }
    std::string out;
    char buf[8192];
    usize n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(fp);
    return out;
}

/// 경로를 '/' 구분자의 상대 경로 문자열로.
std::string relSlash(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::string s = std::filesystem::relative(p, root).generic_string();
    return s;
}

/// 🔴 허용 목록. 여기를 늘리는 것이 곧 **두 번째 발행 경로를 만드는 것**이다.
///    늘려야 한다면 docs/06 결정 ③ 을 먼저 고치고 이유를 적어라.
bool allowed(const std::string& rel) {
    return rel.rfind("sigan/", 0) == 0 ||  // 발행기 자신
           rel.rfind("tests/", 0) == 0 ||  // 테스트는 직접 불러도 된다
           rel == "record/src/sigan_recorder.cpp"; // 유일한 IStrokeRecorder 구현체
}

usize countCalls(const std::string& text) {
    usize n = 0;
    for (const char* pat : {"->publish(", ".publish("}) {
        const std::string needle = pat;
        usize at = text.find(needle);
        while (at != std::string::npos) {
            ++n;
            at = text.find(needle, at + needle.size());
        }
    }
    return n;
}

} // namespace

MARI_TEST(single_publish_path) {
    const std::filesystem::path root = repoRoot();
    CHECK(std::filesystem::exists(root / "sigan"));
    CHECK(std::filesystem::exists(root / "record"));

    usize scanned = 0;
    usize allowedCalls = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::string rel = relSlash(e.path(), root);
        // 빌드 산출물과 .git 은 소스가 아니다.
        if (rel.rfind("build", 0) == 0 || rel.rfind(".git/", 0) == 0) {
            continue;
        }
        const auto ext = e.path().extension();
        if (ext != ".cpp" && ext != ".hpp") {
            continue;
        }
        ++scanned;
        const usize calls = countCalls(readFile(e.path()));
        if (calls == 0) {
            continue;
        }
        if (!allowed(rel)) {
            CHECK_FAIL(std::string("🔴 두 번째 발행 경로가 생겼다: ") + rel + " (" +
                       std::to_string(calls) +
                       "건). 발행은 record/src/sigan_recorder.cpp 하나를 지나야 한다"
                       "(docs/06 결정 ③)");
        } else {
            allowedCalls += calls;
        }
    }

    CHECK(scanned > 50);       // 스캔이 실제로 돌았는지(빈 통과 방지)
    CHECK(allowedCalls > 0);   // 찾는 패턴이 여전히 유효한지(오탐이 아니라 미탐 방지)

    // 🔴 그리고 그 하나가 정말 거기 있는지 눈으로도 못박는다.
    const std::string impl = readFile(root / "record" / "src" / "sigan_recorder.cpp");
    CHECK(impl.find("pub_->publish(") != std::string::npos);
    // 🔴 경계선(docs/03 2절): 배선 계층이 증명을 시작하지 않았는지 같이 본다.
    CHECK(impl.find("ES256") == std::string::npos);
    CHECK(impl.find("prevHash") == std::string::npos);
    CHECK(impl.find("ai-assisted") == std::string::npos);
    CHECK(impl.find("human-only") == std::string::npos);
}

MARI_TEST(neither_app_nor_agent_links_sigan) {
    // 🔴 의존 방향. app 과 agent 는 `agent::IStrokeRecorder` 만 알고 sigan 을 모른다.
    //    이게 깨지면 "Sigan 없이는 못 그리는 빌드"가 되고, docs/03 5.1 이 정상이라고
    //    규정한 상태(Sigan 미설치)에서 제품이 죽는다.
    const std::filesystem::path root = repoRoot();
    for (const char* mod : {"app", "agent"}) {
        const std::string cml = readFile(root / mod / "CMakeLists.txt");
        CHECK(!cml.empty());
        CHECK(cml.find("mari::sigan") == std::string::npos);
    }
    // 소스에서도 sigan 헤더를 직접 들이지 않는다.
    for (const char* mod : {"app", "agent"}) {
        for (const auto& e : std::filesystem::recursive_directory_iterator(root / mod)) {
            if (!e.is_regular_file()) {
                continue;
            }
            const auto ext = e.path().extension();
            if (ext != ".cpp" && ext != ".hpp") {
                continue;
            }
            const std::string text = readFile(e.path());
            if (text.find("#include <mari/sigan/") != std::string::npos) {
                CHECK_FAIL(std::string("app/agent 가 sigan 을 직접 들였다: ") +
                           relSlash(e.path(), root));
            }
        }
    }
    // 공개 헤더 쪽도 같이 본다(include/mari/app · include/mari/agent).
    for (const char* mod : {"app", "agent"}) {
        const std::filesystem::path inc = root / "include" / "mari" / mod;
        for (const auto& e : std::filesystem::recursive_directory_iterator(inc)) {
            if (!e.is_regular_file() || e.path().extension() != ".hpp") {
                continue;
            }
            const std::string text = readFile(e.path());
            if (text.find("#include <mari/sigan/") != std::string::npos) {
                CHECK_FAIL(std::string("공개 헤더가 sigan 을 직접 들였다: ") +
                           relSlash(e.path(), root));
            }
        }
    }
}

MARI_TEST_MAIN()
