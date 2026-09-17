// 기반 타입 테스트 — 좌표계 규약과 기하 연산이 약속대로 도는지 본다.
#include <mari/core/types.hpp>
#include <mari/test/harness.hpp>

using namespace mari;

MARI_TEST(tile_size_is_64) { CHECK_EQ(kTileSize, 64); }

MARI_TEST(rect_is_half_open) {
    const Rect r{10, 20, 30, 40};
    CHECK_EQ(r.right(), 40);
    CHECK_EQ(r.bottom(), 60);
    CHECK(r.contains(Point{10, 20}));   // 좌상단은 포함
    CHECK(!r.contains(Point{40, 20}));  // 오른쪽 경계는 배타
    CHECK(!r.contains(Point{10, 60}));  // 아래 경계는 배타
    CHECK(r.contains(Point{39, 59}));
}

MARI_TEST(rect_intersect_and_unite) {
    const Rect a{0, 0, 10, 10};
    const Rect b{5, 5, 10, 10};
    CHECK(a.intersects(b));
    CHECK_EQ(a.intersected(b), (Rect{5, 5, 5, 5}));
    CHECK_EQ(a.united(b), (Rect{0, 0, 15, 15}));

    const Rect far{100, 100, 1, 1};
    CHECK(!a.intersects(far));
    CHECK(a.intersected(far).isEmpty());
    CHECK_EQ(a.united(Rect{}), a); // 빈 사각형과의 합집합은 자기 자신
    CHECK_EQ(Rect{}.united(a), a);
}

MARI_TEST(rect_from_bounds_roundtrip) {
    const Rect r = Rect::fromBounds(3, 4, 13, 24);
    CHECK_EQ(r.x, 3);
    CHECK_EQ(r.y, 4);
    CHECK_EQ(r.width, 10);
    CHECK_EQ(r.height, 20);
}

// 캔버스 좌표는 좌상단 원점, y 아래로 증가한다.
// 즉 y가 커질수록 타일 인덱스도 커져야 한다 — 뒤집힌 좌표계가 아니다.
MARI_TEST(canvas_y_grows_downward) {
    CHECK(tileIndexFor(0) < tileIndexFor(64));
    CHECK_EQ(tileIndexFor(0), 0);
    CHECK_EQ(tileIndexFor(63), 0);
    CHECK_EQ(tileIndexFor(64), 1);
    CHECK_EQ(tileIndexFor(129), 2);
    // 음수는 아래로 내림한다
    CHECK_EQ(tileIndexFor(-1), -1);
    CHECK_EQ(tileIndexFor(-64), -1);
    CHECK_EQ(tileIndexFor(-65), -2);
    CHECK_EQ(tileOrigin(0), 0);
    CHECK_EQ(tileOrigin(-2), -128);
}

MARI_TEST(pixel_format_sizes) {
    CHECK_EQ(bytesPerPixel(PixelFormat::RGBA8), 4u);
    CHECK_EQ(bytesPerPixel(PixelFormat::RGBA16), 8u);
    CHECK_EQ(bytesPerPixel(PixelFormat::RGBA32F), 16u);
    CHECK_EQ(bytesPerPixel(PixelFormat::Gray8), 1u);
    CHECK_EQ(bytesPerPixel(PixelFormat::Unknown), 0u);
    CHECK_EQ(channelCount(PixelFormat::Gray16), 1);
    CHECK_EQ(channelCount(PixelFormat::RGBA8), 4);
}

MARI_TEST(color8_argb_roundtrip) {
    const Color8 c = Color8::rgba(0x12, 0x34, 0x56, 0x78);
    CHECK_EQ(c.toArgb32(), 0x78123456u);
    const Color8 d = Color8::fromArgb32(0x78123456u);
    CHECK(d == c);
    CHECK_EQ(Color8{}.a, 255); // 기본은 불투명
}

MARI_TEST(blend_mode_names_are_stable) {
    // 이름은 .ora 직렬화에 쓰인다. 바뀌면 파일 호환이 깨진다.
    CHECK_EQ(std::string(blendModeName(BlendMode::Normal)), std::string("normal"));
    CHECK_EQ(std::string(blendModeName(BlendMode::ColorDodge)), std::string("color-dodge"));
    CHECK_EQ(std::string(blendModeName(BlendMode::Luminosity)), std::string("luminosity"));
    CHECK_EQ(static_cast<int>(BlendMode::Normal), 0); // 0은 영원히 Normal
}

MARI_TEST(size_helpers) {
    CHECK((Size{}.isEmpty()));
    CHECK((Size{10, 0}.isEmpty()));
    CHECK(!(Size{1, 1}.isEmpty()));
    CHECK_EQ((Size{4000, 4000}.area()), 16000000LL); // i64 라 오버플로하지 않는다
}

MARI_TEST_MAIN()
