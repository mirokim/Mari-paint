// Mari Paint — 예외 없는 오류 전달 Result<T>
//
// 규칙(docs/02 원칙): 예외는 쓰되 **핫 패스(스트로크)에서는 던지지 않는다.**
// 스트로크 파이프라인·타일 할당·파서 내부 루프는 Result<T> 로 오류를 되돌린다.
// 파일 열기 같은 저빈도 경로는 예외를 써도 된다.
#ifndef MARI_CORE_RESULT_HPP
#define MARI_CORE_RESULT_HPP

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace mari {

/// 오류 분류. 문자열 비교 없이 분기하려고 둔다. 뒤에만 더한다.
enum class ErrorCode {
    Unknown = 0,
    InvalidArgument, ///< 호출자가 잘못된 값을 줬다
    NotFound,        ///< 레이어·키·파일이 없다
    IoError,         ///< 읽기/쓰기 실패
    ParseError,      ///< 포맷이 깨졌거나 우리가 모르는 형태다
    Unsupported,     ///< 알지만 아직 지원하지 않는다
    OutOfMemory,
    Cancelled,
};

/// 오류 한 건. 코드 + 사람이 읽을 메시지.
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    Error() = default;
    explicit Error(std::string msg, ErrorCode c = ErrorCode::Unknown)
        : code(c), message(std::move(msg)) {}
};

/// 성공값 T 또는 Error 를 담는다. 예외를 던지지 않는다.
/// 값이 없는 연산은 Result<void> 를 쓴다.
template <typename T> class Result {
public:
    Result(T value) : store_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : store_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return store_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    /// 성공값 접근. ok() 가 false 일 때 부르면 미정의 동작이다.
    [[nodiscard]] T& value() & { return std::get<0>(store_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(store_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(store_)); }

    /// 실패 시 대체값.
    [[nodiscard]] T valueOr(T fallback) const& { return ok() ? std::get<0>(store_) : fallback; }

    /// 오류 접근. ok() 가 true 일 때 부르면 미정의 동작이다.
    [[nodiscard]] const Error& error() const { return std::get<1>(store_); }
    [[nodiscard]] ErrorCode code() const { return std::get<1>(store_).code; }
    [[nodiscard]] const std::string& message() const { return std::get<1>(store_).message; }

private:
    std::variant<T, Error> store_;
};

/// 값이 없는 연산용 특수화.
template <> class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {} // NOLINT

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const Error& error() const { return error_; }
    [[nodiscard]] ErrorCode code() const { return error_.code; }
    [[nodiscard]] const std::string& message() const { return error_.message; }

private:
    Error error_;
    bool ok_ = true;
};

/// 성공을 만든다. `return Ok();` / `return Ok(42);`
inline Result<void> Ok() { return Result<void>{}; }
template <typename T> Result<std::decay_t<T>> Ok(T&& v) {
    return Result<std::decay_t<T>>(std::forward<T>(v));
}

/// 오류를 만든다. `return Err("타일 없음", ErrorCode::NotFound);`
inline Error Err(std::string msg, ErrorCode code = ErrorCode::Unknown) {
    return Error(std::move(msg), code);
}

} // namespace mari

#endif // MARI_CORE_RESULT_HPP
