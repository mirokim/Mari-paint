// Mari Paint — Adobe Photoshop .psd 읽기/쓰기 (교환 포맷. 네이티브는 .ora)
//
// 범위(정직하게): PSD v1 · RGB · 8비트 · 레이어 RGBA(+ 사용자 마스크) · RLE/Raw · 그룹(lsct) · 클리핑 · 불투명도 ·
// 가시성 · 합성 모드(대응표) · 이름(luni). 안 하는 것: 16/32비트 · CMYK/Lab · 조정/텍스트/스마트 레이어(래스터로 안 굽고
// 건너뛰며 경고) · 레이어 효과 · PSB. 못 옮긴 것은 warnings 에 남긴다 — 조용히 버리지 않는다.
#ifndef MARI_PSD_PSD_HPP
#define MARI_PSD_PSD_HPP

#include <mari/core/layer.hpp>
#include <mari/core/result.hpp>

#include <string>
#include <vector>

namespace mari::psd {

struct Document {
    LayerTreePtr tree;
    std::vector<std::string> warnings;
};

struct SaveOptions {
    bool rle = true; ///< 채널 RLE(PackBits). 끄면 Raw
};

[[nodiscard]] Result<Document> loadFromMemory(const u8* data, usize size);
[[nodiscard]] Result<Document> load(const std::string& path);
[[nodiscard]] Result<std::vector<u8>> saveToMemory(const LayerTree& tree, const SaveOptions& opts = {});
[[nodiscard]] Result<void> save(const LayerTree& tree, const std::string& path, const SaveOptions& opts = {});

/// Mari BlendMode ↔ PSD 4글자 키. 대응이 없으면 "norm"/Normal.
[[nodiscard]] const char* blendKeyOf(BlendMode m) noexcept;
[[nodiscard]] bool blendFromKey(const char key[4], BlendMode& out) noexcept;

} // namespace mari::psd

#endif // MARI_PSD_PSD_HPP
