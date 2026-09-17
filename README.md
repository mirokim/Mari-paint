# Mari Paint

가볍고 빠른 오픈소스 페인팅 툴. **Windows COM**으로 다른 프로그램과 연동하고,
**포토샵 브러시(.abr)** 와 **클립스튜디오 브러시(.sut)** 를 한 자리에서 쓴다.

> 🚧 **현재 단계: 코어 구현됨 · UI 없음.**
> 타일 캔버스 · 레이어 · 합성 · 실행취소 · 스트로크 파이프라인 · .ora 읽기/쓰기 ·
> .abr/.sut 임포트 · Sigan 발행기가 **Linux에서 빌드되고 테스트로 검증된다.**
> Windows 레이어(COM · Windows Ink · 8bf 호스트)는 **작성만 됐고 컴파일조차 안 해봤다.**
> 정확한 경계는 [docs/04 — 구현 현황](docs/04-implementation-status.md)에 있다.

## 목표

- **가볍다** — 콜드 스타트 1.5초 이내, 빈 캔버스 150MB 이내. 수치는 CI로 강제한다.
- **호환된다** — .abr / .sut 브러시, .psd / .ora 파일, .8bf 포토샵 플러그인.
- **붙는다** — COM(dual/IDispatch) 인터페이스로 C#·Python·VBA·PowerShell 어디서든 제어한다.
- **안 죽는다** — 외부 플러그인은 별도 프로세스에서 격리 실행한다.

---

## 빌드

의존성: C++20 컴파일러(g++ 13+ / clang 18+), CMake 3.20+, Ninja,
그리고 시스템 라이브러리 **zlib · libSQLite3 · libpng**.

```sh
# Ubuntu 24.04 기준
sudo apt-get install -y build-essential cmake ninja-build \
                        zlib1g-dev libsqlite3-dev libpng-dev

cmake -S . -B build -G Ninja
cmake --build build
```

Qt는 **필요 없다.** core / stroke / io / sigan 은 Qt 비의존 설계이고,
지금 리포에 Qt를 쓰는 코드는 한 줄도 없다(UI 레이어가 아직 없다).

## 테스트

```sh
ctest --test-dir build --output-on-failure
```

현재 **35개 테스트 실행파일 · 219개 케이스**가 돌고, g++ 13.3 과 clang 18 양쪽에서
**전부 통과**한다. 경고 0 · 오류 0 (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).

테스트를 추가하려면 `tests/<모듈>/test_*.cpp` 파일을 놓기만 하면 된다 —
`tests/CMakeLists.txt` 가 자동으로 실행파일을 만들고 ctest에 등록한다.

---

## 모듈 구조

| 디렉터리 | 내용 | 플랫폼 |
|---|---|---|
| `include/mari/` | 모든 공개 헤더 (`core` `stroke` `brush` `ora` `io/brush` `sigan` `win` `host8bf` `test`) | |
| `core/` | 타일 캔버스(64×64, COW) · 레이어 트리 · 블렌드 19종 · 합성기 · 실행취소 | 이식 가능 |
| `stroke/` | 정규화 → 스무딩 → 보간 → 네이티브 엔진 → 더티 타일 | 이식 가능 |
| `io/ora/` | OpenRaster 읽기/쓰기 (자체 ZIP + libpng + 자체 XML) | 이식 가능 |
| `io/brush/` | .abr(Photoshop) · .sut(Clip Studio, SQLite) → `MariBrushPreset` + `ImportReport` | 이식 가능 |
| `sigan/` | 스트로크 프레임 · 단조 시계 · 저널 · 핸드셰이크 · 발행기 · 무서명 과정 로그 | 이식 가능 |
| `platform/win/` | COM 서버(`mari.idl`) · Windows Ink(WM_POINTER) 입력 | **Windows 전용** |
| `hosts/` | .8bf 격리 호스트(x86/x64) + 클라이언트 | **Windows 전용** |
| `tests/` | 위 전부의 테스트 + `tests/integration/` 모듈 간 이음매 | |

의존 방향은 한 방향이다: `io → core`, `stroke → core`, `platform/win → core`.
core는 아무것도 의존하지 않는다.

`platform/win` 과 `hosts` 는 루트 `CMakeLists.txt` 의 `if(WIN32)` 로 감싸여 있어
Linux 빌드에는 **들어오지 않는다.** CI가 그 사실을 따로 검사한다.

---

## 🔴 검증된 것 / 검증 못 한 것

이 구분이 이 README에서 제일 중요하다. **돌려보지 않은 것을 "완료"라고 쓰지 않는다.**

### Linux에서 실제로 빌드·테스트됨

- 타일 캔버스 — **4000×4000 캔버스 구석에 점 하나 → 타일 1개만 할당**된다는 걸
  테스트가 증명한다(`tests/core/test_tile.cpp`). COW 스냅샷 독립성도.
- 레이어 트리 · 19종 블렌드 모드 · 더티 영역만 재합성 · 타일 단위 실행취소
- 스트로크 파이프라인 전 단계와 네이티브 브러시 엔진 — 실제 `TileMap` 에 그린다
- .ora 왕복 — 픽셀 · 불투명도 · 블렌드 모드 · 레이어 순서 보존, 희소성 유지
- .abr / .sut 임포트 — **번역 못 한 파라미터가 `ImportReport` 에 실제로 남는다**
- Sigan 발행기 — seq 구멍 0, 오버플로 시 **드롭이 아니라 저널 스풀**,
  재연결이 새 구간을 만들지 않음, 버전 불일치에도 기록 지속
- Windows 레이어 중 **Win32 비의존으로 떼어낸 부분** — 뷰 변환(캔버스 좌표 불변성),
  펜 정규화, 8bf 공유 메모리 레이아웃, PIPL 파서 (`tests/win/`, 38개 케이스)

### 작성만 됐고 **컴파일조차 못 해봄** (개발 환경이 Linux)

- COM 서버 전체 — `mari.idl`, dual/IDispatch 구현, `LocalServer32` 등록, 이벤트 싱크
  → MIDL 컴파일러와 Windows SDK가 필요하다
- Windows Ink(WM_POINTER) 입력 경로 — 실제 펜 하드웨어가 필요하다
- .8bf 격리 호스트(x86/x64) — 실제 플러그인 바이너리가 필요하다
- 명명 파이프 `sigan-native` 의 Windows 구현 (`CreateNamedPipe` 경로)

### 아직 없음

UI(Qt) · libmypaint 어댑터 · .psd 읽기/쓰기 · GPU 합성 · 색 관리(LittleCMS) ·
픽셀 해시(SHA-256) 실제 계산 — 인터페이스만 있고 구현이 없다.

전부 [docs/04](docs/04-implementation-status.md)에 이유와 함께 적어 뒀다.

---

## 문서

- [01 — 오픈소스 리서치](docs/01-research.md) — 무엇을 가져오고 무엇을 피할지, 근거와 출처
- [02 — 아키텍처 설계](docs/02-architecture.md) — 모듈 구성, COM 레이어, 로드맵, 위험 요소
- [03 — Sigan(VASE9) 연동](docs/03-sigan-integration.md) — 과정 증명 기록, 채널 설계, WinTab 금지 근거
- [04 — 구현 현황](docs/04-implementation-status.md) — **지금 무엇이 되고 무엇이 안 되는가**

## 로드맵 요약

`M0` 타일 캔버스 → `M1` 그리기 → `M2` 브러시 호환 → `M3` PSD → `M4` COM → `M5` 8bf 호스트 → `M6` 성능

현재 위치: **M0 완료 · M1 절반(엔진·파이프라인은 되고 UI·입력은 미검증) · M2 대부분 · M4/M5 미검증.**

## Sigan 연동 — 제품 목표

[Sigan](https://github.com/mirokim/VASE9)(작업 과정 증명 서비스)은 포토샵·CSP 안으로 들어갈 수 없어
**밖에서 펜 신호를 훔쳐본다.** 그래서 캔버스 좌표도, 픽셀도, 레이어도 모른다.
Mari Paint는 **안에 있다.**

> **Mari Paint = Sigan이 완전한 증거를 얻을 수 있는 유일한 페인트 툴.**

두 제품은 **분리한다.** Mari는 증명 가능한 형태로 그릴 뿐, 증명하지 않는다 —
체인 봉인·서명·등급 판정은 Sigan의 몫이다.
그 경계선은 코드에서도 지켜진다: 리포 전체에 해시체인 봉인도, ES256 서명도 **없다.**
Mari가 붙일 수 있는 등급은 `unsigned` 하나뿐이고, 테스트가 그걸 강제한다
(`tests/sigan/test_prooflog.cpp`).

> ⚠️ 그래서 입력은 **Windows Ink 고정이고 WinTab은 구현하지 않는다.**
> WinTab 앱은 펜 HID를 독점해 죽인다 (CSP·포토샵·Krita에서 실측).
> 목표: **Sigan의 `WinTabKeywords` 목록에 영원히 오르지 않는 최초의 드로잉 앱.**
> 리포 전체에 `wintab32` 를 로드하는 코드가 한 줄도 없고, CI가 산출물의 임포트
> 테이블을 검사한다(`scripts/ci/check-no-wintab.ps1`).

## 정해야 할 것

1. **라이선스 확정** — Apache-2.0 제안 (`LICENSE` 파일은 이미 Apache-2.0)
2. **1차 타깃 OS** — Windows 선출시 후 이식 vs 처음부터 크로스플랫폼
3. **Mari 프로세스명** — `mari-paint.exe`? Sigan의 `DrawingApps` 목록에 등록해야 한다
4. **픽셀 스냅샷 주기·전송 방식** — 공유 메모리 여부, 매 획 vs N초
5. **레이어 마스크 규약** — 없는 타일 = 0 = 가림(Krita와 반대). 임포터가 흡수 중
6. **Sigan 와이어 식별자 폭** — `LayerId`/`BrushId` 는 u64, 와이어는 u32
