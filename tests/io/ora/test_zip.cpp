// Mari Paint — ZIP 컨테이너 테스트.
// 핵심은 두 가지다: (1) 우리가 쓴 걸 우리가 읽는다, (2) mimetype 이 **바이트 수준에서**
// 무압축 첫 항목이다. (2)를 어기면 파일 유틸리티가 .ora 를 알아보지 못한다.
#include <mari/ora/zip.hpp>
#include <mari/test/harness.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace mari;
using namespace mari::ora;

namespace {

u16 get16(const std::vector<u8>& b, usize p) {
    return static_cast<u16>(b[p] | (static_cast<u16>(b[p + 1]) << 8));
}
u32 get32(const std::vector<u8>& b, usize p) {
    return static_cast<u32>(b[p]) | (static_cast<u32>(b[p + 1]) << 8) |
           (static_cast<u32>(b[p + 2]) << 16) | (static_cast<u32>(b[p + 3]) << 24);
}
std::string textAt(const std::vector<u8>& b, usize p, usize n) {
    return std::string(reinterpret_cast<const char*>(b.data()) + p, n);
}

/// u8 버퍼와 문자열을 바이트로 비교한다. char 는 부호가 있어 >=0x80 바이트에서
/// u8 과 직접 비교하면 어긋난다 — UTF-8 한글이 들어가면 바로 걸린다.
bool sameBytes(const std::vector<u8>& a, const std::string& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

/// 잘 압축되는 긴 텍스트. Deflate 경로를 실제로 태우기 위한 것.
std::string compressible() {
    std::string s;
    for (int i = 0; i < 400; ++i)
        s += "<layer name=\"반복되는 내용이라 잘 압축된다\"/>\n";
    return s;
}

} // namespace

MARI_TEST(deflate_inflate_왕복) {
    const std::string src = compressible();
    auto packed = deflateRaw(reinterpret_cast<const u8*>(src.data()), src.size(), 6);
    CHECK(packed.ok());
    CHECK(packed.value().size() < src.size());

    auto back = inflateRaw(packed.value().data(), packed.value().size(), src.size());
    CHECK(back.ok());
    CHECK_EQ(back.value().size(), src.size());
    CHECK(sameBytes(back.value(), src));
}

MARI_TEST(빈_입력_deflate) {
    auto packed = deflateRaw(nullptr, 0, 6);
    CHECK(packed.ok());
    auto back = inflateRaw(packed.value().data(), packed.value().size(), 0);
    CHECK(back.ok());
    CHECK_EQ(back.value().size(), usize{0});
}

MARI_TEST(mimetype_이_무압축_첫_항목이다) {
    ZipWriter w;
    CHECK(w.beginOra().ok());
    const std::string xml = compressible();
    CHECK(w.add("stack.xml", xml, ZipMethod::Deflate).ok());
    auto done = w.finish();
    CHECK(done.ok());
    const std::vector<u8> z = std::move(done).value();

    // 로컬 파일 헤더는 반드시 파일 맨 앞에 있어야 한다.
    CHECK_EQ(get32(z, 0), 0x04034B50u);
    CHECK_EQ(get16(z, 6), u16{0});  // 범용 플래그 0 — 데이터 디스크립터를 쓰지 않는다
    CHECK_EQ(get16(z, 8), u16{0});  // 압축 방식 0 = Store
    CHECK_EQ(get16(z, 26), u16{8}); // 이름 길이 = strlen("mimetype")
    CHECK_EQ(get16(z, 28), u16{0}); // extra 길이 0 — 이름 바로 뒤가 데이터다
    CHECK_EQ(textAt(z, 30, 8), std::string("mimetype"));
    CHECK_EQ(textAt(z, 38, 16), std::string("image/openraster"));
    CHECK_EQ(get32(z, 18), u32{16}); // 압축 크기 == 원본 크기
    CHECK_EQ(get32(z, 22), u32{16});

    // 중앙 디렉터리의 첫 항목도 mimetype 이어야 한다.
    auto opened = ZipReader::open(z);
    CHECK(opened.ok());
    CHECK_EQ(opened.value().entries().size(), usize{2});
    CHECK_EQ(opened.value().entries()[0].name, std::string("mimetype"));
    CHECK_EQ(opened.value().entries()[0].localHeaderOffset, u64{0});
}

MARI_TEST(store_와_deflate_왕복) {
    std::vector<u8> binary;
    for (int i = 0; i < 5000; ++i)
        binary.push_back(static_cast<u8>((i * 37) ^ (i >> 3)));
    const std::string xml = compressible();

    ZipWriter w;
    CHECK(w.beginOra().ok());
    CHECK(w.add("stack.xml", xml, ZipMethod::Deflate).ok());
    CHECK(w.add("data/layer0.png", binary.data(), binary.size(), ZipMethod::Store).ok());
    auto done = w.finish();
    CHECK(done.ok());

    auto opened = ZipReader::open(std::move(done).value());
    CHECK(opened.ok());
    const ZipReader& z = opened.value();
    CHECK(z.contains("data/layer0.png"));
    CHECK(!z.contains("없는파일.png"));

    auto a = z.read("stack.xml");
    CHECK(a.ok());
    CHECK_EQ(a.value().size(), xml.size());
    CHECK(sameBytes(a.value(), xml));

    auto b = z.read("data/layer0.png");
    CHECK(b.ok());
    CHECK(b.value() == binary);

    auto missing = z.read("없다.txt");
    CHECK(!missing.ok());
    CHECK_EQ(missing.code(), ErrorCode::NotFound);
}

MARI_TEST(이름_중복은_거부한다) {
    ZipWriter w;
    CHECK(w.beginOra().ok());
    auto dup = w.add("mimetype", "x", ZipMethod::Store);
    CHECK(!dup.ok());
    CHECK_EQ(dup.code(), ErrorCode::InvalidArgument);
    // mimetype 은 첫 항목이어야 하므로 두 번째 beginOra 도 거부한다.
    CHECK(!w.beginOra().ok());
}

MARI_TEST(손상된_zip_은_크래시하지_않고_오류다) {
    ZipWriter w;
    CHECK(w.beginOra().ok());
    CHECK(w.add("stack.xml", compressible(), ZipMethod::Deflate).ok());
    auto done = w.finish();
    CHECK(done.ok());
    const std::vector<u8> good = std::move(done).value();

    // 1) 텅 빈 입력
    CHECK(!ZipReader::open(std::vector<u8>{}).ok());
    // 2) ZIP 이 아닌 쓰레기
    CHECK(!ZipReader::open(std::vector<u8>(200, u8{0x5A})).ok());
    // 3) 꼬리가 잘려 EOCD 가 사라졌다
    std::vector<u8> truncated(good.begin(), good.end() - 30);
    CHECK(!ZipReader::open(truncated).ok());
    // 4) 중앙 디렉터리 오프셋이 파일 밖을 가리킨다
    std::vector<u8> badOffset = good;
    const usize eocd = badOffset.size() - 22;
    badOffset[eocd + 16] = 0xFF;
    badOffset[eocd + 17] = 0xFF;
    badOffset[eocd + 18] = 0xFF;
    badOffset[eocd + 19] = 0x7F;
    CHECK(!ZipReader::open(badOffset).ok());
    // 5) 압축 데이터 한 바이트를 뒤집으면 CRC 에서 잡힌다.
    //    mimetype(30+8+16=54) → stack.xml 로컬 헤더(30) + 이름(9) = 93 부터가 데이터다.
    std::vector<u8> flipped = good;
    flipped[120] = static_cast<u8>(flipped[120] ^ 0xFFu);
    auto opened = ZipReader::open(flipped);
    CHECK(opened.ok()); // 디렉터리는 멀쩡하다
    auto bad = opened.value().read("stack.xml");
    CHECK(!bad.ok());
    CHECK_EQ(bad.code(), ErrorCode::ParseError);
}

MARI_TEST_MAIN()
