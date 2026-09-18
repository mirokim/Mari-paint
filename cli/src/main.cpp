// Mari Paint — mari-paint 실행파일.
//
// 🔴 여기는 얇다. 진짜 일은 전부 `mari::cli::runCli()` 안에 있다 —
//    그래야 테스트가 스트림을 갈아끼워 같은 코드 경로를 검증할 수 있다.
//    (docs/05 2.8 "같은 바이너리, 같은 코드 경로")
#include <mari/cli/runner.hpp>
#if defined(MARI_HAS_GUI)
#include "gui.hpp"
#endif

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
#if defined(MARI_HAS_GUI)
    // 인자가 없으면 GUI 다. 인자가 하나라도 있으면 헤드리스 CLI 다 — 스크립트가 실수로
    // 창을 띄우는 일은 없고, 사람이 더블클릭하면 창이 뜬다.
    if (argc <= 1) {
        return mari::ui::runGui(argc, argv);
    }
#endif
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    // stdout 은 JSON 하나만 나가므로 버퍼링을 그대로 둔다.
    // stderr 는 진행 상황이라 순서가 보여야 한다 — iostream 기본이 unitbuf 는 아니지만
    // std::cerr 는 unbuffered/tie 되어 있어 그대로 쓴다.
    return mari::cli::runCli(args, std::cout, std::cerr);
}
