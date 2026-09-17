// Mari Paint — 경계 검사하는 빅엔디안 바이트 리더.
//
// .abr(Photoshop)·.sut 의 effector blob 은 둘 다 **빅엔디안**이고, 둘 다 스펙이 없다.
// 스펙이 없다는 건 "잘린 파일·거짓말하는 길이 필드가 반드시 온다"는 뜻이다.
// 그래서 모든 읽기는 남은 바이트를 먼저 확인하고, 모자라면 조용히 실패 상태로 넘어간다.
// 한 번 실패하면 이후 읽기는 전부 실패한다(오염 전파). 예외는 던지지 않는다.
#ifndef MARI_IO_BRUSH_BYTE_READER_HPP
#define MARI_IO_BRUSH_BYTE_READER_HPP

#include <mari/core/types.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace mari::io::brush {

/// 빅엔디안 바이트 스트림 리더. 범위를 벗어나면 failed() 가 서고 0 을 돌려준다.
class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const u8* data, usize size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<u8>& v) : data_(v.data()), size_(v.size()) {}

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] usize pos() const noexcept { return pos_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize remaining() const noexcept { return failed_ ? 0 : size_ - pos_; }
    [[nodiscard]] bool eof() const noexcept { return failed_ || pos_ >= size_; }
    [[nodiscard]] const u8* data() const noexcept { return data_; }

    /// 실패 상태를 세운다. 이후 모든 읽기가 실패한다.
    void fail() noexcept { failed_ = true; }

    /// n 바이트를 더 읽을 수 있는가.
    [[nodiscard]] bool has(usize n) const noexcept { return !failed_ && size_ - pos_ >= n; }

    /// 절대 위치로 이동. 범위를 벗어나면 실패한다.
    void seek(usize p) noexcept {
        if (p > size_)
            fail();
        else if (!failed_)
            pos_ = p;
    }

    /// n 바이트 건너뛴다.
    void skip(usize n) noexcept {
        if (!has(n))
            fail();
        else
            pos_ += n;
    }

    /// 2/4 바이트 경계로 정렬한다(abr 섹션 패딩용).
    void align(usize boundary) noexcept {
        if (boundary == 0)
            return;
        const usize rem = pos_ % boundary;
        if (rem != 0)
            skip(boundary - rem);
    }

    [[nodiscard]] u8 u8v() noexcept {
        if (!has(1)) {
            fail();
            return 0;
        }
        return data_[pos_++];
    }

    [[nodiscard]] u16 u16be() noexcept {
        if (!has(2)) {
            fail();
            return 0;
        }
        const u16 v = static_cast<u16>((static_cast<u16>(data_[pos_]) << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return v;
    }

    [[nodiscard]] i16 i16be() noexcept { return static_cast<i16>(u16be()); }

    [[nodiscard]] u32 u32be() noexcept {
        if (!has(4)) {
            fail();
            return 0;
        }
        const u32 v = (static_cast<u32>(data_[pos_]) << 24) |
                      (static_cast<u32>(data_[pos_ + 1]) << 16) |
                      (static_cast<u32>(data_[pos_ + 2]) << 8) | static_cast<u32>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    [[nodiscard]] i32 i32be() noexcept { return static_cast<i32>(u32be()); }

    [[nodiscard]] u64 u64be() noexcept {
        const u64 hi = u32be();
        const u64 lo = u32be();
        return (hi << 32) | lo;
    }

    /// IEEE754 배정밀도(빅엔디안).
    [[nodiscard]] f64 f64be() noexcept {
        const u64 bits = u64be();
        f64 out = 0.0;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    /// IEEE754 단정밀도(빅엔디안).
    [[nodiscard]] f32 f32be() noexcept {
        const u32 bits = u32be();
        f32 out = 0.0f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    /// 고정 길이 ASCII. Adobe 의 4바이트 키에 쓴다.
    [[nodiscard]] std::string ascii(usize n) noexcept {
        if (!has(n)) {
            fail();
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

    /// 원시 바이트 n 개.
    [[nodiscard]] std::vector<u8> bytes(usize n) noexcept {
        if (!has(n)) {
            fail();
            return {};
        }
        std::vector<u8> v(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return v;
    }

    /// Adobe 유니코드 문자열: u32 문자 수 + UTF-16BE. UTF-8 로 돌려준다.
    /// 길이에 NUL 종단이 포함되는 경우가 흔해 끝의 NUL 은 떼어낸다.
    [[nodiscard]] std::string unicodeString(usize maxChars = 1u << 20) noexcept;

    /// Adobe 키: u32 길이(0 이면 4바이트 고정 키) + ASCII.
    [[nodiscard]] std::string keyString() noexcept {
        const u32 len = u32be();
        return ascii(len == 0 ? 4u : len);
    }

private:
    const u8* data_ = nullptr;
    usize size_ = 0;
    usize pos_ = 0;
    bool failed_ = false;
};

} // namespace mari::io::brush

#endif // MARI_IO_BRUSH_BYTE_READER_HPP
