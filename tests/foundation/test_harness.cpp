// 하네스 자체 테스트 — CHECK 계열이 실제로 통과/실패를 가려내는지.
#include <mari/test/harness.hpp>

MARI_TEST(check_variants_pass) {
    CHECK(true);
    CHECK_EQ(1 + 1, 2);
    CHECK_NE(1, 2);
    CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
}

// 일부러 실패하는 검사 두 개. 아래 테스트가 이걸 별도 문맥으로 돌린다.
// (실행하면 [FAIL] 두 줄이 stderr 에 찍히는데, 정상이다.)
static void deliberateFailures(mari::test::Context& mari_ctx) {
    CHECK(false);
    CHECK_EQ(1, 2);
}

MARI_TEST(failure_is_recorded_but_not_propagated) {
    mari::test::Context sub;
    deliberateFailures(sub);
    CHECK_EQ(sub.failures, 2);
}

MARI_TEST(registry_has_these_tests) { CHECK(mari::test::registry().size() >= 3); }

MARI_TEST_MAIN()
