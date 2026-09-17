# mari-crypto — SHA-256 계산과 픽셀 스냅샷 해시.
#
# 🔴 이 모듈은 **해시를 계산할 뿐이다.** 체인 봉인·서명(ES256)·등급 판정은
#    Sigan 의 몫이라 여기 들어오지 않는다(docs/03 2절 경계선).
# 외부 암호 라이브러리에 의존하지 않는다. SHA-256 은 FIPS 180-4 그대로 구현했다.

set(MARI_CRYPTO_SOURCES
    src/sha256.cpp
    src/canvas_hash.cpp
)

add_library(mari_crypto STATIC ${MARI_CRYPTO_SOURCES})
target_link_libraries(mari_crypto PUBLIC mari::common mari::core)

add_library(mari::crypto ALIAS mari_crypto)
mari_register_module(mari_crypto)
