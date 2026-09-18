// Mari Paint — 브러시 라이브러리: 네이티브 프리셋 파일(.mbp) 저장/불러오기 + 사용자 폴더
//
// .mbp = MariBrushPreset 을 JSON 으로 적은 것. 비트맵(팁·텍스처·듀얼 팁)은 PNG 를 base64 로 싣는다.
// GUI 와 헤드리스(--serve) 가 **같은 폴더**를 읽어 같은 브러시를 본다(docs/05 2.8 같은 바이너리·같은 경로).
//   Windows: %LOCALAPPDATA%/Mari/Mari Paint/brushes   그 외: ~/.local/share/mari-paint/brushes
#ifndef MARI_AGENT_BRUSH_LIBRARY_HPP
#define MARI_AGENT_BRUSH_LIBRARY_HPP

#include <mari/agent/json.hpp>
#include <mari/brush/preset.hpp>
#include <mari/core/result.hpp>

#include <string>
#include <vector>

namespace mari::agent {

/// 사용자 브러시 폴더. 없으면 만든다. 환경변수 MARI_BRUSH_DIR 이 있으면 그것을 쓴다.
[[nodiscard]] std::string defaultBrushDir();

/// 프리셋 ↔ JSON.
[[nodiscard]] Json presetToJson(const brush::MariBrushPreset& p);
[[nodiscard]] Result<brush::MariBrushPreset> presetFromJson(const Json& j);

/// .mbp 파일 하나.
[[nodiscard]] Result<void> savePresetFile(const std::string& path, const brush::MariBrushPreset& p);
[[nodiscard]] Result<brush::MariBrushPreset> loadPresetFile(const std::string& path);

/// 폴더의 *.mbp 를 전부 읽는다(이름순). 깨진 파일은 건너뛰고 `skipped` 에 적는다.
[[nodiscard]] std::vector<brush::MariBrushPreset> loadPresetDir(const std::string& dir,
                                                                std::vector<std::string>* skipped = nullptr);

/// 프리셋 이름으로 안전한 파일 경로를 만든다(`dir/<slug>.mbp`, 겹치면 -2, -3 …).
[[nodiscard]] std::string presetFilePath(const std::string& dir, const std::string& name);

/// 이름이 같은 .mbp 를 지운다. 없으면 그냥 Ok.
[[nodiscard]] Result<void> removePresetFile(const std::string& dir, const std::string& name);

} // namespace mari::agent

#endif // MARI_AGENT_BRUSH_LIBRARY_HPP
