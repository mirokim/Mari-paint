// Mari Paint — composite-op 매핑 구현. 표는 include/mari/ora/composite_op.hpp 주석에 있다.
#include <mari/ora/composite_op.hpp>

#include <array>

namespace mari::ora {
namespace {

struct Row {
    BlendMode mode;
    std::string_view canonical; ///< 쓸 때 쓰는 이름
};

constexpr std::array<Row, 19> kTable{{
    {BlendMode::Normal, "svg:src-over"},
    {BlendMode::Multiply, "svg:multiply"},
    {BlendMode::Screen, "svg:screen"},
    {BlendMode::Overlay, "svg:overlay"},
    {BlendMode::Darken, "svg:darken"},
    {BlendMode::Lighten, "svg:lighten"},
    {BlendMode::ColorDodge, "svg:color-dodge"},
    {BlendMode::ColorBurn, "svg:color-burn"},
    {BlendMode::HardLight, "svg:hard-light"},
    {BlendMode::SoftLight, "svg:soft-light"},
    {BlendMode::Difference, "svg:difference"},
    {BlendMode::Exclusion, "svg:exclusion"},
    {BlendMode::Hue, "svg:hue"},
    {BlendMode::Saturation, "svg:saturation"},
    {BlendMode::Color, "svg:color"},
    {BlendMode::Luminosity, "svg:luminosity"},
    {BlendMode::Add, "svg:plus"},
    {BlendMode::Subtract, "mari:subtract"},
    {BlendMode::Erase, "svg:dst-out"},
}};

/// 접두사(svg:, krita:, mari:, gimp:)를 떼어낸 꼬리 부분.
std::string_view stripPrefix(std::string_view op) {
    const usize colon = op.rfind(':');
    if (colon == std::string_view::npos)
        return op;
    return op.substr(colon + 1);
}

} // namespace

std::string_view compositeOpName(BlendMode mode) {
    for (const Row& r : kTable)
        if (r.mode == mode)
            return r.canonical;
    return "svg:src-over";
}

std::optional<BlendMode> blendModeFromCompositeOp(std::string_view op) {
    if (op.empty())
        return BlendMode::Normal;
    for (const Row& r : kTable)
        if (r.canonical == op)
            return r.mode;

    // 접두사를 뗀 이름으로 한 번 더. Krita·GIMP·MyPaint 가 내보내는 변종을 받아준다.
    const std::string_view tail = stripPrefix(op);
    for (const Row& r : kTable)
        if (stripPrefix(r.canonical) == tail)
            return r.mode;

    if (tail == "normal" || tail == "src-over")
        return BlendMode::Normal;
    if (tail == "add" || tail == "linear-dodge")
        return BlendMode::Add;
    if (tail == "dodge")
        return BlendMode::ColorDodge;
    if (tail == "burn")
        return BlendMode::ColorBurn;
    if (tail == "erase")
        return BlendMode::Erase;
    return std::nullopt;
}

} // namespace mari::ora
