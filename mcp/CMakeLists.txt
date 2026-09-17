# mari-mcp — MCP 서버 어댑터 (docs/05 1절 · 4절 · A3)
#
# 🔴 **얇다.** 능력은 전부 `mari::agent` 에 있다. 이 모듈이 아는 것은 전송 규약뿐이다 —
#    JSON-RPC 2.0 over stdio, 도구 스키마 생성, 이미지 콘텐츠 포장.
#    연산을 여기서 다시 구현하면 "CLI 로는 되는데 MCP 로는 안 되는 것"이 생긴다(docs/05 1절).
#
# 🔴 도구 목록을 손으로 유지하지 않는다. `agent::opTable()` 을 읽어 생성한다(docs/05 4절).
#    `tests/mcp/test_mcp.cpp` 의 `mcp_tools_generated` 가 그 일치를 강제한다.
#
# 의존: agent 하나뿐이다. 외부 MCP SDK 없음, 네트워크 없음, JSON 라이브러리도 없음
#       (`mari::agent::Json` 을 그대로 쓴다 — 두 번째 JSON 구현을 만들지 않는다).

set(MARI_MCP_SOURCES
    src/tools.cpp
    src/server.cpp
)

add_library(mari_mcp STATIC ${MARI_MCP_SOURCES})
target_link_libraries(mari_mcp PUBLIC mari::common mari::agent)

add_library(mari::mcp ALIAS mari_mcp)
mari_register_module(mari_mcp)
