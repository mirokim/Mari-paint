// Mari Paint — Adobe ActionDescriptor 파서 (.abr 의 `desc` 섹션)
//
// 포토샵 브러시의 **동작**(간격·필압 반응·흩뿌림·텍스처)은 전부 `desc` 섹션 안의
// 직렬화된 ActionDescriptor 에 들어 있다. 팁 비트맵만 읽는 임포터가 "텍스처만 가져온다"고
// 욕먹는 이유가 여기 있다 — 이 파서가 Mari 의 차별점의 절반이다.
//
// 공식 스펙이 없어서 PSD 파일 포맷 문서의 Descriptor 구조를 기준으로 구현했다.
// 모르는 OSType 을 만나면 **그 자리에서 멈추지 않고** Unknown 값으로 남긴 뒤
// 상위(임포터)가 ImportReport 에 적을 수 있게 한다. 조용히 버리지 않는 게 원칙이다.
#ifndef MARI_IO_BRUSH_ABR_DESCRIPTOR_HPP
#define MARI_IO_BRUSH_ABR_DESCRIPTOR_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>
#include <mari/io/brush/byte_reader.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mari::io::brush {

struct DescValue;

/// ActionDescriptor 하나. 키는 원본 순서를 유지한다(리포트 재현성).
struct Descriptor {
    /// 클래스 이름(유니코드) 과 클래스 ID(4바이트 키 또는 긴 문자열).
    std::string className;
    std::string classId;
    /// {키, 값} 목록. 같은 키가 두 번 나올 수 있어 map 이 아니라 vector 다.
    std::vector<std::pair<std::string, DescValue>> items;

    /// 키로 값을 찾는다. 없으면 nullptr.
    [[nodiscard]] const DescValue* find(std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const { return find(key) != nullptr; }
};

using DescriptorPtr = std::shared_ptr<Descriptor>;

/// 값의 종류. Adobe 의 OSType 4바이트 태그에 대응한다.
enum class DescType : u8 {
    Unknown = 0, ///< 우리가 모르는 태그. raw 에 남은 바이트가 없을 수도 있다
    Descriptor,  ///< 'Objc' / 'GlbO'
    List,        ///< 'VlLs'
    Double,      ///< 'doub'
    UnitFloat,   ///< 'UntF' — unit 에 '#Prc'(%), '#Pxl'(px), '#Ang'(도) 등이 들어온다
    Text,        ///< 'TEXT'
    Enumerated,  ///< 'enum'
    Integer,     ///< 'long'
    LargeInteger,///< 'comp'
    Boolean,     ///< 'bool'
    Class,       ///< 'type' / 'GlbC'
    Alias,       ///< 'alis'
    RawData,     ///< 'tdta'
    Reference,   ///< 'obj '
};

/// 태그 문자열(리포트·디버그용).
[[nodiscard]] const char* descTypeName(DescType t);

/// ActionDescriptor 의 값 하나. 종류마다 쓰는 필드가 다르다.
struct DescValue {
    DescType type = DescType::Unknown;
    /// 원본 4바이트 OSType. 모르는 타입을 리포트에 그대로 적기 위해 보관한다.
    std::string osType;

    f64 number = 0.0;      ///< Double / UnitFloat / Integer / LargeInteger
    bool boolean = false;  ///< Boolean
    std::string text;      ///< Text / Class(classId) / Alias
    std::string unit;      ///< UnitFloat 의 단위 태그 ('#Prc' 등)
    std::string enumType;  ///< Enumerated 의 타입 키
    std::string enumValue; ///< Enumerated 의 값 키
    std::vector<u8> raw;   ///< RawData / Alias 의 원시 바이트
    DescriptorPtr descriptor; ///< Descriptor
    std::vector<DescValue> list; ///< List

    /// 숫자로 읽는다. 숫자가 아닌 값이면 fallback.
    [[nodiscard]] f64 asNumber(f64 fallback = 0.0) const {
        switch (type) {
        case DescType::Double:
        case DescType::UnitFloat:
        case DescType::Integer:
        case DescType::LargeInteger:
            return number;
        case DescType::Boolean:
            return boolean ? 1.0 : 0.0;
        default:
            return fallback;
        }
    }
    [[nodiscard]] bool asBool(bool fallback = false) const {
        return type == DescType::Boolean ? boolean : fallback;
    }
};

/// `desc` 섹션 본문을 파싱한다. 앞의 4바이트 버전(=16) 은 호출자가 이미 읽었어야 한다.
/// 실패하면 ParseError. 부분 성공은 없다 — 부분 결과가 필요하면 상위가 섹션 단위로 건너뛴다.
[[nodiscard]] Result<DescriptorPtr> parseDescriptor(ByteReader& r);

/// 앞의 버전 워드까지 포함해 읽는다(`desc` 섹션 전체).
[[nodiscard]] Result<DescriptorPtr> parseDescriptorSection(ByteReader& r);

/// 디스크립터 트리를 훑으며 "경로/키" 문자열을 모은다.
/// 임포터가 "우리가 읽은 키" 집합과 비교해 **읽지 않은 파라미터**를 리포트에 남기는 데 쓴다.
void collectKeyPaths(const Descriptor& d, const std::string& prefix,
                     std::vector<std::string>& out);

} // namespace mari::io::brush

#endif // MARI_IO_BRUSH_ABR_DESCRIPTOR_HPP
