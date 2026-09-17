# mari-record — 기록 배선 (docs/06)
#
# 🔴 **이 모듈이 유일하게 `SiganPublisher::publish()` 를 부른다.**
#    사람 획이든 AI 획이든, 붓질이든 영역 연산이든 전부 여기 한 곳을 지난다.
#    `tests/record/test_single_publish_path.cpp` 가 리포를 훑어 그것을 강제한다.
#
# 🔴 의존 방향이 이 모듈의 존재 이유다:
#      app     → sigan 을 모른다 (agent::IStrokeRecorder 만 안다)
#      agent   → sigan 을 모른다 (같은 인터페이스만 안다)
#      record  → 둘 다 안다. 꽂는 것은 위(CLI·호스트·테스트)의 일이다.
#    그래서 Sigan 이 없어도 agent-api 는 그대로 돈다 — 공장을 안 꽂으면 널 레코더다.
#
# 🔴 경계선(docs/03 2절): 서명도 해시체인 봉인도 등급 판정도 없다. 숫자뿐이다.

set(MARI_RECORD_SOURCES
    src/sigan_recorder.cpp
)

add_library(mari_record STATIC ${MARI_RECORD_SOURCES})
target_link_libraries(mari_record PUBLIC mari::common mari::core mari::sigan mari::app)

add_library(mari::record ALIAS mari_record)
mari_register_module(mari_record)
