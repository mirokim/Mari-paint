// Mari Paint — 브러시 임포터 공개 API. .abr(Photoshop) / .sut(Clip Studio) → MariBrushPreset.
//
// docs/01 3.2·3.3, docs/02 5절. 이게 Mari 의 차별점이다:
//   · Krita 의 .abr 임포트는 **텍스처만** 가져온다 → 사용자가 간격·필압을 손으로 다시 맞춘다.
//   · Mari 는 `desc` 섹션의 ActionDescriptor 를 직접 파싱해 **동작까지** 가져온다.
//   · 그리고 번역하지 못한 것은 **전부 ImportReport 에 남긴다.** 조용히 버리지 않는다.
//
// 임포트는 저빈도 경로라 Result<T> 로 실패를 돌려준다(예외를 던지지 않는다).
// **부분 성공이 정상이다.** 프리셋이 하나라도 나왔으면 성공으로 돌려주고,
// 못 읽은 브러시·못 번역한 파라미터는 리포트에 적는다.
#ifndef MARI_IO_BRUSH_IMPORTER_HPP
#define MARI_IO_BRUSH_IMPORTER_HPP

#include <mari/brush/preset.hpp>
#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <string>

namespace mari::io::brush {

using mari::brush::ImportResult;

/// 임포트 원본 포맷.
enum class BrushFormat : u8 {
    Unknown = 0,
    Abr, ///< Adobe Photoshop 브러시
    Sut, ///< Clip Studio Paint 서브툴(= SQLite DB)
};

/// 확장자와 매직 바이트로 포맷을 추정한다. 파일을 열어 앞부분만 읽는다.
[[nodiscard]] BrushFormat detectFormat(const std::string& path);

/// .abr 파일 하나를 임포트한다.
[[nodiscard]] Result<ImportResult> importAbrFile(const std::string& path);

/// 메모리 위의 .abr 을 임포트한다(테스트·스트림 입력용).
[[nodiscard]] Result<ImportResult> importAbrBytes(const u8* data, usize size,
                                                  const std::string& sourceName);

/// .sut 파일 하나를 임포트한다. 파일은 통째로 SQLite DB 다.
/// 컬럼 집합이 CSP 버전마다 다르므로 **런타임 조회 후 있는 것만** 매핑한다.
[[nodiscard]] Result<ImportResult> importSutFile(const std::string& path);

/// 포맷을 자동 판별해 임포트한다.
[[nodiscard]] Result<ImportResult> importBrushFile(const std::string& path);

} // namespace mari::io::brush

#endif // MARI_IO_BRUSH_IMPORTER_HPP
