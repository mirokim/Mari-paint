// Mari Paint — 최소 테스트 하네스 (헤더온리, 외부 의존 없음)
//
// 사용법:
//   #include <mari/test/harness.hpp>
//   MARI_TEST(tile_is_64px) {
//       CHECK_EQ(mari::kTileSize, 64);
//   }
//   MARI_TEST_MAIN()            // 실행파일마다 정확히 한 번
//
// 실패하면 "파일:줄" 과 함께 메시지를 찍고, 종료코드로 성공(0)/실패(1)를 알린다.
#ifndef MARI_TEST_HARNESS_HPP
#define MARI_TEST_HARNESS_HPP

#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mari::test {

/// 테스트 하나의 실행 문맥. 실패를 기록한다.
struct Context {
    int failures = 0;
    const char* name = "";

    void fail(const char* file, int line, const std::string& what) {
        ++failures;
        std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, what.c_str());
    }
};

using TestFn = void (*)(Context&);

/// 등록된 테스트 목록. 정적 초기화 순서 문제를 피하려 함수 지역 정적을 쓴다.
inline std::vector<std::pair<const char*, TestFn>>& registry() {
    static std::vector<std::pair<const char*, TestFn>> r;
    return r;
}

/// MARI_TEST 매크로가 만드는 자동 등록자.
struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().emplace_back(name, fn); }
};

/// 값을 사람이 읽을 수 있는 문자열로. 출력 불가한 타입은 "<?>" 로 떨어진다.
template <typename T> inline std::string show(const T& v) {
    if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(v);
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(static_cast<double>(v));
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else {
        return "<?>";
    }
}

/// 등록된 테스트를 전부 돌린다. 실패한 테스트 수를 반환한다.
inline int runAll() {
    int failed = 0;
    const auto& tests = registry();
    for (const auto& [name, fn] : tests) {
        Context ctx;
        ctx.name = name;
        std::printf("[ RUN  ] %s\n", name);
        try {
            fn(ctx);
        } catch (const std::exception& e) {
            ctx.fail(__FILE__, __LINE__, std::string("예외 발생: ") + e.what());
        } catch (...) {
            ctx.fail(__FILE__, __LINE__, "알 수 없는 예외 발생");
        }
        if (ctx.failures == 0) {
            std::printf("[  OK  ] %s\n", name);
        } else {
            std::printf("[ FAIL ] %s (%d건)\n", name, ctx.failures);
            ++failed;
        }
    }
    std::printf("----\n%zu개 중 %d개 실패\n", tests.size(), failed);
    return failed;
}

} // namespace mari::test

/// 테스트 정의 + 자동 등록. 본문에서 ctx 를 통해 실패를 보고한다(매크로가 대신 한다).
#define MARI_TEST(name)                                                                            \
    static void mari_test_##name(::mari::test::Context&);                                          \
    static ::mari::test::Registrar mari_test_reg_##name{#name, &mari_test_##name};                 \
    static void mari_test_##name([[maybe_unused]] ::mari::test::Context& mari_ctx)

/// 조건이 참인지 검사한다.
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond))                                                                               \
            mari_ctx.fail(__FILE__, __LINE__, "CHECK(" #cond ") 실패");                            \
    } while (0)

/// 두 값이 같은지 검사한다.
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto& mari_a = (a);                                                                  \
        const auto& mari_b = (b);                                                                  \
        if (!(mari_a == mari_b))                                                                   \
            mari_ctx.fail(__FILE__, __LINE__,                                                      \
                          "CHECK_EQ(" #a ", " #b ") 실패: " + ::mari::test::show(mari_a) +         \
                              " != " + ::mari::test::show(mari_b));                                \
    } while (0)

/// 두 값이 다른지 검사한다.
#define CHECK_NE(a, b)                                                                             \
    do {                                                                                           \
        const auto& mari_a = (a);                                                                  \
        const auto& mari_b = (b);                                                                  \
        if (mari_a == mari_b)                                                                      \
            mari_ctx.fail(__FILE__, __LINE__,                                                      \
                          "CHECK_NE(" #a ", " #b ") 실패: 둘 다 " + ::mari::test::show(mari_a));   \
    } while (0)

/// 부동소수 근사 비교. |a-b| <= eps 를 본다.
#define CHECK_NEAR(a, b, eps)                                                                      \
    do {                                                                                           \
        const double mari_a = static_cast<double>(a);                                              \
        const double mari_b = static_cast<double>(b);                                              \
        const double mari_e = static_cast<double>(eps);                                            \
        if (!(std::fabs(mari_a - mari_b) <= mari_e))                                               \
            mari_ctx.fail(__FILE__, __LINE__,                                                      \
                          "CHECK_NEAR(" #a ", " #b ") 실패: " + std::to_string(mari_a) +           \
                              " vs " + std::to_string(mari_b) +                                    \
                              " (허용 " + std::to_string(mari_e) + ")");                           \
    } while (0)

/// 실패를 강제로 기록한다.
#define CHECK_FAIL(msg) mari_ctx.fail(__FILE__, __LINE__, (msg))

/// main() 을 생성한다. 테스트 실행파일마다 한 번만 쓴다.
#define MARI_TEST_MAIN()                                                                           \
    int main() { return ::mari::test::runAll() == 0 ? 0 : 1; }

#endif // MARI_TEST_HARNESS_HPP
