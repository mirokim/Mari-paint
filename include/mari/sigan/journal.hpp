// Mari Paint — 로컬 append-only 저널 (docs/03 4.2 · 5.4)
//
// 🔴 이게 정본이다. 파이프가 막히든 Sigan 이 죽든, 프레임은 여기 전부 남는다.
//    드롭 카운터는 없다. seq 는 끊기지 않는다.
//
// 크래시 안전: append-only, 획 끝(up)마다 flush.
//   매 점 fsync 는 하지 않는다 — 16ms 예산을 깬다(docs/03 5.4).
//   레코드마다 CRC32 를 붙여서, 전원이 나가 꼬리가 잘려도
//   **마지막 완성된 획까지** 복구할 수 있다.
#ifndef MARI_SIGAN_JOURNAL_HPP
#define MARI_SIGAN_JOURNAL_HPP

#include <mari/core/result.hpp>
#include <mari/sigan/frame.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace mari::sigan {

/// 저널 파일 시그니처. 버전은 뒤에만 더한다.
inline constexpr char kJournalMagic[8] = {'M', 'A', 'R', 'I', 'J', 'R', 'N', 'L'};
inline constexpr u32 kJournalVersion = 1;
inline constexpr usize kJournalHeaderSize = 32;

/// 레코드 종류. 값은 직렬화된다 — **뒤에만 더해라.**
enum class JournalRecord : u8 {
    Frame = 1,     ///< StrokeFrame 56B
    StrokeEnd = 2, ///< 획 하나가 끝났다(seq 8B). 여기까지가 "완성된 획"이다
    Note = 3,      ///< UTF-8 메모(협상 결과·재연결 등). 증거가 아니라 기록이다
};

/// 저널을 훑은 결과. 복구는 이걸 보고 한다.
struct JournalScan {
    u32 version = 0;
    u64 sessionId = 0;
    i64 wallAnchorUnixMs = 0;
    std::vector<StrokeFrame> frames; ///< CRC 를 통과한 프레임 전부
    usize completeCount = 0;         ///< 마지막 StrokeEnd 까지의 프레임 개수
    u64 lastCompleteSeq = 0;         ///< 마지막으로 끝난 획의 seq
    bool truncated = false;          ///< 꼬리가 잘렸다(크래시) — 정직하게 표시한다
    std::vector<std::string> notes;

    /// seq 구멍 목록. **비어 있어야 정상이다.**
    [[nodiscard]] std::vector<u64> seqGaps() const;
};

/// append-only 저널 파일.
class Journal {
public:
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    /// 새 저널을 연다(있으면 덮어쓴다). 세션 시작 시 한 번.
    static Result<std::unique_ptr<Journal>> create(const std::string& path, u64 sessionId,
                                                   i64 wallAnchorUnixMs);

    /// 프레임을 덧붙인다. **핫 패스** — noexcept, 할당 없음(버퍼는 미리 잡는다).
    /// 실패해도 예외를 던지지 않고 failed() 로 남긴다.
    bool appendFrame(const StrokeFrame& f) noexcept;

    /// 획이 끝났다고 표시하고 flush 한다(docs/03 5.4). 핫 패스에서 불려도 된다.
    bool markStrokeEnd(u64 seq) noexcept;

    /// 메모를 남긴다. 저빈도.
    bool appendNote(const std::string& text) noexcept;

    /// 버퍼를 파일로 밀어낸다. fsync 는 하지 않는다.
    bool flush() noexcept;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] u64 frameCount() const noexcept { return frameCount_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    /// 저널 파일을 훑는다. 잘린 꼬리는 버리고 truncated 를 세운다.
    static Result<JournalScan> scan(const std::string& path);

    /// `afterSeq` 초과 프레임만 읽어온다. 재연결 후 밀어넣기용(docs/03 5.3).
    /// 내부 버퍼를 먼저 flush 하므로 방금 쓴 프레임도 들어온다.
    Result<std::vector<StrokeFrame>> framesAfter(u64 afterSeq);

private:
    Journal() = default;
    bool writeRecord(JournalRecord tag, const u8* payload, u32 len) noexcept;

    std::FILE* fp_ = nullptr;
    std::string path_;
    std::vector<u8> buf_;  ///< 미리 잡아 둔 고정 버퍼. 핫 패스에서 재할당하지 않는다
    usize bufLen_ = 0;
    u64 frameCount_ = 0;
    bool failed_ = false;
};

using JournalPtr = std::unique_ptr<Journal>;

} // namespace mari::sigan

#endif // MARI_SIGAN_JOURNAL_HPP
