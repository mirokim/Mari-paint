// Mari Paint — 최소 XML 읽기/쓰기 (stack.xml 전용)
//
// 외부 의존성을 끌어오지 않으므로 stack.xml 에 필요한 만큼만 직접 만든다.
// **지원 범위를 좁게 못 박는다.** 범용 XML 파서가 아니다.
//   · 엘리먼트, 속성, 자기닫힘 태그, 주석/PI/DOCTYPE 건너뛰기, CDATA
//   · 기본 엔티티 5개(&amp; &lt; &gt; &quot; &apos;)와 수치 참조(&#48; &#x30;)
//   · 네임스페이스는 **해석하지 않는다.** 이름을 접두사째 그대로 들고 있는다
//   · 텍스트 노드는 보관하지 않는다 — stack.xml 에 의미 있는 텍스트가 없다
#ifndef MARI_ORA_XML_HPP
#define MARI_ORA_XML_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mari::ora {

/// XML 엘리먼트 하나. 트리 노드다.
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;

    /// 속성 값 포인터. 없으면 nullptr.
    [[nodiscard]] const std::string* attr(std::string_view key) const;
    /// 속성 값 또는 기본값.
    [[nodiscard]] std::string attrOr(std::string_view key, std::string fallback) const;
};

/// 문서의 루트 엘리먼트를 파싱한다. 깨진 입력에는 던지지 않고 ParseError 를 돌려준다.
[[nodiscard]] Result<XmlNode> parseXml(std::string_view text);

/// 속성 값·텍스트용 이스케이프. 5개 기본 엔티티만 쓴다.
[[nodiscard]] std::string xmlEscape(std::string_view raw);

} // namespace mari::ora

#endif // MARI_ORA_XML_HPP
