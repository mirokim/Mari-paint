// Mari Paint — MCP 도구 스키마 생성. 선언은 include/mari/mcp/tools.hpp.
//
// 🔴 이 파일에 연산 이름이 하나도 없다. 그게 요점이다(docs/05 4절).
//    있는 것은 (a) 이름 표기 변환, (b) 타입 표기 변환, (c) 그룹별 안내문 — 셋뿐이고
//    셋 다 **연산 개수와 무관하게** 고정 크기다.
#include <mari/mcp/tools.hpp>

#include <string>
#include <vector>

namespace mari::mcp {
namespace {

using agent::OpSpec;
using agent::ParamSpec;

/// `|` 로 갈라진 한 조각을 JSON Schema 타입 이름으로.
/// 없으면 빈 문자열(= 제약을 걸지 않는다).
const char* schemaAtom(std::string_view t) noexcept {
    if (t == "string") {
        return "string";
    }
    if (t == "int") {
        return "integer";
    }
    if (t == "number") {
        return "number";
    }
    if (t == "bool") {
        return "boolean";
    }
    if (t == "array") {
        return "array";
    }
    if (t == "object") {
        return "object";
    }
    return "";
}

/// 타입 이름들을 중복 없이 모은다.
void addType(std::vector<std::string>& out, const char* name) {
    if (name == nullptr || *name == '\0') {
        return;
    }
    for (const std::string& s : out) {
        if (s == name) {
            return;
        }
    }
    out.emplace_back(name);
}

/// 시맨틱 타입을 펼친다. agent-api 의 표기 그대로를 받는다.
/// 🔴 여기서 받아 주는 모양은 **agent-api 파서가 실제로 받는 모양과 같아야 한다.**
///    스키마가 더 좁으면 AI 가 쓸 수 있는 표기를 못 쓰고, 더 넓으면 거절을 늦게 만난다.
bool expandSemantic(std::string_view t, std::vector<std::string>& out) {
    if (t == "layer") { // 이름 · id · {role:...} · "active"
        addType(out, "string");
        addType(out, "integer");
        addType(out, "object");
        return true;
    }
    if (t == "region") { // [x,y,w,h] · "canvas" · {content:...}
        addType(out, "string");
        addType(out, "array");
        addType(out, "object");
        return true;
    }
    if (t == "color") { // "#RRGGBB" · [r,g,b,a] · {r,g,b,a}
        addType(out, "string");
        addType(out, "array");
        addType(out, "object");
        return true;
    }
    if (t == "view") { // "dirty" · {mode,max}
        addType(out, "string");
        addType(out, "object");
        return true;
    }
    return false;
}

} // namespace

std::string toolNameOf(std::string_view opName) {
    std::string out;
    out.reserve(opName.size());
    for (const char c : opName) {
        out.push_back(c == '.' ? '_' : c);
    }
    return out;
}

std::string opNameOf(std::string_view toolName) {
    for (const OpSpec& op : agent::opTable()) {
        if (toolNameOf(op.name) == toolName) {
            return op.name;
        }
    }
    return {};
}

bool isExported(const OpSpec& op) noexcept {
    return op.supported && op.fn != nullptr;
}

const OpSpec* findTool(std::string_view toolName) {
    for (const OpSpec& op : agent::opTable()) {
        if (isExported(op) && toolNameOf(op.name) == toolName) {
            return &op;
        }
    }
    return nullptr;
}

const char* groupUsage(std::string_view group) noexcept {
    // 각 줄은 **언제 이 도구들을 집어 드는지**를 말한다. "무엇을 하는지"는 summary 몫이다.
    if (group == "doc") {
        return "그림을 시작·저장·닫을 때, 그리고 지금 캔버스가 어떤 상태인지 "
               "물을 때 쓴다. 다른 도구를 쓰기 전에 문서가 하나 열려 있어야 한다";
    }
    if (group == "snapshot") {
        return "시도해 보고 마음에 안 들면 되돌리려 할 때 쓴다. O(1) 이라 마음껏 찍어도 "
               "된다 — 큰 획을 긋기 전에 먼저 찍어 두는 것이 정석이다";
    }
    if (group == "layer") {
        return "선화·채색·배경을 따로 다루고 싶을 때 쓴다. 같은 레이어에 겹쳐 그리면 "
               "나중에 한쪽만 고칠 수 없다";
    }
    if (group == "draw") {
        return "실제로 그릴 때 쓴다. 획은 사람 펜과 **같은 파이프라인**을 타므로 "
               "선분을 합성하는 것이 아니라 붓질이 된다";
    }
    if (group == "select") {
        return "다음 그리기를 캔버스 일부로 가두고 싶을 때 먼저 쓴다";
    }
    if (group == "brush") {
        return "어떤 붓이 있는지 모를 때 먼저 물어보고, 획의 질감을 바꾸고 싶을 때 쓴다. "
               "붓 목록은 런타임에 발견된다 — 외워 두지 마라";
    }
    if (group == "visual") {
        return "🔴 **자기가 그린 것을 눈으로 확인할 때 쓴다.** 결과가 이미지로 돌아온다. "
               "몇 획 그린 뒤에는 반드시 한 번 본다";
    }
    if (group == "batch") {
        return "획을 수십·수백 개 그을 때 쓴다. 한 번의 왕복으로 끝나고, atomic 이면 "
               "중간에 실패해도 캔버스가 반쯤 망가지지 않는다";
    }
    if (group == "event") {
        return "사람이 같이 그리고 있거나 긴 작업의 진행을 지켜봐야 할 때 쓴다";
    }
    return nullptr;
}

Json schemaTypeOf(std::string_view apiType) {
    std::vector<std::string> types;
    usize start = 0;
    while (start <= apiType.size()) {
        const usize bar = apiType.find('|', start);
        const std::string_view piece =
            apiType.substr(start, bar == std::string_view::npos ? std::string_view::npos
                                                               : bar - start);
        if (!expandSemantic(piece, types)) {
            addType(types, schemaAtom(piece));
        }
        if (bar == std::string_view::npos) {
            break;
        }
        start = bar + 1;
    }

    if (types.empty()) {
        return Json::null(); // 모르는 표기 — 제약을 걸지 않는다
    }
    if (types.size() == 1) {
        return Json::string(types.front());
    }
    Json arr = Json::array();
    for (std::string& t : types) {
        arr.push(Json::string(std::move(t)));
    }
    return arr;
}

Json inputSchemaOf(const OpSpec& op) {
    Json props = Json::object();
    Json required = Json::array();

    const auto addParam = [&](const ParamSpec& p) {
        Json field = Json::object();
        Json type = schemaTypeOf(p.type);
        if (!type.isNull()) {
            field.set("type", std::move(type));
        }
        field.set("description", Json::string(p.desc));
        props.set(p.name, std::move(field));
        if (p.required) {
            required.push(Json::string(p.name));
        }
    };

    for (const ParamSpec& p : op.params) {
        addParam(p);
    }
    // 🔴 전 연산이 시각 피드백을 받는다(docs/05 2.1). 표에 없어도 여기서 반드시 붙인다 —
    //    "결과를 볼 수 있다"가 이 API 의 핵심이라 도구마다 예외가 있으면 안 된다.
    addParam(ParamSpec{agent::kUniversalViewParam, "view", false,
                       "결과 이미지를 어떻게 받을지. \"none\" | \"dirty\"(바뀐 곳만) | "
                       "\"full\" | \"layer:<id>\" | \"region:[x,y,w,h]\" 또는 "
                       "{\"mode\":...,\"max\":512}. 주지 않으면 이 도구의 기본값"});

    Json schema = Json::object();
    schema.set("type", Json::string("object"));
    schema.set("properties", std::move(props));
    if (!required.empty()) {
        schema.set("required", std::move(required));
    }
    // 🔴 모르는 키는 거절이다 — agent-api 디스패치가 그렇게 동작한다(capabilities.hpp).
    //    스키마도 같은 말을 해 줘야 AI 가 오타를 호출 **전에** 안다.
    schema.set("additionalProperties", Json::boolean(false));
    return schema;
}

Json toolJson(const OpSpec& op) {
    std::string desc = op.summary;
    if (const char* usage = groupUsage(op.group); usage != nullptr) {
        desc += "\n언제: ";
        desc += usage;
        desc += ".";
    }
    if (op.mutates) {
        desc += "\n캔버스를 바꾼다.";
    }
    desc += "\n기본 시각 피드백: ";
    desc += agent::viewModeName(op.defaultView);
    desc += " (view 로 바꿀 수 있다).";

    Json t = Json::object();
    t.set("name", Json::string(toolNameOf(op.name)));
    t.set("description", Json::string(std::move(desc)));
    t.set("inputSchema", inputSchemaOf(op));
    return t;
}

Json toolsArray() {
    Json arr = Json::array();
    for (const OpSpec& op : agent::opTable()) {
        if (isExported(op)) {
            arr.push(toolJson(op));
        }
    }
    return arr;
}

} // namespace mari::mcp
