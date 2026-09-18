// Mari Paint — GUI 진입점. `mari-paint` 가 인자 없이 뜨면 여기로 온다.
//
// 🔴 같은 바이너리, 같은 코드 경로(docs/05 2.8). 헤드리스 CLI 와 GUI 는 실행파일 하나고,
//    문서·기록·그리기 부품도 하나다. GUI 는 그 위의 얇은 껍데기다.
#ifndef MARI_UI_GUI_HPP
#define MARI_UI_GUI_HPP

namespace mari::ui {

/// Qt 애플리케이션을 띄우고 이벤트 루프를 돈다. 창이 닫히면 돌아온다.
/// argc/argv 는 QApplication 규약대로 넘긴다(Qt 옵션을 걸러 낸다).
int runGui(int argc, char** argv);

} // namespace mari::ui

#endif // MARI_UI_GUI_HPP
