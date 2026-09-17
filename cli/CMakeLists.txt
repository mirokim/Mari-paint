# mari-cli — 헤드리스 모드와 CLI (docs/05 2.8 · A2)
#
# 🔴 능력은 라이브러리(`mari_cli`)에 있고, 실행파일(`mari-paint`)은 main() 한 줄짜리 껍데기다.
#    UI 가 얇아야 한다는 규칙은 CLI 에도 똑같이 적용된다 — CLI 도 껍데기다.
#    덕분에 테스트가 같은 코드 경로를 프로세스 없이 직접 부를 수 있다.
#
# 🔴 능력은 여기 없다. 연산은 전부 `mari::agent`(A1 agent-api) 가 갖고 있고
#    CLI 는 그 위에 얹는 어댑터다 — JSON 도 `mari::agent::Json` 을 그대로 쓴다.
#    두 번째 구현을 만들면 "CLI 로는 되는데 MCP 로는 안 되는 것"이 생긴다(docs/05 1절).
#
# 의존: agent(+그 아래 core·io/ora·app). Qt 도 sigan 도 모른다. 외부 JSON 라이브러리 없음.

set(MARI_CLI_SOURCES
    src/runner.cpp
    src/server.cpp
)

add_library(mari_cli STATIC ${MARI_CLI_SOURCES})
# mari::mcp — `--mcp --stdio` 가 쓰는 MCP 어댑터. 얇은 전송 계층이라 여기서 링크만 한다.
target_link_libraries(mari_cli PUBLIC mari::common mari::agent mari::mcp mari::io_ora)

add_library(mari::cli ALIAS mari_cli)
mari_register_module(mari_cli)

# 실제로 돌려 볼 수 있는 바이너리. Qt 없이 빌드된다.
add_executable(mari-paint src/main.cpp)
target_link_libraries(mari-paint PRIVATE mari::common mari::cli)
set_target_properties(mari-paint PROPERTIES OUTPUT_NAME "mari-paint")

# 🔴 진짜 프로세스를 띄워서 검증하는 테스트.
#    tests/ 의 자동 발견(test_*.cpp)에 걸리지 않게 파일 이름을 달리 두고 여기서 직접 건다 —
#    바이너리 경로를 컴파일 타임에 박아 넣어야 해서 타깃을 우리가 쥐고 있어야 한다.
if(MARI_BUILD_TESTS)
    mari_add_test(cli_binary
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../tests/cli/cli_binary_main.cpp"
        LIBS mari::cli)
    target_compile_definitions(mari_test_cli_binary PRIVATE
        MARI_PAINT_BIN="$<TARGET_FILE:mari-paint>")
    add_dependencies(mari_test_cli_binary mari-paint)

    # 🔴 docs/05 6절의 headless_parity + docs/02 8절 실측.
    #    같은 이유로 여기서 직접 건다 — 바이너리를 띄워 세 표면(프로세스·MCP·인프로세스 표)을
    #    맞대 보려면 MARI_PAINT_BIN 이 필요하다.
    mari_add_test(cli_headless
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../tests/cli/headless_main.cpp"
        LIBS mari::cli)
    target_compile_definitions(mari_test_cli_headless PRIVATE
        MARI_PAINT_BIN="$<TARGET_FILE:mari-paint>")
    add_dependencies(mari_test_cli_headless mari-paint)
endif()
