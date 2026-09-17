// Mari Paint — 최소 ZIP 컨테이너 (.ora 의 껍데기)
//
// .ora = ZIP + PNG + XML 이다(docs/01 3.4). 외부 의존성을 끌어오지 않으므로
// 시스템 zlib 의 deflate/inflate 만 써서 ZIP 을 직접 만들고 읽는다.
//
// 지원 범위는 **의도적으로 좁다.** .ora 에 필요한 것만 한다.
//   · 압축 방식: Store(0) / Deflate(8) 둘뿐
//   · Zip64 없음, 암호화 없음, 분할 아카이브 없음, 디스크 스팬 없음
//   · 디렉터리 항목을 만들지 않는다(경로에 '/' 를 넣어 표현한다)
//
// OpenRaster 규약: **`mimetype` 항목은 무압축(Store)으로 맨 앞**에 와야 한다.
// ZipWriter 가 이걸 강제한다 — beginOra() 를 쓰면 자동으로 들어간다.
#ifndef MARI_ORA_ZIP_HPP
#define MARI_ORA_ZIP_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace mari::ora {

/// ZIP 압축 방식. .ora 에 필요한 두 가지만 둔다.
enum class ZipMethod : u16 {
    Store = 0,   ///< 무압축. mimetype 과 이미 압축된 PNG 에 쓴다
    Deflate = 8, ///< zlib raw deflate
};

/// 중앙 디렉터리에서 읽어낸 항목 하나의 메타데이터.
struct ZipEntryInfo {
    std::string name;         ///< 아카이브 안 경로 ('/' 구분, 앞에 '/' 없음)
    ZipMethod method = ZipMethod::Store;
    u32 crc = 0;              ///< 압축 전 데이터의 CRC-32
    u64 compressedSize = 0;
    u64 uncompressedSize = 0;
    u64 localHeaderOffset = 0; ///< 아카이브 선두로부터의 바이트 오프셋
};

/// ZIP 아카이브를 메모리에 만든다. 항목을 더한 순서가 곧 파일 안 순서다.
class ZipWriter {
public:
    ZipWriter() = default;

    /// 바이트 배열 하나를 항목으로 더한다. 같은 이름을 두 번 더하면 오류다.
    [[nodiscard]] Result<void> add(std::string name, const u8* data, usize size,
                                   ZipMethod method, int level = 6);
    /// 문자열 편의 오버로드.
    [[nodiscard]] Result<void> add(std::string name, std::string_view text, ZipMethod method,
                                   int level = 6);

    /// 아직 항목이 없는 상태에서만 부를 수 있다. mimetype 을 Store 로 맨 앞에 넣는다.
    [[nodiscard]] Result<void> beginOra();

    [[nodiscard]] usize entryCount() const { return entries_.size(); }
    [[nodiscard]] bool contains(std::string_view name) const;

    /// 중앙 디렉터리와 EOCD 를 붙여 완성된 아카이브 바이트를 돌려준다.
    /// 호출 후 writer 는 비워진다.
    [[nodiscard]] Result<std::vector<u8>> finish();

private:
    struct Pending {
        std::string name;
        ZipMethod method = ZipMethod::Store;
        u32 crc = 0;
        u32 compressedSize = 0;
        u32 uncompressedSize = 0;
        u32 localHeaderOffset = 0;
    };

    std::vector<u8> buffer_;
    std::vector<Pending> entries_;
};

/// 메모리에 올린 ZIP 아카이브를 읽는다. 손상된 입력에는 **던지지 않고** 오류를 돌려준다.
class ZipReader {
public:
    /// 바이트를 소유권째 넘긴다. 중앙 디렉터리를 파싱해 두고, 데이터는 필요할 때 푼다.
    [[nodiscard]] static Result<ZipReader> open(std::vector<u8> bytes);

    [[nodiscard]] const std::vector<ZipEntryInfo>& entries() const { return entries_; }
    [[nodiscard]] bool contains(std::string_view name) const { return find(name) != nullptr; }
    [[nodiscard]] const ZipEntryInfo* find(std::string_view name) const;

    /// 항목 내용을 풀어서 돌려준다. CRC 를 검사한다.
    [[nodiscard]] Result<std::vector<u8>> read(std::string_view name) const;

private:
    ZipReader() = default;

    std::vector<u8> bytes_;
    std::vector<ZipEntryInfo> entries_;
};

/// raw deflate/inflate. ZIP 바깥에서도 쓸 수 있게 열어 둔다.
[[nodiscard]] Result<std::vector<u8>> deflateRaw(const u8* data, usize size, int level);
/// expectedSize 는 힌트다(0이면 모른다는 뜻).
[[nodiscard]] Result<std::vector<u8>> inflateRaw(const u8* data, usize size, usize expectedSize);

} // namespace mari::ora

#endif // MARI_ORA_ZIP_HPP
