# mari-agent — 에이전트 능력 계층 (docs/05).
#
# 🔴 **이게 진짜 자산이다.** MCP 서버·CLI·JSON-RPC 는 이 위에 씌우는 얇은 어댑터고,
#    사람 UI 도 같은 계층을 쓴다(docs/05 1절). AI 전용 뒷문을 따로 만들지 않는다.
#
# 의존: core(타일·레이어·실행취소) · stroke(사람과 **같은** 스트로크 파이프라인)
#       io/ora(PNG 인코더 재사용) · io/brush(.abr/.sut 임포트) · app(문서 브리지)
#       crypto(스냅샷 대조용 해시). Qt 도 sigan 도 모른다.
#
# 🔴 경계선(docs/03 2절): 여기에는 서명도 해시체인 봉인도 등급 판정도 없다.
#    출처(origin)는 stroke_gate.hpp 가 **박아 넣을 뿐** 고를 수 없다.

set(MARI_AGENT_SOURCES
    src/json.cpp
    src/view.cpp
    src/snapshot.cpp
    src/address.cpp
    src/capabilities.cpp
    src/session.cpp
    src/ops_doc.cpp
    src/ops_layer.cpp
    src/ops_draw.cpp
    src/ops_view.cpp
    src/brush_library.cpp
)

add_library(mari_agent STATIC ${MARI_AGENT_SOURCES})
target_link_libraries(mari_agent PUBLIC
    mari::common mari::core mari::crypto mari::stroke mari::io_ora mari::io_brush mari::app)

add_library(mari::agent ALIAS mari_agent)
mari_register_module(mari_agent)
