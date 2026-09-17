// Mari Paint — Adobe ActionDescriptor 파서 구현.
//
// 구조(PSD 파일 포맷 문서의 "Descriptor structure" 기준):
//   Descriptor := unicode className | key classId | u32 itemCount | itemCount × (key, OSType, value)
//   key        := u32 len (0 이면 4) | ASCII
//
// 재귀 깊이에 상한을 둔다. 깨진 파일로 스택을 터뜨리는 건 파서가 아니라 버그다.
#include <mari/io/brush/abr_descriptor.hpp>

namespace mari::io::brush {
namespace {

/// 깨진/악의적 파일로 무한 재귀하지 않도록 건 상한.
constexpr int kMaxDepth = 32;
/// 한 디스크립터/리스트가 가질 수 있는 항목 수 상한. 길이 필드가 거짓말할 때의 방어선.
constexpr u32 kMaxItems = 1u << 16;

Result<DescValue> parseValue(ByteReader& r, int depth);

Result<DescriptorPtr> parseDescriptorBody(ByteReader& r, int depth) {
    if (depth > kMaxDepth)
        return Err("디스크립터 중첩이 너무 깊다", ErrorCode::ParseError);

    auto d = std::make_shared<Descriptor>();
    d->className = r.unicodeString();
    d->classId = r.keyString();
    const u32 count = r.u32be();
    if (r.failed())
        return Err("디스크립터 머리말이 잘렸다", ErrorCode::ParseError);
    if (count > kMaxItems)
        return Err("디스크립터 항목 수가 비정상이다", ErrorCode::ParseError);

    d->items.reserve(count < 64 ? count : 64);
    for (u32 i = 0; i < count; ++i) {
        std::string key = r.keyString();
        if (r.failed())
            return Err("디스크립터 키가 잘렸다", ErrorCode::ParseError);
        auto v = parseValue(r, depth + 1);
        if (!v)
            return v.error();
        d->items.emplace_back(std::move(key), std::move(v).value());
    }
    return Ok(DescriptorPtr(std::move(d)));
}

Result<DescValue> parseValue(ByteReader& r, int depth) {
    if (depth > kMaxDepth)
        return Err("디스크립터 중첩이 너무 깊다", ErrorCode::ParseError);

    DescValue v;
    v.osType = r.ascii(4);
    if (r.failed())
        return Err("OSType 태그가 잘렸다", ErrorCode::ParseError);

    const std::string& t = v.osType;
    if (t == "Objc" || t == "GlbO") {
        v.type = DescType::Descriptor;
        auto sub = parseDescriptorBody(r, depth + 1);
        if (!sub)
            return sub.error();
        v.descriptor = std::move(sub).value();
    } else if (t == "VlLs") {
        v.type = DescType::List;
        const u32 n = r.u32be();
        if (r.failed())
            return Err("리스트 길이가 잘렸다", ErrorCode::ParseError);
        if (n > kMaxItems)
            return Err("리스트 항목 수가 비정상이다", ErrorCode::ParseError);
        v.list.reserve(n < 64 ? n : 64);
        for (u32 i = 0; i < n; ++i) {
            auto item = parseValue(r, depth + 1);
            if (!item)
                return item.error();
            v.list.push_back(std::move(item).value());
        }
    } else if (t == "doub") {
        v.type = DescType::Double;
        v.number = r.f64be();
    } else if (t == "UntF") {
        v.type = DescType::UnitFloat;
        v.unit = r.ascii(4);
        v.number = r.f64be();
    } else if (t == "TEXT") {
        v.type = DescType::Text;
        v.text = r.unicodeString();
    } else if (t == "enum") {
        v.type = DescType::Enumerated;
        v.enumType = r.keyString();
        v.enumValue = r.keyString();
    } else if (t == "long") {
        v.type = DescType::Integer;
        v.number = static_cast<f64>(r.i32be());
    } else if (t == "comp") {
        v.type = DescType::LargeInteger;
        v.number = static_cast<f64>(static_cast<i64>(r.u64be()));
    } else if (t == "bool") {
        v.type = DescType::Boolean;
        v.boolean = r.u8v() != 0;
    } else if (t == "type" || t == "GlbC") {
        v.type = DescType::Class;
        v.text = r.unicodeString(); // className
        v.enumType = r.keyString(); // classId
    } else if (t == "alis") {
        v.type = DescType::Alias;
        const u32 n = r.u32be();
        v.raw = r.bytes(n);
    } else if (t == "tdta") {
        v.type = DescType::RawData;
        const u32 n = r.u32be();
        v.raw = r.bytes(n);
    } else if (t == "obj ") {
        // 참조(Reference). 브러시 프리셋에는 거의 안 나오지만 만나면 건너뛸 수 있어야 한다.
        v.type = DescType::Reference;
        const u32 n = r.u32be();
        if (r.failed())
            return Err("참조 길이가 잘렸다", ErrorCode::ParseError);
        if (n > kMaxItems)
            return Err("참조 항목 수가 비정상이다", ErrorCode::ParseError);
        for (u32 i = 0; i < n; ++i) {
            const std::string form = r.ascii(4);
            if (r.failed())
                return Err("참조 폼 태그가 잘렸다", ErrorCode::ParseError);
            if (form == "prop") {
                (void)r.unicodeString();
                (void)r.keyString();
                (void)r.keyString();
            } else if (form == "Clss") {
                (void)r.unicodeString();
                (void)r.keyString();
            } else if (form == "Enmr") {
                (void)r.unicodeString();
                (void)r.keyString();
                (void)r.keyString();
                (void)r.keyString();
            } else if (form == "rele" || form == "indx") {
                (void)r.unicodeString();
                (void)r.keyString();
                (void)r.u32be();
            } else if (form == "Idnt" || form == "name") {
                (void)r.unicodeString();
                (void)r.keyString();
                if (form == "name")
                    (void)r.unicodeString();
                else
                    (void)r.u32be();
            } else {
                return Err("모르는 참조 폼: " + form, ErrorCode::ParseError);
            }
            if (r.failed())
                return Err("참조 항목이 잘렸다", ErrorCode::ParseError);
        }
    } else {
        // 길이를 모르는 태그는 건너뛸 수도 없다. 여기서 멈추고 상위가 리포트한다.
        return Err("모르는 OSType 태그: " + t, ErrorCode::ParseError);
    }

    if (r.failed())
        return Err("값(" + t + ") 이 잘렸다", ErrorCode::ParseError);
    return Ok(std::move(v));
}

} // namespace

const DescValue* Descriptor::find(std::string_view key) const {
    for (const auto& kv : items)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

const char* descTypeName(DescType t) {
    switch (t) {
    case DescType::Unknown:      return "unknown";
    case DescType::Descriptor:   return "descriptor";
    case DescType::List:         return "list";
    case DescType::Double:       return "double";
    case DescType::UnitFloat:    return "unit-float";
    case DescType::Text:         return "text";
    case DescType::Enumerated:   return "enum";
    case DescType::Integer:      return "integer";
    case DescType::LargeInteger: return "large-integer";
    case DescType::Boolean:      return "boolean";
    case DescType::Class:        return "class";
    case DescType::Alias:        return "alias";
    case DescType::RawData:      return "raw-data";
    case DescType::Reference:    return "reference";
    }
    return "unknown";
}

Result<DescriptorPtr> parseDescriptor(ByteReader& r) { return parseDescriptorBody(r, 0); }

Result<DescriptorPtr> parseDescriptorSection(ByteReader& r) {
    const u32 version = r.u32be();
    if (r.failed())
        return Err("desc 섹션 버전이 잘렸다", ErrorCode::ParseError);
    if (version != 16)
        return Err("모르는 desc 섹션 버전: " + std::to_string(version), ErrorCode::Unsupported);
    return parseDescriptorBody(r, 0);
}

void collectKeyPaths(const Descriptor& d, const std::string& prefix,
                     std::vector<std::string>& out) {
    for (const auto& [key, value] : d.items) {
        const std::string path = prefix.empty() ? key : prefix + "/" + key;
        out.push_back(path);
        if (value.type == DescType::Descriptor && value.descriptor)
            collectKeyPaths(*value.descriptor, path, out);
        else if (value.type == DescType::List) {
            for (usize i = 0; i < value.list.size(); ++i) {
                const auto& item = value.list[i];
                if (item.type == DescType::Descriptor && item.descriptor)
                    collectKeyPaths(*item.descriptor, path + "[" + std::to_string(i) + "]", out);
            }
        }
    }
}

} // namespace mari::io::brush
