# Mari Paint

가볍고 빠른 오픈소스 페인팅 툴. **Windows COM**으로 다른 프로그램과 연동하고,
**포토샵 브러시(.abr)** 와 **클립스튜디오 브러시(.sut)** 를 한 자리에서 쓴다.

> 🚧 **현재 단계: 코어 + 에이전트 능력 계층 + 헤드리스 CLI/MCP 구현됨 · GUI 없음.**
> 타일 캔버스 · 레이어 · 합성 · 실행취소 · 스트로크 파이프라인 · .ora 읽기/쓰기 ·
> .abr/.sut 임포트 · Sigan 발행기 · **agent-api(연산 37개) · `mari-paint` 헤드리스
> 실행파일 · MCP 서버**가 **Linux에서 빌드되고 테스트로 검증된다.**
> Windows 레이어(COM · Windows Ink · 8bf 호스트)는 **작성만 됐고 컴파일조차 안 해봤다.**
> GUI 는 한 줄도 없다. 정확한 경계는
> [docs/04 — 구현 현황](docs/04-implementation-status.md)에 있다.

## 목표

- **가볍다** — 콜드 스타트 1.5초 이내, 빈 캔버스 150MB 이내. 수치는 CI로 강제한다.
  (헤드리스 실측: **3.8ms · 6.06MB** — 아래 [성능 실측](#성능--실측치)에 전부 적었다)
- **호환된다** — .abr / .sut 브러시, .psd / .ora 파일, .8bf 포토샵 플러그인.
- **붙는다** — COM(dual/IDispatch) 인터페이스로 C#·Python·VBA·PowerShell 어디서든 제어한다.
- **안 죽는다** — 외부 플러그인은 별도 프로세스에서 격리 실행한다.
- **AI가 일급 사용자다 — 단, 정직하게.** 사람이 하는 모든 것을 AI도 같은 계층·같은
  스트로크 파이프라인으로 한다. 대신 **모든 획이 출처를 달고 다니고, 그 출처는
  서명될 프레임 바이트 안에 있다.** origin 을 고르는 API 는 만들지 않는다.

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

현재 **47개 테스트 실행파일 · 375개 케이스**가 돌고, g++ 13.3 과 clang 18 양쪽에서
**전부 통과**한다. 경고 0 · 오류 0 (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).
skip·disable 한 테스트는 없다.

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
| `crypto/` | SHA-256(FIPS 180-4, 외부 의존 0) · 캔버스/레이어/파일 해시 규약 | 이식 가능 |
| `app/` | 플랫폼 중립 브리지 구현체(`Application`·`Document`·`EventHub`) | 이식 가능 |
| `agent/` | 🔴 **에이전트 능력 계층**(docs/05). 연산 표 · 세션 · 스냅샷 · 시각 피드백 · 시맨틱 주소 | 이식 가능 |
| `mcp/` | MCP 서버 어댑터. **도구 목록을 손으로 안 짠다** — `agent::opTable()` 에서 생성 | 이식 가능 |
| `cli/` | 헤드리스 모드 + `mari-paint` 실행파일. `--script` `--exec` `--serve` `--mcp` | 이식 가능 |
| `platform/win/` | COM 서버(`mari.idl`) · Windows Ink(WM_POINTER) 입력 | **Windows 전용** |
| `hosts/` | .8bf 격리 호스트(x86/x64) + 클라이언트 | **Windows 전용** |
| `tests/` | 위 전부의 테스트 + `tests/integration/` 모듈 간 이음매 | |

의존 방향은 한 방향이다:

```
core → crypto → stroke → io/ora → io/brush → sigan → app → agent → mcp → cli
```

core는 아무것도 의존하지 않는다. **`agent` 위의 `mcp`·`cli` 는 능력을 갖지 않는다** —
연산은 전부 `agent::opTable()` 하나에 있고 그 위는 전송 계층이다(docs/05 1절).
`tests/cli/headless_main.cpp` 의 `headless_parity` 가 그 "하나뿐"을 매 빌드마다 검사한다.

`platform/win` 과 `hosts` 는 루트 `CMakeLists.txt` 의 `if(WIN32)` 로 감싸여 있어
Linux 빌드에는 **들어오지 않는다.** CI가 그 사실을 따로 검사한다.

---

## AI · 에이전트로 쓰기

> 설계 근거는 [docs/05](docs/05-agent-api.md). 아래 예제는 **전부 실제로 실행해서
> 출력을 붙여 넣은 것**이다. 지어낸 응답이 아니다.

능력은 `mari::agent` 한 계층에 있고, CLI·MCP·(예정된) COM·GUI 는 그 위에 씌우는
**얇은 어댑터**다. 그래서 어느 입구로 들어와도 같은 연산 37개를 본다.

### 1. 먼저 물어본다 — 문서를 안 읽어도 된다

```sh
build/cli/mari-paint --capabilities | jq '.ops[].name'
```

연산·브러시·블렌드 모드·한계가 **런타임에** 나온다. 임포트한 .abr/.sut 브러시도
여기에 같이 잡힌다 — 하드코딩된 목록이 아니기 때문이다.

### 2. 스크립트로 그린다

`paint.json`:

```json
{
  "agentId": "claude/opus-5",
  "ops": [
    { "op": "doc.create", "width": 512, "height": 512 },
    { "op": "layer.add", "name": "선화", "role": "lineart" },
    { "op": "fill", "layer": "레이어 1", "region": "canvas", "color": "#F4EFE6" },
    { "op": "snapshot", "label": "밑색 완료" },
    { "op": "stroke", "layer": { "role": "lineart" },
      "points": [ {"x":100,"y":380,"p":0.2}, {"x":256,"y":120,"p":0.9}, {"x":412,"y":380,"p":0.3} ],
      "size": 14, "color": "#1B2430", "smoothing": 0.4,
      "pressureProfile": "taper-in-out",
      "view": { "mode": "dirty", "max": 256 } },
    { "op": "doc.describe" },
    { "op": "doc.save", "path": "demo.ora" }
  ]
}
```

```sh
build/cli/mari-paint --headless --script paint.json --out-dir out --pretty
```

눈여겨볼 것 넷:

**① 레이어를 인덱스가 아니라 이름·역할로 지목한다.** `{"role": "lineart"}` 가
방금 만든 레이어를 찾아간다. AI에게 인덱스를 외우게 하지 않는다(docs/05 2.4).

**② 획마다 바뀐 영역 그림이 돌아온다.** `--out-dir out` 을 주면 파일로 떨어진다:

```
out/0-fill.png     5628 bytes
out/1-stroke.png   3022 bytes
```

응답의 이미지 봉투는 이렇게 생겼다 — **바뀐 영역만** 잘라서 준다:

```json
"dirtyRect": [64, 256, 192, 128],
"image": { "format": "png", "encoding": "base64",
           "w": 192, "h": 128, "x": 64, "y": 256,
           "scale": 1, "mode": "dirty", "bytes": 3022 }
```

512×512 캔버스인데 **192×128 만** 돌아왔다. 타일 캔버스가 더티 타일을 이미 추적하고
있어서 공짜로 나오는 것이다. 헤드리스 앱을 스크린샷 찍는 것과 다르다.

**③ `doc.describe` 는 JSON 트리가 아니라 문장을 준다.**
실제 출력:

```
"512x512, 레이어 2개: 레이어 1(채워짐, 편집 중), 선화(1% 채워짐).
 스냅샷 1개. 이 세션의 AI 획 2개(에이전트 claude/opus-5)"
```

**④ 저장 응답은 해시 둘을 이름으로 구분한다.**

```json
{ "path": "demo.ora", "format": "ora",
  "canvasHash": "20a78f9e…",   // 합성 픽셀 해시 (도메인 접두사 있음)
  "fileHash":   "6311bb1c…" }  // 파일 바이트 그대로
```

`fileHash` 는 `sha256sum demo.ora` 와 **글자 그대로 같다.** 확인해 봤다:

```
$ sha256sum demo.ora
6311bb1c2ed12a2a197c6897457bb3cb1283ff5396d34bb161a80f844fbf7a81  demo.ora
```

### 3. 실험하고 되돌린다 — O(1)

COW 타일 구조라서 스냅샷이 픽셀을 복사하지 않는다.

```jsonc
{ "op": "snapshot", "label": "러프 완성" }   // → snapshot.sharesPixels: true
{ "op": "restore",  "snapshot": "러프 완성" }
```

**실측**(`snapshot_is_cheap`, 매 ctest 마다 돈다):
4096×4096 캔버스에 2048² 를 칠한 뒤(타일 1024장) 스냅샷 100개 —
**메모리 +13.43MB, 스냅샷 하나 평균 0.088ms / 최악 0.146ms.**
픽셀을 복사했다면 1.6GB 가 나왔어야 한다.

### 4. 한 번에 묶어 보낸다 — 실패하면 전부 롤백

```jsonc
{ "op": "batch", "atomic": true,
  "ops": [ /* …최대 1000개… */ ],
  "view": { "mode": "dirty" } }   // 렌더는 마지막에 한 번만
```

중간에 하나라도 실패하면 캔버스가 **호출 전 상태로 돌아간다**(`batch_atomic`).
반쯤 망가진 캔버스가 없으니 AI가 안심하고 큰 묶음을 보낸다.

### 5. MCP 서버로 붙인다

```sh
build/cli/mari-paint --headless --mcp --stdio
```

한 줄 = JSON-RPC 2.0 메시지 하나. **도구 목록은 손으로 유지하지 않는다** —
`agent::opTable()` 을 읽어 생성한다. 연산을 하나 더하면 도구도 저절로 하나 는다
(`mcp_tools_generated` 가 글자 단위로 강제한다).

지금 나가는 도구는 **35개**다. 37개가 아닌 이유: 미지원 연산 둘
(`select.invert`·`select.feather`)은 **도구를 아예 주지 않는다.** 주면 AI가 반드시
한 번 불러 보고 실패한다. 대신 `capabilities` 도구가 그 둘과 이유를 알려준다.

```sh
# 클라이언트 설정 예 (MCP 서버 목록에 추가)
{ "command": "/path/to/mari-paint",
  "args": ["--headless", "--mcp", "--stdio", "--agent-id", "claude/opus-5"] }
```

### 6. 🔴 그 대가 — 출처는 고를 수 없다

에이전트 API로 들어온 획은 **무조건** `origin=agent` 다. 고르는 파라미터가 없다.

```jsonc
// capabilities 응답에 그대로 적혀 있다
"origin": { "forced": "agent", "agentId": "claude/opus-5", "settable": false }
```

`--agent-id` 로 **이름을 댈 수만** 있고, 이름이 없으면 세션이 아예 열리지 않는다
(익명 AI 획은 만들지 않는다). 실행이 끝나면 집계가 따라 나온다:

```json
"strokeOrigins": {
  "agentId": "claude/opus-5",
  "originCounts": { "humanPen": 0, "humanMouse": 0, "agent": 2,
                    "imported": 0, "filter": 0, "unspecified": 0 },
  "humanStrokes": 0, "agentStrokes": 2, "totalStrokes": 2, "agentStrokeRatio": 1
}
```

이걸 **코드 구조로** 막는다. `StrokeOrigin::Agent` 를 만들 수 있는 코드는 리포 전체에
`mari::agent::AgentStrokeGate` 하나뿐이다 — `StrokeSource` 는 기본 생성자가 **삭제**돼
있고(출처를 빠뜨리면 컴파일이 깨진다), 유일한 생성자가 private 이며, friend 는 그
게이트 하나다. origin 을 받는 공개 세터도, API 파라미터도 0개다.

그리고 origin 은 **서명될 프레임 바이트 안**에 있다. origin 하나만 바꾸면 64바이트
프레임에서 정확히 오프셋 56 한 바이트가 달라진다(`origin_in_signature`).
서명 밖이면 사후에 고칠 수 있어 아무 의미가 없다.

> ⚠️ **다만 아직 배선은 없다.** 위는 "위조할 수 없다"까지의 증명이고,
> 에이전트가 그린 획을 실제로 `SiganPublisher` 로 넘기는 코드는 **아직 없다.**
> 자세한 건 [docs/05 8.1](docs/05-agent-api.md), [docs/04 4절 7번](docs/04-implementation-status.md).

---

## 성능 — 실측치

docs/02 8절 목표 5개 중 **화면 없이 잴 수 있는 4개를 실제로 잰다.**
`tests/cli/headless_main.cpp` 가 매 ctest 마다 재고, **목표를 넘기면 테스트가 깨진다.**

| 목표 | 목표치 | 실측 (g++ 13.3 · Linux 컨테이너) | |
|---|---|---|---|
| 콜드 스타트 | < 1.5초 | **3.8 ms** (`--capabilities` 5회 최소, 셸 경유 포함) | ✅ |
| 빈 캔버스 1920×1080 | < 150MB | **6.06 MB** (프로세스 전체 RSS) | ✅ |
| 8K 캔버스 열기 | < 3초 | **550 ms** (8192² 전면 채색 .ora) | ✅ |
| 설치 용량 | < 80MB | **24.94 MB** (미스트립 실행파일 하나) | ⚠️ |
| 펜 입력 → 화면 표시 | < 16ms | **측정 불가** | 🔴 |

**⚠️ 와 🔴 를 흐리지 않는다:**

- 24.94MB 는 **GUI·COM·8bf 호스트가 하나도 안 들어간** 빌드다. 붙으면 는다.
  zlib·sqlite3·libpng 는 시스템 공유 라이브러리라 여기 안 잡힌다.
- **16ms 목표는 달성했다고 쓰지 않는다.** 화면이 없어서 절반만 잴 수 있다.
  잰 절반(입력 → 픽셀)은 agent-api 를 통과하는 점 하나당 **111.8 us**,
  획(점 64개) 하나당 평균 **7.2 ms**. 표시 쪽을 붙이지 않은 숫자다.

목표에 없던 값 하나가 눈에 띈다: **8K .ora 저장이 9.0초**다. 여는 건 550ms 인데
쓰는 게 16배 느리다(8192² PNG 디플레이트). 목표가 없어 통과/실패를 말할 수 없고,
그래서 게이트도 걸지 않았다. 숫자만 남긴다.

참고로 순수 파이프라인 처리량(`stroke_bench`, UI·에이전트 미경유):
스탬프 **150,056개/초**, 이벤트당 **72.4 us**, 그리고 **2차 획 힙 할당 0회.**

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
- SHA-256 — NIST 벡터 통과. **계산만 한다. 체인 봉인은 안 한다**(`tests/crypto/`)
- **agent-api 연산 37개** — 세션 · O(1) 스냅샷 · 원자적 batch · 시각 피드백 ·
  시맨틱 주소 · 런타임 브러시 발견 (`tests/agent/`)
- **`mari-paint` 헤드리스 실행파일** — 진짜 프로세스를 띄워 stdout/stderr/종료 코드를
  검사한다. 스크립트로 그리고 저장한 `.ora` 가 다시 열린다 (`tests/cli/`)
- **MCP 서버** — `tools/list` 가 연산 표와 글자 단위로 일치하고, 결과 이미지가
  MCP 이미지 콘텐츠로 실린다 (`tests/mcp/`)
- 🔴 **출처 위조 불가** — `StrokeOrigin::Agent` 를 만들 수 있는 코드가 리포 전체에
  게이트 하나뿐이다. origin 을 받는 공개 생성자·세터·API 파라미터가 **0개**
  (`agent_origin_forced` · `no_origin_override` · `origin_in_signature`)

### 작성만 됐고 **컴파일조차 못 해봄** (개발 환경이 Linux)

- COM 서버 전체 — `mari.idl`, dual/IDispatch 구현, `LocalServer32` 등록, 이벤트 싱크
  → MIDL 컴파일러와 Windows SDK가 필요하다
- Windows Ink(WM_POINTER) 입력 경로 — 실제 펜 하드웨어가 필요하다
- .8bf 격리 호스트(x86/x64) — 실제 플러그인 바이너리가 필요하다
- 명명 파이프 `sigan-native` 의 Windows 구현 (`CreateNamedPipe` 경로)

### 아직 없음

GUI(Qt) · libmypaint 어댑터 · .psd 읽기/쓰기 · GPU 합성 · 색 관리(LittleCMS) ·
선택 마스크(지금 선택은 **사각형 하나**뿐이라 `select.invert`/`select.feather` 는
"미지원"으로 표시하고 도구로도 안 내보낸다).

🔴 그리고 하나 더, 제일 큰 빈칸:
**에이전트가 그린 획이 Sigan 발행기로 흘러가지 않는다.**
origin 은 위조할 수 없고(테스트가 증명) 프레임 바이트 안에 있지만(오프셋 56),
`agent`/`cli`/`mcp` 중 `mari::sigan` 을 링크하는 모듈이 하나도 없다.
그래서 docs/05 3.2 의 "사람 획 1,847 / AI 획 213" 은 **아직 인증서에 못 찍힌다** —
지금 나오는 건 세션 로컬 집계뿐이다.

전부 [docs/04](docs/04-implementation-status.md)에 이유와 함께 적어 뒀다.

---

## 문서

- [01 — 오픈소스 리서치](docs/01-research.md) — 무엇을 가져오고 무엇을 피할지, 근거와 출처
- [02 — 아키텍처 설계](docs/02-architecture.md) — 모듈 구성, COM 레이어, 로드맵, 위험 요소
- [03 — Sigan(VASE9) 연동](docs/03-sigan-integration.md) — 과정 증명 기록, 채널 설계, WinTab 금지 근거
- [04 — 구현 현황](docs/04-implementation-status.md) — **지금 무엇이 되고 무엇이 안 되는가**
- [05 — 에이전트 API](docs/05-agent-api.md) — **AI가 일급 사용자가 된다는 게 무슨 뜻인가**, 그리고 그 대가로 지킨 출처 규약

## 로드맵 요약

`M0` 타일 캔버스 → `M1` 그리기 → `M2` 브러시 호환 → `M3` PSD → `M4` COM → `M5` 8bf 호스트 → `M6` 성능

현재 위치: **M0 완료 · M1 절반(엔진·파이프라인은 되고 GUI·입력은 미검증) · M2 대부분 ·
M4/M5 미검증.** 에이전트 트랙은 별도로 **A0~A4 완료 · A5 부분**(docs/05 7절).

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
7. 🔴 **에이전트 획의 프레임 표현** — `stroke` 는 점 목록이 있으니 프레임으로 바꿀 수
   있지만 `fill`·`erase`·`gradient` 는 타일에 직접 쓴다(점이 없다). 지금은 집계에만
   잡힌다. 발행기에 배선하기 **전에** "획 하나 = 프레임 하나"를 어떻게 맞출지 정해야
   한다 — 서명 포맷은 한 번 나가면 못 바꾼다(docs/03 8절)
