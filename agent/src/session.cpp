// Mari Paint — 에이전트 세션 구현. 선언은 include/mari/agent/session.hpp.
#include <mari/agent/session.hpp>

#include <mari/app/document.hpp>

#include <algorithm>
#include <cmath>

namespace mari::agent {

// ── 이벤트 리스너 ────────────────────────────────────────────────────────

/// EventHub 의 리스너. 앱 이벤트를 세션 큐로 옮긴다.
/// 🔴 Sigan 에 쓰는 것과 **같은 이벤트 버스**다(docs/05 2.7). 두 번 만들지 않는다.
class AgentSession::Listener final : public app::IAppEventListener {
public:
    explicit Listener(AgentSession& s) : s_(s) {}

    void onDocumentOpened(const std::string& path, const std::string& fileHash, i32 w,
                          i32 h) override {
        Json d = Json::object();
        d.set("path", Json::string(path));
        d.set("fileHash", Json::string(fileHash));
        d.set("width", Json::integer(static_cast<i64>(w)));
        d.set("height", Json::integer(static_cast<i64>(h)));
        s_.pushEvent("documentOpened", std::move(d));
    }
    void onDocumentSaved(const std::string& path, const std::string& fileHash,
                         i64 sizeBytes) override {
        Json d = Json::object();
        d.set("path", Json::string(path));
        d.set("fileHash", Json::string(fileHash));
        d.set("sizeBytes", Json::integer(sizeBytes));
        s_.pushEvent("documentSaved", std::move(d));
    }
    void onCanvasSnapshot(const std::string& canvasHash) override {
        Json d = Json::object();
        d.set("canvasHash", Json::string(canvasHash));
        s_.pushEvent("canvasSnapshot", std::move(d));
    }
    void onViewChanged(f64 zoom, f64 rotationDeg) override {
        Json d = Json::object();
        d.set("zoom", Json::number(zoom));
        d.set("rotationDeg", Json::number(rotationDeg));
        s_.pushEvent("viewChanged", std::move(d));
    }
    void onPaste(app::PasteSource source) override {
        Json d = Json::object();
        d.set("source", Json::string(app::pasteSourceName(source)));
        s_.pushEvent("paste", std::move(d));
    }
    void onUndo(i32 steps) override {
        Json d = Json::object();
        d.set("steps", Json::integer(static_cast<i64>(steps)));
        s_.pushEvent("undo", std::move(d));
    }
    void onLayerChanged(LayerId layerId, const std::string& changeKind) override {
        Json d = Json::object();
        d.set("layer", Json::integer(layerId));
        d.set("change", Json::string(changeKind));
        s_.pushEvent("layerChanged", std::move(d));
    }
    void onStrokeCompleted(LayerId layerId, u64 firstSeq, u64 lastSeq, i32 pointCount) override {
        Json d = Json::object();
        d.set("layer", Json::integer(layerId));
        d.set("firstSeq", Json::integer(firstSeq));
        d.set("lastSeq", Json::integer(lastSeq));
        d.set("pointCount", Json::integer(static_cast<i64>(pointCount)));
        s_.pushEvent("strokeCompleted", std::move(d));
    }

private:
    AgentSession& s_;
};

// ── 필압 프리셋 ──────────────────────────────────────────────────────────

const char* pressureProfileName(PressureProfile p) noexcept {
    switch (p) {
    case PressureProfile::Flat:
        return "flat";
    case PressureProfile::TaperIn:
        return "taper-in";
    case PressureProfile::TaperOut:
        return "taper-out";
    case PressureProfile::TaperInOut:
        return "taper-in-out";
    case PressureProfile::Pulse:
        return "pulse";
    }
    return "flat";
}

Result<PressureProfile> pressureProfileFromName(std::string_view s) {
    if (s == "flat") {
        return Ok(PressureProfile::Flat);
    }
    if (s == "taper-in") {
        return Ok(PressureProfile::TaperIn);
    }
    if (s == "taper-out") {
        return Ok(PressureProfile::TaperOut);
    }
    if (s == "taper-in-out") {
        return Ok(PressureProfile::TaperInOut);
    }
    if (s == "pulse") {
        return Ok(PressureProfile::Pulse);
    }
    return Err("모르는 pressureProfile 이다: \"" + std::string(s) +
                   "\" (flat|taper-in|taper-out|taper-in-out|pulse)",
               ErrorCode::InvalidArgument);
}

f32 pressureProfileAt(PressureProfile p, f32 t) noexcept {
    const f32 u = std::clamp(t, 0.0f, 1.0f);
    // 완전히 0 인 필압은 스탬프를 아예 안 찍게 만든다. 끝을 얇게 하되 0 으로 두지 않는다.
    constexpr f32 kMin = 0.08f;
    switch (p) {
    case PressureProfile::Flat:
        return 1.0f;
    case PressureProfile::TaperIn:
        return kMin + (1.0f - kMin) * u;
    case PressureProfile::TaperOut:
        return kMin + (1.0f - kMin) * (1.0f - u);
    case PressureProfile::TaperInOut: {
        const f32 s = std::sin(static_cast<f32>(3.14159265358979) * u);
        return kMin + (1.0f - kMin) * s;
    }
    case PressureProfile::Pulse: {
        const f32 s = 0.5f - 0.5f * std::cos(2.0f * static_cast<f32>(3.14159265358979) * u);
        return kMin + (1.0f - kMin) * s;
    }
    }
    return 1.0f;
}

// ── 세션 ─────────────────────────────────────────────────────────────────

AgentSession::AgentSession(agent::AgentStrokeGate gate) : gate_(gate) {
    listener_ = std::make_unique<Listener>(*this);
    app_.events().addListener(listener_.get());
    installBuiltinBrushes();
}

AgentSession::~AgentSession() { (void)app_.events().removeListener(listener_.get()); }

Result<std::unique_ptr<AgentSession>> AgentSession::open(std::string_view agentId) {
    // 🔴 게이트가 안 열리면 세션도 없다. 익명 AI 획은 만들지 않는다.
    Result<agent::AgentStrokeGate> gate = agent::AgentStrokeGate::open(agentId);
    if (!gate.ok()) {
        return gate.error();
    }
    return Ok(std::unique_ptr<AgentSession>(new AgentSession(std::move(gate).value())));
}

void AgentSession::installBuiltinBrushes() {
    auto add = [&](const char* name, f32 diameter, f32 hardness, f32 spacing, f32 opacity,
                   bool pressureSize) {
        brush::MariBrushPreset p;
        p.name = name;
        p.sourceFormat = "native";
        p.engine = "native";
        p.tip.diameter = diameter;
        p.tip.hardness = hardness;
        p.spacing = spacing;
        p.opacity = opacity;
        if (pressureSize) {
            brush::DynamicLink link;
            link.input = brush::DynamicInput::Pressure;
            link.output = brush::DynamicOutput::Size;
            link.amount = 1.0f;
            link.curve.points = {{0.0f, 0.15f}, {1.0f, 1.0f}};
            p.dynamics.push_back(link);
        }
        (void)addBrush(std::move(p), "native", {});
    };
    // 기본 네 자루. 런타임 발견이 원칙이므로 목록은 brush.list 로 읽게 한다.
    add("연필", 6.0f, 0.85f, 0.08f, 0.9f, true);
    add("잉크펜", 10.0f, 1.0f, 0.06f, 1.0f, true);
    add("에어브러시", 48.0f, 0.15f, 0.05f, 0.35f, true);
    add("납작붓", 28.0f, 0.6f, 0.12f, 1.0f, false);
    currentBrush_ = brushes_.empty() ? kInvalidBrushId : brushes_.front().id;
}

BrushId AgentSession::addBrush(brush::MariBrushPreset preset, std::string source,
                               std::vector<std::string> notes) {
    BrushEntry e;
    e.id = nextBrushId_++;
    e.preset = std::move(preset);
    e.source = std::move(source);
    e.notes = std::move(notes);
    brushes_.push_back(std::move(e));
    return brushes_.back().id;
}

Result<const BrushEntry*> AgentSession::resolveBrush(const Json& addr) const {
    if (addr.isNull()) {
        for (const BrushEntry& b : brushes_) {
            if (b.id == currentBrush_) {
                return Ok(&b);
            }
        }
        return Err("현재 브러시가 없다", ErrorCode::NotFound);
    }
    if (addr.isNumber()) {
        for (const BrushEntry& b : brushes_) {
            if (b.id == static_cast<BrushId>(addr.asInt())) {
                return Ok(&b);
            }
        }
        return Err("그런 브러시 id 가 없다: " + std::to_string(addr.asInt()), ErrorCode::NotFound);
    }
    if (!addr.isString()) {
        return Err("brush 는 이름(문자열) 또는 id(정수)여야 한다", ErrorCode::InvalidArgument);
    }
    const BrushEntry* hit = nullptr;
    usize count = 0;
    for (const BrushEntry& b : brushes_) {
        if (b.preset.name == addr.asString()) {
            hit = &b;
            ++count;
        }
    }
    if (count == 0) {
        return Err("그런 브러시가 없다: \"" + addr.asString() + "\" (brush.list 로 확인해라)",
                   ErrorCode::NotFound);
    }
    if (count > 1) {
        return Err("브러시 이름이 겹친다: \"" + addr.asString() + "\" — id 로 지목해라",
                   ErrorCode::InvalidArgument);
    }
    return Ok(hit);
}

app::Document* AgentSession::document() noexcept {
    return static_cast<app::Document*>(app_.activeDocument());
}

const app::Document* AgentSession::document() const noexcept {
    return const_cast<AgentSession*>(this)->document();
}

Result<app::Document*> AgentSession::requireDocument() {
    app::Document* d = document();
    if (d == nullptr) {
        return Err("열린 문서가 없다 — doc.create 나 doc.open 을 먼저 해라", ErrorCode::NotFound);
    }
    if (d->isClosed()) {
        return Err("문서가 닫혀 있다", ErrorCode::NotFound);
    }
    return Ok(d);
}

void AgentSession::subscribeEvents(std::vector<std::string> kinds) {
    subscribed_ = true;
    eventFilter_ = std::move(kinds);
}

void AgentSession::unsubscribeEvents() {
    subscribed_ = false;
    eventFilter_.clear();
    events_.clear();
}

void AgentSession::pushEvent(std::string kind, Json data) {
    if (!subscribed_) {
        return;
    }
    if (!eventFilter_.empty() &&
        std::find(eventFilter_.begin(), eventFilter_.end(), kind) == eventFilter_.end()) {
        return;
    }
    QueuedEvent e;
    e.kind = std::move(kind);
    e.data = std::move(data);
    events_.push_back(std::move(e));
    // 큐가 무한정 자라면 그게 누수다. 오래된 것부터 버린다(버렸다는 사실은 poll 이 알린다).
    constexpr usize kMaxQueued = 1024;
    while (events_.size() > kMaxQueued) {
        events_.pop_front();
    }
}

std::vector<QueuedEvent> AgentSession::drainEvents(usize max) {
    std::vector<QueuedEvent> out;
    while (!events_.empty() && out.size() < max) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return out;
}

// ── 파라미터 검증 ────────────────────────────────────────────────────────

Result<void> AgentSession::validateParams(const OpSpec& spec, const Json& request) const {
    if (!request.isObject()) {
        return Err("요청은 JSON 객체여야 한다", ErrorCode::InvalidArgument);
    }
    for (const std::string& k : request.keys()) {
        if (k == kOpKey || k == kUniversalViewParam) {
            continue;
        }
        bool known = false;
        for (const ParamSpec& p : spec.params) {
            known = known || k == p.name;
        }
        if (!known) {
            // 🔴 모르는 키는 거절한다. 이게 두 가지를 동시에 지킨다:
            //    (1) 오타가 조용히 무시되지 않는다.
            //    (2) 표에 없는 파라미터는 **존재할 수 없다** — origin 파라미터가
            //        몰래 생길 자리가 없다(docs/05 3.1).
            std::string known_list;
            for (const ParamSpec& p : spec.params) {
                known_list += (known_list.empty() ? "" : ", ") + std::string(p.name);
            }
            return Err(std::string(spec.name) + " 에 모르는 파라미터가 있다: \"" + k + "\"" +
                           (known_list.empty() ? " (받는 파라미터가 없다)"
                                               : " (받는 것: " + known_list + ")"),
                       ErrorCode::InvalidArgument);
        }
    }
    for (const ParamSpec& p : spec.params) {
        if (p.required && !request.has(p.name)) {
            return Err(std::string(spec.name) + " 에 필수 파라미터가 빠졌다: \"" + p.name + "\"",
                       ErrorCode::InvalidArgument);
        }
    }
    return Ok();
}

Result<Json> AgentSession::runOp(const OpSpec& spec, const Json& request) {
    const Result<void> v = validateParams(spec, request);
    if (!v.ok()) {
        return v.error();
    }
    if (!spec.supported) {
        return Err(std::string(spec.name) + " 는 이 빌드에서 지원하지 않는다: " +
                       spec.unsupportedReason,
                   ErrorCode::Unsupported);
    }
    if (spec.fn == nullptr) {
        return Err(std::string(spec.name) + " 에 구현이 없다", ErrorCode::Unsupported);
    }
    return spec.fn(*this, request);
}

// ── 디스패치 ─────────────────────────────────────────────────────────────

namespace {

const char* errorCodeName(ErrorCode c) noexcept {
    switch (c) {
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::IoError:
        return "IoError";
    case ErrorCode::ParseError:
        return "ParseError";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::Unknown:
        break;
    }
    return "Unknown";
}

Json errorEnvelope(const std::string& op, const Error& e) {
    Json out = Json::object();
    out.set("ok", Json::boolean(false));
    out.set("op", Json::string(op));
    Json err = Json::object();
    err.set("code", Json::string(errorCodeName(e.code)));
    err.set("message", Json::string(e.message));
    out.set("error", std::move(err));
    return out;
}

} // namespace

Json AgentSession::execute(const Json& request) {
    ++requests_;
    dirty_ = Rect{};

    if (!request.isObject()) {
        return errorEnvelope("", Err("요청은 JSON 객체여야 한다", ErrorCode::InvalidArgument));
    }
    const Json& opName = request[kOpKey];
    if (!opName.isString()) {
        return errorEnvelope("", Err("요청에 \"op\" 문자열이 없다", ErrorCode::InvalidArgument));
    }
    const std::string op = opName.asString();
    const OpSpec* spec = findOp(op);
    if (spec == nullptr) {
        return errorEnvelope(op, Err("모르는 연산이다: \"" + op +
                                         "\" (capabilities 로 목록을 받아라)",
                                     ErrorCode::NotFound));
    }

    // view 는 연산 실행 **전에** 검증한다 — 그려 놓고 view 오타로 실패하면
    // 캔버스는 바뀌었는데 응답은 실패다. 그게 제일 나쁘다.
    Result<ViewSpec> view = parseViewSpec(request[kUniversalViewParam]);
    if (!view.ok()) {
        return errorEnvelope(op, view.error());
    }
    if (!request.has(kUniversalViewParam)) {
        view.value().mode = spec->defaultView;
    }

    Result<Json> body = runOp(*spec, request);
    if (!body.ok()) {
        return errorEnvelope(op, body.error());
    }

    Json out = Json::object();
    out.set("ok", Json::boolean(true));
    out.set("op", Json::string(op));
    if (!body.value().isNull()) {
        out.set("result", std::move(body).value());
    }
    if (!dirty_.isEmpty()) {
        out.set("dirtyRect", jsonRect(dirty_));
    }

    // 🔴 시각 피드백(docs/05 2.1). 문서가 없으면 이미지도 없다 — 조용히 실패하지 않고
    //    그 사실을 응답에 적는다.
    app::Document* doc = document();
    if (view.value().mode != ViewMode::None && doc != nullptr && !doc->isClosed()) {
        Result<ViewResult> rendered = renderView(doc->layers(), view.value(), dirty_);
        if (!rendered.ok()) {
            Json note = Json::object();
            note.set("code", Json::string(errorCodeName(rendered.error().code)));
            note.set("message", Json::string(rendered.error().message));
            out.set("imageError", std::move(note)); // 연산은 성공했다. 그림만 못 그렸다.
        } else if (!rendered.value().image.isNull()) {
            out.set("image", std::move(rendered).value().image);
        }
    }
    return out;
}

std::string AgentSession::executeText(std::string_view requestJson) {
    Result<Json> req = Json::parse(requestJson);
    if (!req.ok()) {
        return errorEnvelope("", req.error()).dump();
    }
    return execute(req.value()).dump();
}

// ── 공용 도우미 ──────────────────────────────────────────────────────────

Result<Color8> colorFromJson(const Json& v, Color8 fallback) {
    if (v.isNull()) {
        return Ok(fallback);
    }
    if (v.isString()) {
        const std::string& s = v.asString();
        if (s.size() < 2 || s[0] != '#' || (s.size() != 7 && s.size() != 9 && s.size() != 4)) {
            return Err("색은 \"#RGB\" | \"#RRGGBB\" | \"#RRGGBBAA\" 형식이다: \"" + s + "\"",
                       ErrorCode::InvalidArgument);
        }
        auto hex = [](char c, int& out) {
            if (c >= '0' && c <= '9') {
                out = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                out = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                out = c - 'A' + 10;
            } else {
                return false;
            }
            return true;
        };
        int comp[8]{};
        for (usize i = 1; i < s.size(); ++i) {
            if (!hex(s[i], comp[i - 1])) {
                return Err("색에 16진수가 아닌 문자가 있다: \"" + s + "\"",
                           ErrorCode::InvalidArgument);
            }
        }
        Color8 c{};
        if (s.size() == 4) { // #RGB
            c.r = static_cast<u8>(comp[0] * 17);
            c.g = static_cast<u8>(comp[1] * 17);
            c.b = static_cast<u8>(comp[2] * 17);
            c.a = 255;
        } else {
            c.r = static_cast<u8>(comp[0] * 16 + comp[1]);
            c.g = static_cast<u8>(comp[2] * 16 + comp[3]);
            c.b = static_cast<u8>(comp[4] * 16 + comp[5]);
            c.a = s.size() == 9 ? static_cast<u8>(comp[6] * 16 + comp[7]) : static_cast<u8>(255);
        }
        return Ok(c);
    }
    if (v.isArray()) {
        if (v.size() != 3 && v.size() != 4) {
            return Err("색 배열은 [r,g,b] 또는 [r,g,b,a] 다", ErrorCode::InvalidArgument);
        }
        Color8 c{};
        u8* ch[4] = {&c.r, &c.g, &c.b, &c.a};
        c.a = 255;
        for (usize i = 0; i < v.size(); ++i) {
            const Json& e = v.at(i);
            if (!e.isNumber()) {
                return Err("색 성분이 숫자가 아니다", ErrorCode::InvalidArgument);
            }
            const i64 n = e.asInt();
            if (n < 0 || n > 255) {
                return Err("색 성분은 0..255 다", ErrorCode::InvalidArgument);
            }
            *ch[i] = static_cast<u8>(n);
        }
        return Ok(c);
    }
    if (v.isObject()) {
        Color8 c{};
        c.a = 255;
        static const char* kKeys[] = {"r", "g", "b", "a"};
        u8* ch[4] = {&c.r, &c.g, &c.b, &c.a};
        for (const std::string& k : v.keys()) {
            bool known = false;
            for (const char* kk : kKeys) {
                known = known || k == kk;
            }
            if (!known) {
                return Err("색 객체에 모르는 키가 있다: \"" + k + "\" (r|g|b|a)",
                           ErrorCode::InvalidArgument);
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (v.has(kKeys[i])) {
                const i64 n = v[kKeys[i]].asInt(-1);
                if (n < 0 || n > 255) {
                    return Err("색 성분은 0..255 다", ErrorCode::InvalidArgument);
                }
                *ch[i] = static_cast<u8>(n);
            }
        }
        return Ok(c);
    }
    return Err("색 표기를 읽을 수 없다", ErrorCode::InvalidArgument);
}

Result<BlendMode> blendModeFromName(std::string_view s) {
    for (int i = 0; i <= static_cast<int>(BlendMode::Erase); ++i) {
        const auto m = static_cast<BlendMode>(i);
        if (s == blendModeName(m)) {
            return Ok(m);
        }
    }
    return Err("모르는 블렌드 모드다: \"" + std::string(s) +
                   "\" (capabilities.blendModes 를 봐라)",
               ErrorCode::InvalidArgument);
}

f64 layerCoverage(const Layer& layer, const Size& canvas) {
    if (canvas.isEmpty()) {
        return 0.0;
    }
    const TileMap* map = layer.tiles();
    if (map == nullptr || map->tileCount() == 0) {
        return 0.0;
    }
    const Rect canvasRect{0, 0, canvas.width, canvas.height};
    DirtyTiles tiles;
    map->collectTiles(canvasRect, tiles);
    u64 opaque = 0;
    for (const TileCoord& c : tiles) {
        const ConstTilePtr t = map->at(c);
        if (!t || t->isBlank()) {
            continue;
        }
        const Rect part = c.canvasRect().intersected(canvasRect);
        if (part.isEmpty()) {
            continue;
        }
        const u8* px = t->pixels();
        const usize stride = t->stride();
        for (i32 y = part.y; y < part.bottom(); ++y) {
            const usize row = static_cast<usize>(y - c.canvasRect().y) * stride;
            for (i32 x = part.x; x < part.right(); ++x) {
                const usize idx = row + static_cast<usize>(x - c.canvasRect().x) * 4u + 3u;
                if (px[idx] != 0) {
                    ++opaque;
                }
            }
        }
    }
    return static_cast<f64>(opaque) / static_cast<f64>(canvas.area());
}

Json layerToJson(const Layer& layer, const RoleTags& roles, const Size& canvas) {
    Json j = Json::object();
    j.set("id", Json::integer(layer.id()));
    j.set("name", Json::string(layer.name()));
    j.set("kind", Json::string(layer.kind() == LayerKind::Group ? "group" : "raster"));
    const char* roleSource = "";
    const LayerRole role = roles.effective(layer, &roleSource);
    j.set("role", Json::string(layerRoleName(role)));
    // 🔴 역할의 출처를 밝힌다. 이름에서 유추한 것을 태그인 척하지 않는다.
    j.set("roleSource", Json::string(roleSource));
    j.set("opacity", Json::number(static_cast<f64>(layer.opacity())));
    j.set("blendMode", Json::string(blendModeName(layer.blendMode())));
    j.set("visible", Json::boolean(layer.visible()));
    j.set("locked", Json::boolean(layer.locked()));
    j.set("alphaLocked", Json::boolean(layer.alphaLocked()));
    j.set("bounds", jsonRect(layer.bounds()));
    if (layer.kind() == LayerKind::Raster) {
        const TileMap* map = layer.tiles();
        j.set("tiles", Json::integer(static_cast<i64>(map == nullptr ? 0 : map->tileCount())));
        j.set("coverage", Json::number(layerCoverage(layer, canvas)));
    } else {
        Json kids = Json::array();
        for (const LayerPtr& c : layer.children()) {
            kids.push(Json::integer(c->id()));
        }
        j.set("children", std::move(kids));
    }
    return j;
}

} // namespace mari::agent
