// Result<T> 테스트 — 예외 없이 오류가 전달되는지 본다.
#include <mari/core/result.hpp>
#include <mari/test/harness.hpp>

#include <string>

using namespace mari;

static Result<int> parsePositive(int v) {
    if (v <= 0)
        return Err("양수가 아니다", ErrorCode::InvalidArgument);
    return v * 2;
}

static Result<void> doWork(bool fail) {
    if (fail)
        return Err("작업 실패", ErrorCode::IoError);
    return Ok();
}

MARI_TEST(result_success) {
    auto r = parsePositive(21);
    CHECK(r.ok());
    CHECK(static_cast<bool>(r));
    CHECK_EQ(r.value(), 42);
}

MARI_TEST(result_failure) {
    auto r = parsePositive(-1);
    CHECK(!r.ok());
    CHECK(!static_cast<bool>(r));
    CHECK(r.code() == ErrorCode::InvalidArgument);
    CHECK_EQ(r.message(), std::string("양수가 아니다"));
    CHECK_EQ(r.valueOr(7), 7);
}

MARI_TEST(result_void) {
    auto ok = doWork(false);
    CHECK(ok.ok());
    auto bad = doWork(true);
    CHECK(!bad.ok());
    CHECK(bad.code() == ErrorCode::IoError);
}

MARI_TEST(result_moves_nontrivial) {
    Result<std::string> r = Ok(std::string("타일"));
    CHECK(r.ok());
    CHECK_EQ(r.value(), std::string("타일"));
    std::string taken = std::move(r).value();
    CHECK_EQ(taken, std::string("타일"));
}

MARI_TEST(error_default_code) {
    Result<int> r = Err("무엇인가 잘못됐다");
    CHECK(r.code() == ErrorCode::Unknown);
}

MARI_TEST_MAIN()
