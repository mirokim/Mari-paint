// Mari Paint — 기록 배선의 알맹이 (docs/06 결정 ①·②·③·④·⑤)
//
// 🔴 이 파일이 리포에서 `SiganPublisher::publish()` 를 부르는 **유일한** 곳이다.
#include <mari/record/sigan_recorder.hpp>

#include <mari/sigan/prooflog.hpp>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace mari::record {
namespace {

// 🔴 중립 인터페이스(agent/recording.hpp)의 플래그와 와이어 플래그(sigan/frame.hpp)가
//    **같은 값**인지 컴파일 타임에 못박는다. 갈라지면 기록이 조용히 거짓말을 한다.
static_assert(static_cast<u32>(agent::RecordFlag::Down) ==
                  static_cast<u32>(sigan::FrameFlag::Down),
              "Down 비트가 어긋났다");
static_assert(static_cast<u32>(agent::RecordFlag::Move) ==
                  static_cast<u32>(sigan::FrameFlag::Move),
              "Move 비트가 어긋났다");
static_assert(static_cast<u32>(agent::RecordFlag::Up) == static_cast<u32>(sigan::FrameFlag::Up),
              "Up 비트가 어긋났다");
static_assert(static_cast<u32>(agent::RecordFlag::Eraser) ==
                  static_cast<u32>(sigan::FrameFlag::Eraser),
              "Eraser 비트가 어긋났다");
static_assert(static_cast<u32>(agent::RecordFlag::Synthetic) ==
                  static_cast<u32>(sigan::FrameFlag::Synthetic),
              "Synthetic 비트가 어긋났다 — 영역 연산이 붓질로 둔갑한다");

/// 파일 이름으로 쓸 수 없는 문자를 걷어낸다(힌트가 경로를 벗어나지 않게).
std::string sanitize(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(okChar ? c : '-');
    }
    if (out.size() > 32) {
        out.resize(32);
    }
    if (out.empty()) {
        out = "doc";
    }
    return out;
}

} // namespace

// ── SiganRecorder ────────────────────────────────────────────────────────

Result<std::unique_ptr<SiganRecorder>> SiganRecorder::open(const RecordingConfig& cfg,
                                                           const std::string& journalPath,
                                                           u64 sessionId) {
    sigan::PublisherConfig pc;
    pc.journalPath = journalPath;
    pc.app = cfg.app;
    // 🔴 `sessionId` 의 뜻은 **"문서 작업 구간 id"** 다(docs/06 결정 ⑤).
    //    agent-api 세션 id 와 다른 것이고, 후자는 와이어에 나가지 않는다.
    pc.sessionId = sessionId;
    Result<sigan::PublisherPtr> pub = sigan::SiganPublisher::open(std::move(pc));
    if (!pub.ok()) {
        return pub.error();
    }
    std::unique_ptr<SiganRecorder> r(new SiganRecorder());
    r->pub_ = std::move(pub).value();
    r->pub_->setSink(cfg.sink);
    r->journalPath_ = journalPath;
    r->app_ = cfg.app;
    r->agents_ = cfg.agents;
    r->sessionId_ = r->pub_->sessionId();
    // 싱크가 있으면 한 번 붙여 본다. **실패해도 실패가 아니다** —
    // Sigan 미설치·미기동은 정상 상태이고 정본은 저널이다(docs/03 5.1 · docs/06 결정 ④).
    if (cfg.sink != nullptr) {
        (void)r->pub_->pump();
    }
    return Ok(std::move(r));
}

SiganRecorder::~SiganRecorder() {
    if (owner_ != nullptr) {
        owner_->retire(this);
    }
}

void SiganRecorder::checkJournal() noexcept {
    // 🔴 파이프가 막힌 것은 고장이 아니다(저널에 스풀된다). 저널이 못 쓰는 것만 고장이다.
    if (pub_->journal().failed()) {
        broken_ = true;
    }
}

void SiganRecorder::onStrokePoint(const StrokeSource& src,
                                  const agent::StrokePointRecord& p) noexcept {
    // 핫 패스. 할당도 블로킹도 없다.
    sigan::StrokeSample s(src); // 🔴 출처는 받은 것을 그대로 싣는다. 고르지 않는다
    s.pos = p.pos;
    s.pressure = p.pressure;
    s.tiltX = p.tiltX;
    s.tiltY = p.tiltY;
    s.rotation = p.rotation;
    s.velocity = p.velocity;
    s.layerId = p.layerId;
    s.brushId = p.brushId;
    s.flags = p.flags;
    pub_->publish(s);
    checkJournal();
}

void SiganRecorder::onStrokeEnd(const StrokeSource& src, u32 changedTiles) noexcept {
    pub_->endStroke(); // 획 끝마다 flush(docs/03 5.4)
    checkJournal();
    // 축 A(붓질 수) · 축 C(변경 타일 수). 축 B 와 **같은 칸에 세지 않는다**(H3).
    tally_.strokes.add(src.origin());
    tally_.addTiles(src.origin(), changedTiles);
}

Result<void> SiganRecorder::onRegionOp(const StrokeSource& src, const agent::RegionOpRecord& op) {
    if (op.area.isEmpty()) {
        return Err("영역이 비었다 — 일어나지 않은 연산을 기록하지 않는다",
                   ErrorCode::InvalidArgument);
    }
    if (broken_) {
        return Err("기록이 고장 난 상태다 — 기록 없이 그리지 않는다(docs/06 결정 ④)",
                   ErrorCode::IoError);
    }

    // 🔴 결정 ① — 합성 프레임 **쌍**으로 남긴다.
    //    두 점이 사각형을 정하므로 "붓질이 아님"과 "얼마나 넓게"가 둘 다
    //    서명될 프레임 바이트 안에 들어간다. 새 필드는 만들지 않았다.
    //    필압·기울기·회전·속도는 **0 고정**이다 — 없던 필압을 지어내지 않는다.
    const u32 base = op.eraser ? (agent::recordFlags(agent::RecordFlag::Synthetic) |
                                  agent::RecordFlag::Eraser)
                               : agent::recordFlags(agent::RecordFlag::Synthetic);

    sigan::StrokeSample a(src);
    a.layerId = op.layerId;
    a.pos = PointF{static_cast<f32>(op.area.x), static_cast<f32>(op.area.y)};
    a.flags = base | agent::RecordFlag::Down;
    pub_->publish(a);

    sigan::StrokeSample b(src);
    b.layerId = op.layerId;
    b.pos = PointF{static_cast<f32>(op.area.right() - 1), static_cast<f32>(op.area.bottom() - 1)};
    b.flags = base | agent::RecordFlag::Up;
    pub_->publish(b);

    // 획과 **같은** 복구 단위를 갖게 한다(획 끝마다 flush).
    pub_->endStroke();

    // 부연. 여기 없어도 사실은 산다 — 영역도 "붓질 아님"도 이미 프레임 안에 있다.
    // 🔴 하중을 받는 사실을 Note 에 두지 않는다(docs/06 6절 H4 · docs/03 8절 `:src` 사고).
    pub_->journal().appendNote(std::string("{\"op\":\"") + agent::regionOpKindName(op.kind) +
                               "\",\"layer\":" + std::to_string(op.layerId) +
                               ",\"tiles\":" + std::to_string(op.changedTiles) + "}");

    checkJournal();
    if (broken_) {
        // 🔴 저널이 여기서 깨졌다 = 픽셀은 바뀌었는데 정본에 흔적이 없다.
        //    호출자가 롤백해야 하므로 실패를 그대로 돌려준다(docs/06 결정 ④).
        return Err("저널에 기록하지 못했다 — 이 연산을 롤백해라(docs/06 결정 ④)",
                   ErrorCode::IoError);
    }
    // 축 B(영역 연산 수) · 축 C(변경 타일 수).
    tally_.regionOps.add(src.origin());
    tally_.addTiles(src.origin(), op.changedTiles);
    return Ok();
}

Result<std::string> SiganRecorder::buildProofLog() const {
    // 저널이 정본이다. 방금 쓴 것까지 파일로 밀어내고 그 파일을 다시 읽는다 —
    // 메모리의 집계가 아니라 **기록된 것**에서 요약을 뽑기 위해서다.
    pub_->journal().flush();
    Result<sigan::JournalScan> scan = sigan::recoverJournal(journalPath_);
    if (!scan.ok()) {
        return scan.error();
    }
    sigan::ProofLogInput in;
    in.app = app_;
    in.sessionId = sessionId_;
    in.wallAnchorUnixMs = pub_->clock().wallAnchorUnixMs();
    in.scan = &scan.value();
    in.agents = agents_;
    // 축 C 는 프레임에 자리가 없는 값이라 기록한 쪽이 건넨다.
    // 축 A·B 는 prooflog 가 **프레임에서 직접** 센다 — 우리가 넘기지 않는다.
    in.changedTilesByOrigin = tally_.changedTiles;
    return sigan::buildProofLog(in);
}

// ── SiganRecorderFactory ─────────────────────────────────────────────────

SiganRecorderFactory::SiganRecorderFactory(RecordingConfig cfg) : cfg_(std::move(cfg)) {}

SiganRecorderFactory::~SiganRecorderFactory() {
    // 살아 있는 구간이 공장보다 오래 살면 dangling 이 된다. 관찰만 끊어 둔다.
    for (SiganRecorder* r : live_) {
        r->observeBy(nullptr);
    }
}

Result<std::unique_ptr<agent::IStrokeRecorder>>
SiganRecorderFactory::openForDocument(std::string_view hint) {
    std::filesystem::path dir =
        cfg_.journalDir.empty() ? std::filesystem::temp_directory_path()
                                : std::filesystem::path(cfg_.journalDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec && !std::filesystem::exists(dir)) {
        return Err("저널 디렉터리를 만들 수 없다: " + dir.string(), ErrorCode::IoError);
    }
    const u64 segment = nextSegment_++;
    // 힌트가 경로면 파일 이름만 쓴다(경로를 그대로 파일 이름에 넣지 않는다).
    const std::filesystem::path hp{std::string(hint)};
    const std::string tag = sanitize(hp.filename().string());
    const std::string path =
        (dir / ("mari-" + tag + "-" + std::to_string(segment) + ".jrnl")).string();

    Result<std::unique_ptr<SiganRecorder>> rec = SiganRecorder::open(cfg_, path, segment);
    if (!rec.ok()) {
        return rec.error();
    }
    SiganRecorder* raw = rec.value().get();
    raw->observeBy(this);
    journals_.push_back(path);
    live_.push_back(raw);
    return Ok(std::unique_ptr<agent::IStrokeRecorder>(std::move(rec).value().release()));
}

void SiganRecorderFactory::retire(SiganRecorder* r) noexcept {
    const auto it = std::find(live_.begin(), live_.end(), r);
    if (it == live_.end()) {
        return;
    }
    // 구간이 닫혀도 숫자는 사라지지 않는다. 합쳐서 들고 있는다.
    const agent::RecordingTally& t = r->tally();
    for (usize i = 0; i < kStrokeOriginSlots; ++i) {
        const StrokeOrigin o = static_cast<StrokeOrigin>(static_cast<u8>(i));
        closed_.strokes.add(o, t.strokes.count(o));
        closed_.regionOps.add(o, t.regionOps.count(o));
        closed_.changedTiles[i] += t.changedTiles[i];
    }
    live_.erase(it);
}

agent::RecordingTally SiganRecorderFactory::totalTally() const noexcept {
    agent::RecordingTally out = closed_;
    for (const SiganRecorder* r : live_) {
        const agent::RecordingTally& t = r->tally();
        for (usize i = 0; i < kStrokeOriginSlots; ++i) {
            const StrokeOrigin o = static_cast<StrokeOrigin>(static_cast<u8>(i));
            out.strokes.add(o, t.strokes.count(o));
            out.regionOps.add(o, t.regionOps.count(o));
            out.changedTiles[i] += t.changedTiles[i];
        }
    }
    return out;
}

SiganRecorder* SiganRecorderFactory::liveAt(usize i) const noexcept {
    return i < live_.size() ? live_[i] : nullptr;
}

Result<std::string> proofLogFromJournal(const std::string& journalPath,
                                        const RecordingConfig& cfg) {
    Result<sigan::JournalScan> scan = sigan::recoverJournal(journalPath);
    if (!scan.ok()) {
        return scan.error();
    }
    sigan::ProofLogInput in;
    in.app = cfg.app;
    in.sessionId = scan.value().sessionId;
    in.wallAnchorUnixMs = scan.value().wallAnchorUnixMs;
    in.scan = &scan.value();
    in.agents = cfg.agents;
    // changedTilesByOrigin 은 nullptr 그대로 둔다 — 안 잰 것을 잰 척하지 않는다.
    return sigan::buildProofLog(in);
}

} // namespace mari::record
