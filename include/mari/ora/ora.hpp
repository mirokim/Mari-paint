// Mari Paint — OpenRaster(.ora) 읽기/쓰기. Mari 의 네이티브 포맷이다(docs/01 3.4).
//
// 컨테이너 구성:
//   mimetype                 "image/openraster" — **무압축 첫 항목**(스펙 요구)
//   stack.xml                레이어 트리. 첫 자식이 **맨 위** 레이어다
//   data/layer*.png          레이어별 픽셀(RGBA8)
//   mergedimage.png          전체 합성 결과(스펙 요구)
//   Thumbnails/thumbnail.png 최대 256×256 미리보기(스펙 요구)
//   mari/prooflog.json       **Mari 확장** — Sigan 미설치 시의 로컬 무서명 과정 로그
//                            (docs/03 6절). 선택 항목이라 표준 리더는 그냥 무시한다.
//                            내용 생성은 sigan 모듈 몫이고, 여기는 통로만 낸다.
//
// 좌표는 전부 **캔버스 좌표**(좌상단 원점, y 아래로 증가)다.
#ifndef MARI_ORA_ORA_HPP
#define MARI_ORA_ORA_HPP

#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>
#include <mari/core/types.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mari::ora {

/// 아카이브 안 고정 경로들. 테스트와 sigan 모듈이 같은 상수를 보게 한다.
inline constexpr const char* kMimeTypeEntry = "mimetype";
inline constexpr const char* kMimeType = "image/openraster";
inline constexpr const char* kStackEntry = "stack.xml";
inline constexpr const char* kMergedEntry = "mergedimage.png";
inline constexpr const char* kThumbnailEntry = "Thumbnails/thumbnail.png";
/// docs/03 6절 — Sigan 미설치 시 로컬 무서명 로그가 들어갈 자리.
inline constexpr const char* kProofLogEntry = "mari/prooflog.json";
/// 썸네일 한 변의 상한(스펙 권장값).
inline constexpr i32 kThumbnailMaxSide = 256;

/// 저장 옵션.
struct SaveOptions {
    bool writeMergedImage = true;  ///< mergedimage.png 를 넣는다
    bool writeThumbnail = true;    ///< Thumbnails/thumbnail.png 를 넣는다
    i32 thumbnailMaxSide = kThumbnailMaxSide;
    int compressionLevel = 6;      ///< PNG/XML zlib 압축 강도(0~9)
    /// 있으면 mari/prooflog.json 으로 넣는다. 내용은 sigan 모듈이 만든다.
    std::optional<std::string> proofLog;
};

/// 읽기 옵션.
struct LoadOptions {
    bool loadProofLog = true; ///< mari/prooflog.json 이 있으면 같이 읽는다
};

/// 읽어들인 문서 한 벌.
struct Document {
    LayerTreePtr tree;
    /// mari/prooflog.json 내용. 없으면 nullopt. **Mari 는 이걸 검증하지 않는다**
    /// — 봉인·서명은 Sigan 의 몫이다(docs/03 2절 경계선).
    std::optional<std::string> proofLog;
    /// 정직하게 실패한 기록. 모르는 composite-op, 빠진 PNG 등을 남긴다.
    std::vector<std::string> warnings;
};

// ── 메모리 ──────────────────────────────────────────────────────────────

[[nodiscard]] Result<std::vector<u8>> saveToMemory(const LayerTree& tree,
                                                   const SaveOptions& opts = {});
[[nodiscard]] Result<Document> loadFromMemory(std::vector<u8> bytes, const LoadOptions& opts = {});

// ── 파일 ────────────────────────────────────────────────────────────────

[[nodiscard]] Result<void> save(const LayerTree& tree, const std::string& path,
                                const SaveOptions& opts = {});
[[nodiscard]] Result<Document> load(const std::string& path, const LoadOptions& opts = {});

/// .ora 안의 무서명 과정 로그만 꺼낸다. 없으면 nullopt. 픽셀을 디코드하지 않아 싸다.
[[nodiscard]] Result<std::optional<std::string>> readProofLog(const std::string& path);

// ── 파일 유틸 (io/ora 내부용이지만 테스트에서도 쓴다) ──────────────────────

[[nodiscard]] Result<std::vector<u8>> readFileBytes(const std::string& path);
[[nodiscard]] Result<void> writeFileBytes(const std::string& path, const u8* data, usize size);

} // namespace mari::ora

#endif // MARI_ORA_ORA_HPP
