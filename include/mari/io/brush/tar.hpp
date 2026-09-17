// Mari Paint — 최소 tar(ustar) 리더.
//
// .sut 의 `FileData` 컬럼 안에는 **압축되지 않은 tar 아카이브**가 들어 있고
// 그 안에 브러시 텍스처 PNG 가 있다(docs/01 3.3, docs/02 5.1).
// 우리가 필요한 건 "블록을 돌며 이름과 내용을 꺼내는 것" 하나뿐이라 500줄짜리
// tar 라이브러리를 vendor 할 이유가 없다.
#ifndef MARI_IO_BRUSH_TAR_HPP
#define MARI_IO_BRUSH_TAR_HPP

#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <string>
#include <vector>

namespace mari::io::brush {

/// tar 아카이브 안의 항목 하나.
struct TarEntry {
    std::string name;
    /// 원본 버퍼 안에서의 오프셋과 길이. 내용을 복사하지 않는다.
    usize offset = 0;
    usize size = 0;
    /// ustar typeflag. '0' 또는 '\0' 이 보통 파일.
    char typeFlag = '0';

    [[nodiscard]] bool isFile() const { return typeFlag == '0' || typeFlag == '\0'; }
};

/// tar 아카이브를 훑어 항목 목록을 돌려준다. 내용은 복사하지 않는다(offset/size 만).
/// 헤더가 깨졌거나 체크섬이 맞지 않으면 ParseError. 이미 읽어낸 항목은 버린다 —
/// 반쯤 읽은 아카이브를 "성공"이라고 보고하면 상위가 조용히 틀린 결과를 쓴다.
[[nodiscard]] Result<std::vector<TarEntry>> readTar(const u8* data, usize size);

/// data 가 tar 처럼 보이는가(첫 헤더의 "ustar" 매직 또는 체크섬 일치).
[[nodiscard]] bool looksLikeTar(const u8* data, usize size);

} // namespace mari::io::brush

#endif // MARI_IO_BRUSH_TAR_HPP
