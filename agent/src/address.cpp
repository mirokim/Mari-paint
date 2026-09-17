// Mari Paint — 시맨틱 주소 구현. 선언은 include/mari/agent/address.hpp.
#include <mari/agent/address.hpp>

#include <vector>

namespace mari::agent {
namespace {

struct RoleWord {
    LayerRole role;
    const char* word;
};

/// 이름 유추 사전. 한국어를 먼저 본다 — 이 앱의 주 사용자가 쓰는 말이다.
const RoleWord kRoleWords[] = {
    {LayerRole::Background, "배경"},   {LayerRole::Background, "background"},
    {LayerRole::Background, "bg"},     {LayerRole::Sketch, "러프"},
    {LayerRole::Sketch, "스케치"},     {LayerRole::Sketch, "밑그림"},
    {LayerRole::Sketch, "sketch"},     {LayerRole::Sketch, "rough"},
    {LayerRole::Lineart, "선화"},      {LayerRole::Lineart, "펜선"},
    {LayerRole::Lineart, "lineart"},   {LayerRole::Lineart, "line"},
    {LayerRole::Lineart, "ink"},       {LayerRole::Color, "채색"},
    {LayerRole::Color, "밑색"},        {LayerRole::Color, "color"},
    {LayerRole::Color, "colour"},      {LayerRole::Color, "flat"},
    {LayerRole::Shading, "명암"},      {LayerRole::Shading, "그림자"},
    {LayerRole::Shading, "shading"},   {LayerRole::Shading, "shadow"},
    {LayerRole::Effects, "효과"},      {LayerRole::Effects, "effect"},
    {LayerRole::Effects, "fx"},        {LayerRole::Text, "텍스트"},
    {LayerRole::Text, "대사"},         {LayerRole::Text, "text"},
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void flatten(const std::vector<LayerPtr>& list, std::vector<LayerPtr>& out) {
    for (const LayerPtr& l : list) {
        out.push_back(l);
        flatten(l->children(), out);
    }
}

std::vector<LayerPtr> allLayers(const LayerTree& tree) {
    std::vector<LayerPtr> out;
    flatten(tree.roots(), out);
    return out;
}

Result<LayerId> uniqueOr(const std::vector<LayerId>& hits, const std::string& what) {
    if (hits.empty()) {
        return Err("그런 레이어가 없다: " + what, ErrorCode::NotFound);
    }
    if (hits.size() > 1) {
        std::string ids;
        for (const LayerId id : hits) {
            ids += (ids.empty() ? "" : ", ") + std::to_string(id);
        }
        // 🔴 애매한 지목을 조용히 하나 고르지 않는다. AI 가 엉뚱한 레이어에 그리고도 모른다.
        return Err("레이어 지목이 애매하다(" + what + " → id " + ids + "). id 로 지목해라",
                   ErrorCode::InvalidArgument);
    }
    return Ok(hits.front());
}

} // namespace

const char* layerRoleName(LayerRole r) noexcept {
    switch (r) {
    case LayerRole::Background:
        return "background";
    case LayerRole::Sketch:
        return "sketch";
    case LayerRole::Lineart:
        return "lineart";
    case LayerRole::Color:
        return "color";
    case LayerRole::Shading:
        return "shading";
    case LayerRole::Effects:
        return "effects";
    case LayerRole::Text:
        return "text";
    case LayerRole::None:
        break;
    }
    return "";
}

LayerRole layerRoleFromName(std::string_view s) noexcept {
    const std::string k = lower(s);
    if (k == "background" || k == "bg") {
        return LayerRole::Background;
    }
    if (k == "sketch" || k == "rough") {
        return LayerRole::Sketch;
    }
    if (k == "lineart" || k == "line") {
        return LayerRole::Lineart;
    }
    if (k == "color" || k == "colour") {
        return LayerRole::Color;
    }
    if (k == "shading") {
        return LayerRole::Shading;
    }
    if (k == "effects" || k == "fx") {
        return LayerRole::Effects;
    }
    if (k == "text") {
        return LayerRole::Text;
    }
    return LayerRole::None;
}

LayerRole guessRoleFromLayerName(std::string_view layerName) noexcept {
    const std::string k = lower(layerName);
    for (const RoleWord& w : kRoleWords) {
        if (k.find(w.word) != std::string::npos) {
            return w.role;
        }
    }
    return LayerRole::None;
}

void RoleTags::set(LayerId id, LayerRole role) {
    if (role == LayerRole::None) {
        tags_.erase(id);
        return;
    }
    tags_[id] = role;
}

void RoleTags::erase(LayerId id) { tags_.erase(id); }

LayerRole RoleTags::tagged(LayerId id) const {
    const auto it = tags_.find(id);
    return it == tags_.end() ? LayerRole::None : it->second;
}

LayerRole RoleTags::effective(const Layer& layer, const char** source) const {
    const LayerRole t = tagged(layer.id());
    if (t != LayerRole::None) {
        if (source != nullptr) {
            *source = "tag";
        }
        return t;
    }
    const LayerRole g = guessRoleFromLayerName(layer.name());
    if (source != nullptr) {
        *source = g == LayerRole::None ? "" : "name";
    }
    return g;
}

Result<LayerId> resolveLayer(const LayerTree& tree, const RoleTags& roles, const Json& addr) {
    if (addr.isNull()) {
        const LayerId a = tree.activeLayer();
        if (a == kInvalidLayerId) {
            return Err("활성 레이어가 없다 — layer 를 지목해라", ErrorCode::NotFound);
        }
        return Ok(a);
    }

    if (addr.isNumber()) {
        const i64 id = addr.asInt();
        if (id <= 0 || !tree.find(static_cast<LayerId>(id))) {
            return Err("그런 레이어 id 가 없다: " + std::to_string(id), ErrorCode::NotFound);
        }
        return Ok(static_cast<LayerId>(id));
    }

    if (addr.isString()) {
        const std::string& s = addr.asString();
        if (s == "active") {
            return resolveLayer(tree, roles, Json::null());
        }
        std::vector<LayerId> hits;
        for (const LayerPtr& l : allLayers(tree)) {
            if (l->name() == s) {
                hits.push_back(l->id());
            }
        }
        return uniqueOr(hits, "이름 \"" + s + "\"");
    }

    if (!addr.isObject()) {
        return Err("layer 주소는 문자열·정수·객체여야 한다", ErrorCode::InvalidArgument);
    }

    static const char* kKnown[] = {"id", "name", "role", "index"};
    for (const std::string& k : addr.keys()) {
        bool known = false;
        for (const char* kk : kKnown) {
            known = known || k == kk;
        }
        if (!known) {
            return Err("layer 주소에 모르는 키가 있다: \"" + k + "\" (id | name | role | index)",
                       ErrorCode::InvalidArgument);
        }
    }
    if (addr.keys().size() != 1) {
        return Err("layer 주소는 키 하나만 쓴다(id | name | role | index)",
                   ErrorCode::InvalidArgument);
    }

    if (addr.has("id")) {
        return resolveLayer(tree, roles, addr["id"]);
    }
    if (addr.has("name")) {
        if (!addr["name"].isString()) {
            return Err("layer.name 은 문자열이어야 한다", ErrorCode::InvalidArgument);
        }
        return resolveLayer(tree, roles, addr["name"]);
    }
    if (addr.has("index")) {
        const i64 i = addr["index"].asInt(-1);
        const std::vector<LayerPtr>& roots = tree.roots();
        if (i < 0 || static_cast<usize>(i) >= roots.size()) {
            return Err("layer.index 가 범위를 벗어났다(루트 " + std::to_string(roots.size()) +
                           "장)",
                       ErrorCode::NotFound);
        }
        return Ok(roots[static_cast<usize>(i)]->id());
    }

    const Json& r = addr["role"];
    if (!r.isString()) {
        return Err("layer.role 은 문자열이어야 한다", ErrorCode::InvalidArgument);
    }
    const LayerRole want = layerRoleFromName(r.asString());
    if (want == LayerRole::None) {
        return Err("모르는 역할이다: \"" + r.asString() +
                       "\" (background|sketch|lineart|color|shading|effects|text)",
                   ErrorCode::InvalidArgument);
    }
    std::vector<LayerId> hits;
    for (const LayerPtr& l : allLayers(tree)) {
        if (roles.effective(*l) == want) {
            hits.push_back(l->id());
        }
    }
    return uniqueOr(hits, std::string("역할 \"") + layerRoleName(want) + "\"");
}

Result<Rect> resolveRegion(const LayerTree& tree, const RoleTags& roles, const Json& addr,
                           const Rect& selection, const Rect& fallback) {
    const Size sz = tree.canvasSize();
    const Rect canvas{0, 0, sz.width, sz.height};

    if (addr.isNull()) {
        return Ok(fallback);
    }
    if (addr.isArray()) {
        return rectFromJson(addr);
    }
    if (addr.isString()) {
        const std::string& s = addr.asString();
        if (s == "canvas") {
            return Ok(canvas);
        }
        if (s == "selection") {
            if (selection.isEmpty()) {
                return Err("선택 영역이 비어 있다", ErrorCode::NotFound);
            }
            return Ok(selection);
        }
        return Err("모르는 region 문자열이다: \"" + s + "\" (canvas | selection)",
                   ErrorCode::InvalidArgument);
    }
    if (!addr.isObject()) {
        return Err("region 주소는 배열·문자열·객체여야 한다", ErrorCode::InvalidArgument);
    }

    static const char* kKnown[] = {"rect", "selection", "content", "layer"};
    for (const std::string& k : addr.keys()) {
        bool known = false;
        for (const char* kk : kKnown) {
            known = known || k == kk;
        }
        if (!known) {
            return Err("region 에 모르는 키가 있다: \"" + k +
                           "\" (rect | selection | content | layer)",
                       ErrorCode::InvalidArgument);
        }
    }

    if (addr.has("rect")) {
        return rectFromJson(addr["rect"]);
    }
    if (addr.has("selection")) {
        if (selection.isEmpty()) {
            return Err("선택 영역이 비어 있다", ErrorCode::NotFound);
        }
        return Ok(selection);
    }
    if (!addr.has("content")) {
        return Err("region 객체에 rect | selection | content 중 하나가 있어야 한다",
                   ErrorCode::InvalidArgument);
    }

    const std::string& what = addr["content"].asString();
    if (what != "nonEmpty" && what != "all") {
        return Err("region.content 는 \"nonEmpty\" 또는 \"all\" 이다", ErrorCode::InvalidArgument);
    }

    Rect area{};
    if (what == "all" || !addr.has("layer")) {
        for (const LayerPtr& l : tree.roots()) {
            area = area.united(l->bounds());
        }
    } else {
        const Result<LayerId> id = resolveLayer(tree, roles, addr["layer"]);
        if (!id.ok()) {
            return id.error();
        }
        area = tree.find(id.value())->bounds();
    }
    if (area.isEmpty()) {
        return Err("내용이 있는 영역이 없다(빈 레이어다)", ErrorCode::NotFound);
    }
    return Ok(area);
}

} // namespace mari::agent
