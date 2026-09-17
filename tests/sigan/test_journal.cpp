// docs/03 5.4 / 10절 `mari_crash_survives`
//   — 저널을 중간에서 잘라내고 복구 → **마지막 완성된 획까지** 살아난다.
#include <mari/sigan/journal.hpp>
#include <mari/sigan/publisher.hpp>
#include <mari/test/harness.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace mari;
using namespace mari::sigan;

namespace {

std::string tmpPath(const char* stem) {
    auto p = std::filesystem::temp_directory_path() / (std::string("mari_sigan_") + stem + ".jrnl");
    return p.string();
}

StrokeFrame makeFrame(u64 seq, u32 flags) {
    StrokeFrame f;
    f.seq = seq;
    f.tMs = static_cast<f64>(seq) * 8.0;
    f.cx = static_cast<f32>(seq);
    f.cy = static_cast<f32>(seq) * 2.0f;
    f.pressure = 0.5f;
    f.layerId = 3;
    f.brushId = 11;
    f.flags = flags;
    return f;
}

/// 파일 꼬리를 n 바이트 잘라낸다 = 전원이 나간 상황.
void truncateFile(const std::string& path, usize bytes) {
    const auto size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size > bytes ? size - bytes : 0);
}

} // namespace

MARI_TEST(journal_roundtrip_has_no_seq_gaps) {
    const std::string path = tmpPath("roundtrip");
    {
        auto j = Journal::create(path, 42, 1'700'000'000'000);
        CHECK(j.ok());
        auto& journal = *j.value();
        for (u64 s = 1; s <= 500; ++s) {
            CHECK(journal.appendFrame(makeFrame(s, frameFlags(FrameFlag::Move))));
        }
        CHECK(journal.markStrokeEnd(500));
    }
    auto scan = Journal::scan(path);
    CHECK(scan.ok());
    CHECK_EQ(scan.value().frames.size(), static_cast<usize>(500));
    CHECK_EQ(scan.value().sessionId, 42ull);
    CHECK_EQ(scan.value().wallAnchorUnixMs, 1'700'000'000'000);
    CHECK_EQ(scan.value().seqGaps().size(), static_cast<usize>(0));
    CHECK_EQ(scan.value().lastCompleteSeq, 500ull);
    CHECK(!scan.value().truncated);
    std::filesystem::remove(path);
}

MARI_TEST(mari_crash_survives_truncated_tail) {
    const std::string path = tmpPath("crash");
    {
        auto j = Journal::create(path, 7, 1);
        CHECK(j.ok());
        auto& journal = *j.value();
        // 획 1: seq 1..10, 끝까지 완성
        for (u64 s = 1; s <= 10; ++s) {
            journal.appendFrame(makeFrame(s, s == 1   ? frameFlags(FrameFlag::Down)
                                             : s == 10 ? frameFlags(FrameFlag::Up)
                                                       : frameFlags(FrameFlag::Move)));
        }
        journal.markStrokeEnd(10);
        // 획 2: 그리는 도중 전원이 나갔다 — up 도 flush 도 없다
        for (u64 s = 11; s <= 20; ++s) {
            journal.appendFrame(makeFrame(s, frameFlags(FrameFlag::Move)));
        }
        journal.flush();
    }
    // 마지막 레코드를 반토막 낸다.
    truncateFile(path, 20);

    auto scan = Journal::scan(path);
    CHECK(scan.ok());
    CHECK(scan.value().truncated);            // 조용히 넘어가지 않는다
    CHECK(scan.value().frames.size() >= 10);  // 완성된 획은 전부 살아 있다
    CHECK_EQ(scan.value().lastCompleteSeq, 10ull);

    // 복구는 **마지막 완성 획까지**만 인정한다.
    auto recovered = recoverJournal(path);
    CHECK(recovered.ok());
    CHECK_EQ(recovered.value().frames.size(), static_cast<usize>(10));
    CHECK_EQ(recovered.value().frames.back().seq, 10ull);
    CHECK_EQ(recovered.value().seqGaps().size(), static_cast<usize>(0));
    std::filesystem::remove(path);
}

MARI_TEST(journal_detects_bit_rot_and_stops) {
    const std::string path = tmpPath("crc");
    {
        auto j = Journal::create(path, 1, 1);
        CHECK(j.ok());
        for (u64 s = 1; s <= 5; ++s) {
            j.value()->appendFrame(makeFrame(s, frameFlags(FrameFlag::Move)));
        }
        j.value()->markStrokeEnd(5);
    }
    // 세 번째 레코드의 페이로드 한 바이트를 뒤집는다.
    {
        std::FILE* fp = std::fopen(path.c_str(), "r+b");
        CHECK(fp != nullptr);
        const long off = static_cast<long>(kJournalHeaderSize) + 2 * 65 + 10;
        std::fseek(fp, off, SEEK_SET);
        int c = std::fgetc(fp);
        std::fseek(fp, off, SEEK_SET);
        std::fputc(c ^ 0xFF, fp);
        std::fclose(fp);
    }
    auto scan = Journal::scan(path);
    CHECK(scan.ok());
    CHECK(scan.value().truncated);
    CHECK_EQ(scan.value().frames.size(), static_cast<usize>(2)); // 깨진 레코드에서 멈춘다
    std::filesystem::remove(path);
}

MARI_TEST(journal_framesAfter_returns_backlog) {
    const std::string path = tmpPath("backlog");
    auto j = Journal::create(path, 1, 1);
    CHECK(j.ok());
    for (u64 s = 1; s <= 30; ++s) {
        j.value()->appendFrame(makeFrame(s, frameFlags(FrameFlag::Move)));
    }
    auto rest = j.value()->framesAfter(20);
    CHECK(rest.ok());
    CHECK_EQ(rest.value().size(), static_cast<usize>(10));
    CHECK_EQ(rest.value().front().seq, 21ull);
    j.value().reset();
    std::filesystem::remove(path);
}

MARI_TEST_MAIN()
