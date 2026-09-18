// Mari Paint — 아이콘 (Tabler Icons, MIT · ui/icons/LICENSE.tabler)
//
// 🔴 QtSvg 는 CSS `currentColor` 를 모른다(검정으로 그린다). 그래서 파일에는 기본색 #dcdcdc 를
//    구워 두고, 여기서 상태(보통·호버·체크·비활성)에 맞춰 색을 치환해 QSvgRenderer 로 직접 그린다.
//    qsvgicon 아이콘엔진 **플러그인**에 기대지 않는다 — 설치 크기 예산과 배포 누락 위험 때문이다.
//    DPR 을 곱해 150% 화면에서 흐려지지 않는다.
#ifndef MARI_UI_ICONS_HPP
#define MARI_UI_ICONS_HPP

#include <QIcon>
#include <QString>

namespace mari::ui {

/// `:/icons/<name>.svg` 를 다크 테마 색으로 굽는다. px 는 논리 px.
[[nodiscard]] QIcon themedIcon(const QString& name, int px = 24);

} // namespace mari::ui

#endif // MARI_UI_ICONS_HPP
