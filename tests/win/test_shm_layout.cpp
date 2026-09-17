// Mari Paint — .8bf 호스트 공유 메모리 레이아웃 테스트
//
// 남의 코드(.8bf)와 주고받는 버퍼의 크기 계산을 검증한다.
// 🔴 여기서 오버플로가 새면 신뢰할 수 없는 플러그인이 우리 힙을 밟는다.
//    그래서 x86 호스트/x64 본체가 **같은 바이트**를 본다는 것도 같이 못박는다.
#include <mari/host8bf/protocol.hpp>
#include <mari/test/harness.hpp>

using namespace mari;
using namespace mari::host8bf;

MARI_TEST(header_layout_is_pinned) {
    // 이 값이 바뀌면 x86 호스트와 x64 본체가 서로 다른 구조체를 보게 된다.
    CHECK_EQ(sizeof(ShmHeader), static_cast<usize>(76));
    CHECK_EQ(offsetof(ShmHeader, magic), static_cast<usize>(0));
    CHECK_EQ(offsetof(ShmHeader, pixelOffset), static_cast<usize>(24));
    CHECK_EQ(offsetof(ShmHeader, pixelBytes), static_cast<usize>(32));
    CHECK_EQ(offsetof(ShmHeader, selLeft), static_cast<usize>(40));
    CHECK_EQ(offsetof(ShmHeader, status), static_cast<usize>(56));
    CHECK_EQ(offsetof(ShmHeader, cancel), static_cast<usize>(64));
    // 🔴 타임아웃 강제 종료가 이 필드에 달려 있다(docs/02 6.2).
    CHECK_EQ(offsetof(ShmHeader, hostPid), static_cast<usize>(68));
}

MARI_TEST(pixel_data_is_aligned) {
    // SSE/AVX 로 읽는 필터가 있다. 픽셀 시작이 64바이트 경계여야 한다.
    CHECK_EQ(pixelOffsetFor() % kPixelAlign, static_cast<u64>(0));
    CHECK(pixelOffsetFor() >= sizeof(ShmHeader));
}

MARI_TEST(bytes_per_pixel) {
    CHECK_EQ(bytesPerPixelOf(ShmPixelFormat::RGBA8), 4u);
    CHECK_EQ(bytesPerPixelOf(ShmPixelFormat::RGB8), 3u);
    CHECK_EQ(bytesPerPixelOf(ShmPixelFormat::Gray8), 1u);
    CHECK_EQ(bytesPerPixelOf(ShmPixelFormat::RGBA16), 8u);
}

MARI_TEST(layout_for_normal_image) {
    auto r = computeLayout(ShmPixelFormat::RGBA8, 1920, 1080);
    CHECK(r.ok());
    const ShmLayout& l = r.value();
    CHECK_EQ(l.rowBytes, 1920u * 4u);
    CHECK_EQ(l.pixelBytes, static_cast<u64>(1920) * 1080 * 4);
    CHECK_EQ(l.totalBytes, l.pixelOffset + l.pixelBytes);
}

MARI_TEST(layout_rejects_zero) {
    CHECK(!computeLayout(ShmPixelFormat::RGBA8, 0, 100).ok());
    CHECK(!computeLayout(ShmPixelFormat::RGBA8, 100, 0).ok());
    CHECK_EQ(computeLayout(ShmPixelFormat::RGBA8, 0, 100).code(), ErrorCode::InvalidArgument);
}

MARI_TEST(layout_rejects_overflow) {
    // 0xFFFFFFFF × 8바이트 = 32비트 스트라이드를 넘는다.
    auto r = computeLayout(ShmPixelFormat::RGBA16, 0xFFFFFFFFu, 2);
    CHECK(!r.ok());

    // 65536 × 65536 RGBA = 16GB. 3GB 상한에 걸려야 한다.
    auto r2 = computeLayout(ShmPixelFormat::RGBA8, 65536, 65536);
    CHECK(!r2.ok());
    CHECK_EQ(r2.code(), ErrorCode::InvalidArgument);

    // 그 바로 아래는 통과한다(경계에서 과하게 막지 않는다).
    auto r3 = computeLayout(ShmPixelFormat::RGBA8, 8192, 8192);
    CHECK(r3.ok());
}

namespace {

/// 정상 헤더 하나를 만든다.
ShmHeader makeHeader(u32 w, u32 h, ShmPixelFormat fmt, u64& mapped) {
    const auto l = computeLayout(fmt, w, h).value();
    ShmHeader hd{};
    hd.magic = kShmMagic;
    hd.version = kShmVersion;
    hd.format = static_cast<u32>(fmt);
    hd.width = w;
    hd.height = h;
    hd.rowBytes = l.rowBytes;
    hd.pixelOffset = l.pixelOffset;
    hd.pixelBytes = l.pixelBytes;
    hd.selRight = static_cast<i32>(w);
    hd.selBottom = static_cast<i32>(h);
    mapped = l.totalBytes;
    return hd;
}

} // namespace

MARI_TEST(validate_accepts_good_header) {
    u64 mapped = 0;
    const ShmHeader h = makeHeader(640, 480, ShmPixelFormat::RGBA8, mapped);
    CHECK(validateHeader(h, mapped).ok());
}

MARI_TEST(validate_rejects_hostile_headers) {
    u64 mapped = 0;
    const ShmHeader good = makeHeader(640, 480, ShmPixelFormat::RGBA8, mapped);

    // 매직이 틀림 — 남이 만든 섹션을 열었다.
    ShmHeader h = good;
    h.magic = 0xDEADBEEFu;
    CHECK(!validateHeader(h, mapped).ok());

    // 버전이 다름 — 본체만 갱신하고 호스트를 안 바꾼 상황.
    h = good;
    h.version = kShmVersion + 1u;
    CHECK_EQ(validateHeader(h, mapped).code(), ErrorCode::Unsupported);

    // 픽셀 영역이 매핑 밖 — **고전적인 힙 오버런 시도다.**
    h = good;
    h.pixelBytes = mapped * 4u;
    CHECK(!validateHeader(h, mapped).ok());

    // 스트라이드가 폭보다 작다.
    h = good;
    h.rowBytes = 4u;
    CHECK(!validateHeader(h, mapped).ok());

    // 픽셀 오프셋이 헤더를 침범한다.
    h = good;
    h.pixelOffset = 4u;
    CHECK(!validateHeader(h, mapped).ok());

    // 선택 영역이 버퍼 밖.
    h = good;
    h.selRight = 100000;
    CHECK(!validateHeader(h, mapped).ok());

    h = good;
    h.selLeft = -1;
    CHECK(!validateHeader(h, mapped).ok());

    // 뒤집힌 사각형.
    h = good;
    h.selTop = 100;
    h.selBottom = 10;
    CHECK(!validateHeader(h, mapped).ok());
}

MARI_TEST(status_names_are_present) {
    // 리포트에 그대로 나가는 문자열이다. 비어 있으면 사용자가 뭘 봐야 할지 모른다.
    const HostStatus all[] = {HostStatus::Ok,
                              HostStatus::PluginLoadFailed,
                              HostStatus::PluginRefused,
                              HostStatus::PluginCrashed,
                              HostStatus::TimedOut,
                              HostStatus::UnsupportedFormat,
                              HostStatus::ShmError,
                              HostStatus::Cancelled,
                              HostStatus::ArchMismatch};
    for (HostStatus s : all) {
        const char* n = hostStatusName(s);
        CHECK(n != nullptr);
        CHECK(n[0] != '\0');
    }
}

MARI_TEST_MAIN()
