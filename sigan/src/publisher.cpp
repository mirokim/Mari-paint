// Mari Paint — Sigan 증거 발행기 (docs/03 4.2 · 5.2 · 5.3 · 5.4)
#include <mari/sigan/publisher.hpp>

#include <utility>

namespace mari::sigan {

SiganPublisher::~SiganPublisher() = default;

Result<PublisherPtr> SiganPublisher::open(PublisherConfig cfg, SessionClock clock) {
    if (cfg.journalPath.empty()) {
        return Err("저널 경로가 비었다 — 저널 없이는 발행하지 않는다",
                   ErrorCode::InvalidArgument);
    }
    PublisherPtr p(new SiganPublisher());
    p->clock_ = clock;
    p->sessionId_ =
        cfg.sessionId != 0 ? cfg.sessionId
                           : static_cast<u64>(clock.wallAnchorUnixMs()) ^ clock.steadyOriginNs();
    auto journal = Journal::create(cfg.journalPath, p->sessionId_, clock.wallAnchorUnixMs());
    if (!journal) {
        return journal.error();
    }
    p->journal_ = std::move(journal).value();
    p->cfg_ = std::move(cfg);
    // 싱크가 없어도 기본 능력은 전부 켜 둔다 — 저널은 항상 완전한 정본이다.
    p->negotiated_.proto = kProtoVersion;
    p->negotiated_.caps = {caps::kCanvasXy, caps::kLayer, caps::kBrush, caps::kView,
                           caps::kResume};
    return Ok(std::move(p));
}

void SiganPublisher::setSink(ISiganSink* sink) noexcept { sink_ = sink; }

Handshake SiganPublisher::localHandshake() const {
    Handshake h;
    h.proto = kProtoVersion;
    h.app = cfg_.app;
    h.caps = {caps::kCanvasXy, caps::kLayer, caps::kBrush, caps::kView, caps::kResume};
    h.sessionId = sessionId_; // 🔴 재연결해도 같은 세션이다 — 새 Segment 가 아니다
    h.resumeFromSeq = sentSeq_ + 1 <= seq_ ? sentSeq_ + 1 : 0; // 밀린 구간을 알린다
    h.wallAnchorUnixMs = clock_.wallAnchorUnixMs();
    return h;
}

void SiganPublisher::applyCaps(StrokeFrame& f) const noexcept {
    // 규칙 3(docs/03 5.5): 상대가 모르는 능력은 **필드를 비울 뿐 연결을 끊지 않는다.**
    // 프레임은 고정 길이라 "생략" = 0 이다. 저널의 정본은 손대지 않는다.
    if (!capLayer_) {
        f.layerId = 0;
    }
    if (!capBrush_) {
        f.brushId = 0;
    }
    // 🔴 origin 은 능력 협상 대상이 **아니다.** 상대가 모른다고 비우면
    //    AI 획이 출처 없는 획이 된다 — 그게 바로 막으려던 구멍이다.
    //    고정 길이 프레임이라 구버전 Sigan 은 꼬리 8바이트를 그냥 무시하면 된다.
}

u64 SiganPublisher::publish(const StrokeSample& s) noexcept {
    ++seq_;
    StrokeFrame f;
    f.seq = seq_;
    f.tMs = clock_.elapsedMs(); // 단조 시계. 벽시계는 보지 않는다(docs/03 5.6)
    f.cx = s.pos.x;
    f.cy = s.pos.y;
    f.pressure = s.pressure;
    f.tiltX = s.tiltX;
    f.tiltY = s.tiltY;
    f.rotation = s.rotation;
    f.velocity = s.velocity;
    bool truncated = false;
    f.layerId = narrowId(s.layerId, truncated);
    f.brushId = narrowId(s.brushId, truncated);
    if (truncated) {
        ++stats_.idTruncations; // 조용히 넘어가지 않는다
    }
    f.flags = s.flags;
    // 🔴 출처는 여기서 프레임에 들어간다. 호출자가 고른 게 아니라 StrokeSource 가
    //    들고 온 값이고, StrokeSource 는 바깥에서 origin 을 지정할 길이 없다.
    //    프레임 안이라는 것이 핵심이다 — 서명 밖이면 사후에 고칠 수 있어 무의미하다.
    f.origin = s.source.origin();
    f.agentId = s.source.agentDigest();
    if (hasFlag(f.flags, FrameFlag::Down)) {
        // 🔴 붓질과 영역 연산을 **다른 칸**에 센다(docs/06 결정 ② · 6절 H3).
        //    합치면 캔버스 전체를 칠한 fill 이 붓질 한 번으로 보인다.
        if (hasFlag(f.flags, FrameFlag::Synthetic)) {
            stats_.regionOps.add(f.origin);
        } else {
            stats_.origins.add(f.origin);
        }
    }

    // 🔴 저널이 먼저다. 파이프가 어떤 상태든 정본은 남는다. 드롭은 없다(docs/03 4.2).
    journal_->appendFrame(f);
    ++stats_.published;

    // 순서가 이미 밀렸으면(sentSeq_+1 != seq) 새 프레임을 앞질러 보내지 않는다.
    // 밀린 구간은 pump() 가 순서대로 밀어넣는다.
    if (sink_ != nullptr && sink_->connected() && sentSeq_ + 1 == seq_) {
        StrokeFrame wire = f;
        applyCaps(wire);
        encodeFrame(wire, wire_);
        if (sink_->write(wire_, kFrameSize) == SinkStatus::Ok) {
            sentSeq_ = seq_;
            ++stats_.sent;
        }
    }
    stats_.spooled = seq_ - sentSeq_; // 저널에만 있는 프레임. **버린 게 아니다**
    return seq_;
}

void SiganPublisher::endStroke() noexcept {
    // 획 끝마다 flush (docs/03 5.4). 매 점 fsync 는 하지 않는다.
    journal_->markStrokeEnd(seq_);
    if (sink_ != nullptr && sink_->connected()) {
        sink_->pump();
    }
}

Result<void> SiganPublisher::resendBacklog() {
    if (sink_ == nullptr || !sink_->connected() || sentSeq_ >= seq_) {
        return Ok();
    }
    auto pending = journal_->framesAfter(sentSeq_);
    if (!pending) {
        return pending.error();
    }
    for (const auto& f : pending.value()) {
        StrokeFrame wire = f;
        applyCaps(wire);
        encodeFrame(wire, wire_);
        if (sink_->write(wire_, kFrameSize) != SinkStatus::Ok) {
            break; // 아직 못 민다. 저널에 남아 있으니 다음 pump() 에서 이어간다
        }
        sentSeq_ = f.seq;
        ++stats_.sent;
        ++stats_.resent;
    }
    stats_.spooled = seq_ - sentSeq_;
    return Ok();
}

Result<void> SiganPublisher::pump() {
    if (sink_ == nullptr) {
        return Ok(); // 로컬 모드. 저널만으로 충분하다(docs/03 6절)
    }
    if (!sink_->connected()) {
        auto connected = sink_->connect();
        if (!connected) {
            return connected.error(); // 다음 pump() 에서 다시 시도한다. 그리기는 계속된다
        }
        const Handshake local = localHandshake();
        auto remote = sink_->handshake(local);
        ++stats_.reconnects;
        if (remote) {
            negotiated_ = negotiate(local, remote.value());
            capLayer_ = negotiated_.hasCap(caps::kLayer);
            capBrush_ = negotiated_.hasCap(caps::kBrush);
            // 상대가 "seq N 부터 달라"고 하면 그 말을 따른다.
            // 반토막 난 프레임까지 확실히 다시 간다 — 중복은 괜찮고 **구멍은 안 된다.**
            if (remote.value().resumeFromSeq > 0) {
                const u64 from = remote.value().resumeFromSeq - 1;
                if (from < sentSeq_) {
                    sentSeq_ = from;
                }
            }
            journal_->appendNote("재핸드셰이크: proto=" + std::to_string(negotiated_.proto) +
                                 " seq 연속 유지(같은 세션)");
        } else {
            // 답이 없어도 끊지 않는다. 능력 미상 = 전부 보낸다(고정 길이라 상대가 무시하면 그만).
            capLayer_ = true;
            capBrush_ = true;
            journal_->appendNote("핸드셰이크 무응답: 최소 능력으로 계속 기록한다");
        }
    } else {
        sink_->pump();
    }
    return resendBacklog();
}

Result<JournalScan> recoverJournal(const std::string& path) {
    auto scanned = Journal::scan(path);
    if (!scanned) {
        return scanned;
    }
    JournalScan scan = std::move(scanned).value();
    // 🔴 마지막 **완성된 획**까지만 복구한다. 반쯤 그려진 획을 증거로 내밀지 않는다.
    if (scan.completeCount < scan.frames.size()) {
        scan.frames.resize(scan.completeCount);
        scan.truncated = true;
    }
    return Ok(std::move(scan));
}

} // namespace mari::sigan
