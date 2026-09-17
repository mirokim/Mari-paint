# Mari Paint — 기록 규약 (Recording Contract)

> 대상: 에이전트 획 → `SiganPublisher` 배선 (docs/04 4절 7번 · 5절 3-2)
> 기준: 2026-09-17 · **배선 완료 후 재검증 · ctest 55/55 · 경고 0**(g++ 13.3 · clang 18)
> **이 문서는 코드가 아니라 결정이다.** 배선을 넣는 사람은 여기 적힌 다섯 결정을 그대로 따른다.
> 다섯 결정은 구현 뒤에도 **하나도 바뀌지 않았다.** 문서가 현실과 어긋났던 곳은
> 9절 규칙에 따라 10절·11절에 적었다.

---

## 0. 왜 배선 전에 이 문서가 먼저인가

지금 서 있는 조각은 셋이고, 잇는 한 겹이 비어 있다(docs/04 4절 7번).

| 있는 것 | 증명 |
|---|---|
| origin 을 위조할 수 없다 | `no_origin_override` · `agent_origin_forced` |
| origin 이 프레임 바이트(오프셋 56) 안에 있다 | `origin_in_signature` |
| 발행기가 유실 없이 기록한다 | `no_frame_drop` · `published == sent + spooled` |
| **없는 것** | 에이전트 획을 발행기로 넘기는 배선 |

그런데 그 빈 겹을 그냥 채우면 안 된다. **`fill` · `erase` · `gradient` 는 타일을 직접 쓴다.**
스트로크 프레임을 만들지 않는데 `countStroke()` 로 집계에는 잡힌다.
즉 지금 코드에서 **"획 하나 = 프레임 하나"가 이미 성립하지 않는다.**

이걸 정하지 않고 배선하면 docs/05 3.2 의 숫자가 **거짓이 된다.**

> AI 가 `fill` 한 번으로 1024×1024 캔버스 전체를 칠했는데
> 인증서에 "AI 획 1개 (0.05%)" 로 찍히면, 그건 참말로 쓴 거짓말이다.

---

## 0.1 먼저 확인한 것 — `LogEvent` 는 **없다**

과제 지시는 "docs/03 의 `LogEvent` 구조가 이미 있으니 활용할 수 있는지 먼저 보라"고 했다.
확인 결과 **그런 구조는 리포에도 docs/03 에도 없다.**

```
$ grep -rn "LogEvent" --include=*.hpp --include=*.cpp --include=*.md .
(히트 0)
```

없는 것을 있다고 쓰면 docs/04 6절의 규칙("테스트가 덮지 않는 것을 됨으로 옮기지 않는다")을
문서가 먼저 어기는 것이다. 그래서 **가장 가까운 실재하는 둘**을 대신 본다.

| 실재하는 것 | 정의 위치 | 성격 | 이 규약에서의 쓰임 |
|---|---|---|---|
| `JournalRecord::Note = 3` | `include/mari/sigan/journal.hpp` | UTF-8 메모. 주석이 *"증거가 아니라 기록이다"* 라고 못박음 | **보조**. 사람이 읽을 부연만 |
| `ProofStrokeSummary` | `include/mari/sigan/prooflog.hpp` | 저널 프레임에서 **뽑아낸** 획 요약 | **파생물**. 여기서 새로 만들지 않는다 |

결론: **`Note` 에 비-스트로크 연산을 기록하면 안 된다.**
`Note` 는 프레임 스트림 밖이고, 프레임 스트림 밖은 **서명 정본 밖**이다.
그건 docs/03 8절이 경고한 `:src` 사고 — *"붙여넣기가 있었다"는 서명 안이었는데
"어디서 왔나"는 서명 밖이라 web 을 self 로 고쳐도 서명이 멀쩡했던* — 를 그대로 반복하는 것이다.

**하중을 받는 사실은 전부 프레임 안에 있어야 한다.** 그게 이 문서 전체를 관통하는 원칙이다.

---

## 1. 결정 ① — 비-스트로크 연산은 **합성 프레임 쌍**으로 기록한다

### 결정

`fill` · `erase` · `gradient`(그리고 앞으로 생길 모든 영역 직접쓰기)는
**정확히 2개의 스트로크 프레임**으로 발행한다.

| 프레임 | flags | `cx, cy` | 뜻 |
|---|---|---|---|
| 1번째 | `Down \| Synthetic` (+ erase 면 `Eraser`) | 영향 영역의 **좌상단** `(r.x, r.y)` | 연산 시작 |
| 2번째 | `Up \| Synthetic` (+ erase 면 `Eraser`) | 영향 영역의 **우하단** `(r.right()-1, r.bottom()-1)` | 연산 끝 |

- `pressure` · `tilt*` · `rotation` · `velocity` = **0 고정.** 필압이 없었으니 0 이다. 지어내지 않는다.
- `origin` · `agentId` · `layerId` · `brushId` · `seq` · `t` 는 스트로크와 **똑같이** 채운다.
- 두 프레임 사이에 `JournalRecord::StrokeEnd` 를 찍는다 → 크래시 복구 단위가 스트로크와 같아진다.

새 플래그 하나를 더한다. **꼬리에만 더하므로 docs/03 5.5 규칙 1("필드는 더하기만 한다")을 지킨다.**

```cpp
enum class FrameFlag : u32 {
    Down = 1u << 0,
    Move = 1u << 1,
    Up   = 1u << 2,
    Eraser = 1u << 3,
    Synthetic = 1u << 4,   // ← 새로 더함. "이건 붓질이 아니다"
};
```

프레임 크기는 **64바이트 그대로**다. `flags` 는 이미 u32 이고 비트 4는 비어 있었다.
`StrokeFrameWire` 의 오프셋은 한 바이트도 움직이지 않는다 → `origin_in_signature` 가 깨지지 않는다.

### 왜 이 모양인가

- **영향 영역이 프레임 바이트 안에 들어간다.** 두 점이 사각형을 결정한다.
  새 필드를 만들지 않고도 "얼마나 넓게 칠했나"가 **서명될 정본 안**에 남는다.
- **`Synthetic` 비트가 서명 안에 있다.** 사후에 이 비트를 지워 붓질로 둔갑시키면 서명이 깨진다.
  비트가 서명 밖이었다면 아무 의미가 없다(다시 `:src` 교훈).
- **Sigan 이 모르는 비트를 만나도 깨지지 않는다.** docs/03 5.5 규칙 2 — 모르는 것은 무시.
  구버전 Sigan 은 이걸 그냥 2점짜리 짧은 획으로 본다. 기록이 사라지지는 않는다.

### 탈락한 후보 셋 — 왜 안 되는가

| 후보 | 탈락 이유 |
|---|---|
| **(b) 별도 이벤트 종류로 기록** | 프레임 스트림 **밖**이다 = 서명 정본 밖이다. `:src` 사고의 재현. 그리고 채널이 둘로 갈리면 `seq` 하나로 순서를 증명하던 성질(docs/03 5.3)이 깨진다 |
| **(c) 영향 영역을 훑는 가상 스트로크로 변환** | **반대 방향의 거짓말.** `fill` 한 번이 "AI 획 500개"가 된다. 일어나지 않은 붓질을 지어내는 것이고, 분모까지 부풀려 비율을 망친다. Mari 가 증거를 **창작**하기 시작하는 순간이다 |
| **(d) 기록하지 않고 리포트에만** | **가장 큰 거짓말.** 픽셀은 바뀌었는데 정본에 흔적이 없다. 리포트는 서명되지 않으므로 지우면 그만이다. 캔버스 전체를 AI 가 칠하고도 인증서상 "AI 개입 없음"이 된다 |
| **(a) 합성 프레임 1개로** | 한 개면 영향 영역을 담을 자리가 없다(점 하나는 사각형이 아니다). 그리고 `Down` 만 있고 `Up` 이 없는 프레임은 저널 복구 규약(획 끝마다 flush)을 깬다 |

**(a) 를 "쌍"으로 고친 것이 채택안이다.** 원안의 정신은 맞았고, 개수만 틀렸다.

### 예시

```
AI 가 1024×1024 캔버스의 배경 레이어를 fill 한다.

seq 41  t=1203.5  cx=0     cy=0     p=0  layer=2  flags=Down|Synthetic  origin=agent  agentId=0x9f3a...
seq 42  t=1203.5  cx=1023  cy=1023  p=0  layer=2  flags=Up|Synthetic    origin=agent  agentId=0x9f3a...
        → StrokeEnd(42)
Note: {"op":"fill","layer":2,"color":"#f2e9dc","tiles":256}   ← 부연일 뿐. 여기 없어도 사실은 산다
```

---

## 2. 결정 ② — 집계 단위는 **셋**이다. 획 수만으로는 정직할 수 없다

### 결정

출처별로 **세 축**을 센다. 하나도 빼지 않고, 하나로 합치지도 않는다.

| 축 | 단위 | 세는 것 | 왜 필요한가 |
|---|---|---|---|
| **A. 붓질 수** | 개 | `Synthetic` 이 **없는** 획(= 진짜 스트로크) | "몇 번 그었나". docs/05 3.2 의 원래 숫자 |
| **B. 영역 연산 수** | 개 | `Synthetic` 이 **있는** 획(= fill/erase/gradient) | A 에 섞으면 fill 한 번이 붓질 한 번으로 보인다 |
| **C. 영향 면적** | **변경된 타일 수**(64×64) | A·B 가 실제로 바꾼 타일 | "얼마나 칠했나". A·B 만으로는 안 나온다 |

면적 단위를 **픽셀이 아니라 타일**로 잡은 이유:

- 타일맵이 이미 정확히 추적하고 있다(`DirtyTiles` · `TileSnapshotCommand::changedTileCount()`).
  새로 재지 않으므로 핫 패스 비용이 0 이다.
- 정수라 부동소수 오차가 없고, **안티에일리어싱 가장자리로 장난칠 수 없다.**
- 덮어쓴 타일만 센다 — 같은 자리를 100번 칠해도 면적은 안 는다. 부풀리기가 안 먹힌다.

### 왜 셋 다인가

**획 수만 세면:** AI 의 `fill` 한 번 = 1. 사람의 세밀한 선화 1,847획 = 1,847.
→ "AI 0.05%" 라고 찍힌다. **캔버스 전체가 AI 색인데도.** 거짓이다.

**면적만 세면:** 배경 그라데이션 하나가 캔버스의 90%를 덮는다.
사람이 그 위에 그린 선화 1,847획은 얇아서 타일 몇 개다.
→ "AI 90%" 라고 찍힌다. **사람의 노동이 지워진다.** 이것도 거짓이다.

**둘 다 참이고 둘 다 불완전하다.** 그래서 둘 다 보여준다. 고르지 않는다.
고르는 순간 Mari 가 해석을 하는 것이고, 해석은 Sigan 몫이다(docs/03 2절).

### docs/05 3.2 의 문구는 **고쳐야 한다**

지금 적힌 것:

```
이 작품: 사람 획 1,847 / AI 획 213 (10.3%)
         AI 개입: 밑색 채우기, 배경 그라데이션
```

이 규약에서 정직한 형태:

```
이 작품
  붓질      사람 1,847  /  AI 213          (AI 10.3%)
  영역 연산 사람 0      /  AI 2            (fill 1, gradient 1)
  변경 타일 사람 4,210  /  AI 18,364       (AI 81.3%)
  AI 식별자 claude-opus-5
```

**아래 두 줄이 없으면 위 한 줄은 거짓말에 가깝다.**
`(10.3%)` 하나만 내보내는 인증서를 Mari 가 만들어서는 안 된다.

> 🔴 **등급은 여전히 만들지 않는다.** 위 표에 `ai-assisted` 같은 문자열은 없다.
> 숫자 여섯 개와 식별자 하나까지가 Mari 의 몫이다(docs/03 2절 · `origin.hpp` 주석).

---

## 3. 결정 ③ — 사람 획 경로: **발행 지점을 하나로 못박는다**

### 문제

지금 GUI 가 없어서 사람 획이 들어올 입구가 없다(docs/04 3절).
나중에 WM_POINTER 가 붙을 때 **두 번째 발행 경로가 생기면 기록이 갈라진다**(docs/05 1절 원칙).
그때 가서 맞추려 하면 늦는다 — 그래서 지금 자리를 잡는다.

### 결정

**`SiganPublisher::publish()` 를 부르는 코드는 리포 전체에 딱 한 곳이다.**
그 한 곳을 `mari::agent::IStrokeRecorder` 의 유일한 구현체 안에 둔다.

```
  [사람: WM_POINTER]          [AI: agent-api / MCP / CLI]
            │                            │
            │  StrokeSource::humanPen()  │  AgentStrokeGate::source()
            ▼                            ▼
        ┌───────────────────────────────────┐
        │  brush::StrokeContext(source)      │   ← 출처는 여기서 한 번 박히고 끝
        │  brush::StrokePipeline             │   ← 사람도 AI 도 같은 파이프라인
        └────────────────┬──────────────────┘
                         │ onStrokePoint(source, ...)
        ┌────────────────▼──────────────────┐
        │  IStrokeRecorder  (구현체 1개)      │   ← publish() 호출자는 여기뿐
        └────────────────┬──────────────────┘
                         ▼
                  SiganPublisher
```

강제 규약 넷:

1. **`IStrokeRecorder` 는 `origin` 을 인자로 받지 않는다.** `const StrokeSource&` 만 받는다.
   `StrokeSource` 는 바깥에서 값을 고를 수 없으므로(`origin.hpp`), 레코더도 고를 수 없다.
2. **레코더는 문서(`app::Document`)에 하나 붙는다.** 세션에 붙이지 않는다 —
   세션은 오고 가지만 문서 작업 구간은 이어져야 하기 때문이다(결정 ⑤).
3. **사람 경로가 추가로 하는 일은 없다.** GUI 는 `StrokeSource::humanPen()` 을 만들어
   `StrokeContext` 에 넣는 것으로 끝이다. 발행 코드를 한 줄도 새로 쓰지 않는다.
   쓸 필요가 생겼다면 그건 **설계가 어긋난 신호**다.
4. **비-스트로크 연산도 같은 레코더를 탄다.** `onRegionOp()` 진입점 하나를 더 두고,
   `writeRegionWithUndo()` 가 그것만 부른다.

### 이걸 CI 로 못박는다 (`single_publish_path`)

`no_origin_override` 가 헤더를 훑어 금지 이름을 찾는 것과 **같은 수법**을 쓴다.

```
리포의 *.cpp 를 훑어 `publish(`·`->publish(`·`.publish(` 호출을 센다.
허용 목록: sigan/ 자신, tests/, 그리고 레코더 구현체 파일 하나.
그 밖에서 한 건이라도 나오면 테스트가 깨진다.
```

GUI 가 붙는 날 누군가 지름길을 뚫으면, 그날 빌드가 빨개진다.

---

## 4. 결정 ④ — 발행 실패: **파이프 실패는 성공, 저널 실패는 실패**

### 결정

| 무엇이 실패했나 | agent-api 연산 | 근거 |
|---|---|---|
| **파이프(싱크)** — Sigan 미설치·끊김·`WouldBlock` | ✅ **성공.** 저널에 스풀 | 아래 |
| **저널** — `Journal::failed()` (디스크 가득·권한·I/O) | ❌ **실패.** undo 로 롤백 + 이후 그리기 연산 전부 거절 | 아래 |

### 파이프 실패를 성공으로 보는 근거

**그건 애초에 실패가 아니다.** `SiganPublisher::publish()` 의 순서가 이미 그렇게 설계돼 있다:

```
1. 저널에 먼저 쓴다   ← 파이프 상태와 무관하게 정본은 남는다
2. 파이프가 붙어 있고 순서가 안 밀렸으면 그때 보낸다
3. 못 보낸 건 spooled 로 센다.  published == sent + spooled  (유실 0)
```

- **정본은 저널이다**(`journal.hpp` 첫 주석). 파이프는 전달 수단일 뿐이다.
- docs/03 5.1 은 **Sigan 미설치를 정상 상태로 규정**한다("실패하면 조용히 로컬 모드").
  Sigan 을 안 깐 사용자의 `fill` 을 Mari 가 거절하면 그건 제품이 망가진 것이다.
- docs/03 5.2 는 **"파이프 쓰기는 절대 그리기 스레드를 막지 않는다"** 고 못박았다.
  연산을 실패시키는 건 막는 것보다 더 나쁘다.
- docs/03 4.2 의 "드롭 금지"는 이미 `PublisherStats` 에 `Dropped` 필드가 **없는 것**으로
  지켜지고 있다. 파이프가 막혀도 버려지는 것이 없으므로 실패시킬 이유가 없다.

### 저널 실패를 연산 실패로 보는 근거

여기서만 **진짜로 기록이 사라진다.** 그리고 그때 남는 상태가 최악이다:

> **픽셀은 바뀌었는데 정본에 그 변화가 없다.**
> 나중에 그 .ora 의 prooflog 는 캔버스를 설명하지 못한다.
> 설명하지 못하는 인증서는 **거짓 인증서**다 — 결정 ①의 후보 (d) 와 같은 결과다.

그래서:

1. 저널 append 가 실패하면 그 연산은 **undo 스택으로 롤백**한다(이미 모든 그리기 연산이
   `TileSnapshotCommand` 를 쌓고 있으므로 새 기능이 아니다).
2. 세션에 **기록 불능** 플래그를 세우고, 이후 모든 그리기 연산을
   `ErrorCode::Internal` 로 **거절**한다. 응답에 이유를 그대로 적는다.
3. 자동 복구는 하지 않는다. 조용히 다시 그려지기 시작하면 그 사이가 구멍이 된다.

**"기록 없이 그릴 수 있는 모드"는 만들지 않는다.** 만들면 그게 뒷문이다.

### 예외가 아닌 것 하나

`PublisherStats::idTruncations`(u64 식별자를 u32 로 좁히다 잘린 횟수)는 **실패가 아니다.**
이미 정직하게 세고 있으므로(docs/04 4절 1번) 응답에 숫자로 실어 보내고 연산은 성공시킨다.

---

## 5. 결정 ⑤ — 세션 경계: **agent-api 세션은 Segment 를 만들지 않는다**

### 문제

`AgentSession::open()` / 소멸이 Sigan `Segment` 를 쪼개면,
MCP 클라이언트가 재시작할 때마다 작품이 여러 구간으로 갈라진다.

docs/03 5.3 은 이미 답을 줬다 — **재연결이 새 Segment 를 만들면 안 된다.**
Sigan 의 `Segment` 는 *"전원off·다른기기·다른작업"* 의 경계이고,
에이전트가 재접속한 건 작가 입장에서 **아무 일도 아니다.** 재시작한 Sigan 과 똑같다.

### 결정 — 구간을 만드는 것은 **문서**다

| 사건 | 저널 | `seq` | Segment 에 미치는 영향 |
|---|---|---|---|
| `doc.create` / `doc.open` | **연다**(새 파일) | 1 부터 | 구간 시작 |
| `doc.close` (저장 후) | **닫는다** | — | 구간 끝 |
| `AgentSession::open()` | 그대로 | **이어진다** | **없음.** `Note` 만 남긴다 |
| 세션 소멸 · 클라이언트 재시작 | 그대로 | **이어진다** | **없음.** `Note` 만 |
| 에이전트 교체(다른 `agentId`) | 그대로 | **이어진다** | **없음.** 프레임의 `agentId` 가 바뀔 뿐 |
| 파이프 끊김 · 재핸드셰이크 | 그대로 | **이어진다** | **없음**(docs/03 5.3 · 이미 구현됨) |
| 사람 GUI 가 같은 문서를 이어 그림 | 그대로 | **이어진다** | **없음.** `origin` 이 바뀔 뿐 |

따라서:

- **저널 하나 = 문서 작업 구간 하나.** 한 세션이 문서 셋을 열면 저널이 셋이다.
  한 문서를 세션 셋이 이어 쓰면 저널은 **하나**다.
- `PublisherConfig::sessionId` 의 뜻을 **"문서 작업 구간 id"** 로 확정한다.
  지금 이름이 `sessionId` 라 agent-api 세션과 헷갈리기 쉽다 —
  **둘은 다른 것이다.** agent-api 세션 id 는 `Note` 로만 남고 와이어에 나가지 않는다.
- **Segment 판정 자체는 Mari 가 하지 않는다.** Mari 가 주는 것은 `seq` 연속성과 구간 id 뿐이고,
  그걸 보고 구간을 가르는 것은 Sigan 이다(docs/03 2절).

### 왜 세션이 아니라 문서인가

작가가 증명하려는 것은 **작품 하나가 만들어진 과정**이다. 도구가 몇 번 재접속했는지가 아니다.
문서는 작품과 1:1 이고, 세션은 아니다. 그래서 문서가 경계다.

---

## 6. 🔴 정직성 기준 — 무엇을 기록하면 인증서가 거짓이 되는가

이 절이 이 문서의 결론이다. **아래 다섯 줄 중 하나라도 어기면 인증서는 거짓이다.**

| # | 금지 | 어겼을 때 인증서가 하는 거짓말 |
|---|---|---|
| **H1** | **바뀐 픽셀 중 정본에 흔적 없는 것이 있으면 안 된다** | "이 캔버스는 기록된 과정의 결과다" — 아니다. 기록 밖에서 바뀐 부분이 있다 |
| **H2** | **일어나지 않은 획을 만들어내면 안 된다** | "213번 그었다" — 아니다. 한 번 칠한 것을 213으로 쪼갰다 |
| **H3** | **한 번의 광역 연산을 한 번의 붓질과 같은 칸에 세면 안 된다** | "AI 10.3%" — 아니다. 면적으로는 81%다. 같은 칸에 세는 순간 단위가 거짓이 된다 |
| **H4** | **하중을 받는 사실이 서명 정본 밖에 있으면 안 된다** | "이 사실은 검증됐다" — 아니다. 서명 밖이라 사후에 고칠 수 있다(docs/03 8절 `:src` 사고) |
| **H5** | **Mari 가 등급을 자칭하면 안 된다** | "이 작품은 ai-assisted 다" — Mari 는 판정할 자격이 없다(docs/03 2절). 숫자까지가 몫이다 |

각 결정이 어느 기준을 지키는지:

| 결정 | H1 | H2 | H3 | H4 | H5 |
|---|---|---|---|---|---|
| ① 합성 프레임 쌍 | ✅ 영역 연산도 프레임이 남는다 | ✅ 2개지 500개가 아니다 | ✅ `Synthetic` 으로 갈린다 | ✅ 영역·플래그가 프레임 안 | — |
| ② 세 축 집계 | — | — | ✅ 칸이 셋이다 | — | ✅ 숫자만 낸다 |
| ③ 단일 발행 경로 | ✅ 기록 없는 그리기 경로가 없다 | — | — | — | — |
| ④ 저널 실패 = 연산 실패 | ✅ 기록 없이 픽셀이 안 바뀐다 | — | — | — | — |
| ⑤ 문서 = 구간 | ✅ 구간이 갈려 과정이 끊기지 않는다 | — | — | — | ✅ 구간 판정은 Sigan 몫 |

### 반례 모음 — "이렇게 하면 거짓이 된다"

```
❌ fill 을 Note 로만 남긴다
   → Note 는 서명 정본 밖이다. 지우면 그만이다. (H1 · H4)

❌ fill 을 Down|Up 없이 프레임 1개로 남긴다
   → 영향 영역을 담을 자리가 없다. "칠했다"는 남고 "얼마나"는 사라진다. (H3)

❌ fill 을 영역 픽셀 수만큼의 가상 스트로크로 편다
   → 1,048,576획짜리 AI 가 태어난다. 사람 획이 통계적으로 소멸한다. (H2)

❌ 파이프가 끊겼다고 fill 을 실패시킨다
   → 거짓은 아니지만 제품이 망가진다. Sigan 미설치가 정상인데(docs/03 5.1) 그리기가 죽는다.

❌ 저널이 실패했는데 그리기를 계속 허용한다
   → 이후 모든 픽셀이 기록 밖이다. 가장 조용하고 가장 큰 거짓. (H1)

❌ prooflog 에 "AI 10.3%" 한 줄만 싣는다
   → 참인 숫자 하나로 거짓인 인상을 만든다. 세 축을 다 싣는다. (H3)

❌ 세 축을 하나로 합친 "AI 기여도 45%" 를 만든다
   → 가중치를 고르는 순간 그건 판정이다. Mari 의 몫이 아니다. (H5)
```

---

## 7. 이 규약이 손대는 것 — 2단계 작업 목록

> 이 문서는 **결정**이고, 아래는 그 결정을 코드로 옮길 때의 범위다. 코드는 2단계다.

| # | 파일 | 변경 | 위험 |
|---|---|---|---|
| 1 | `include/mari/sigan/frame.hpp` | `FrameFlag::Synthetic = 1u << 4` 추가 | **없음.** 크기·오프셋 불변 |
| 2 | `include/mari/agent/recording.hpp` | **신규.** 선언만(아래 8절) | 없음 |
| 3 | `agent/src/recording.cpp` | **신규.** `IStrokeRecorder` 유일 구현체 | `publish()` 호출자는 여기뿐 |
| 4 | `agent/CMakeLists.txt` | `mari::sigan` 링크 | docs/04 4절 7번의 빈칸이 메워진다 |
| 5 | `include/mari/agent/session.hpp` | `countStroke()` → 세 축 집계로 확장 | `cli/src/runner.cpp:43` 이 같이 바뀐다 |
| 6 | `agent/src/ops_draw.cpp` | 4곳의 `countStroke()` 호출부 정리 | 기존 응답 필드는 **빼지 않는다**(더하기만) |
| 7 | `include/mari/sigan/prooflog.hpp` | 요약에 `synthetic` · `tiles` 추가 | `test_prooflog.cpp` 의 부재 검사는 그대로 통과 |

**경계선 확인(docs/03 2절):** 위 어디에도 ES256 · 해시체인 봉인 · `prevHash` · 신뢰 등급이 없다.
새로 계산하는 것은 **개수와 타일 수뿐**이다. `test_prooflog.cpp` 와 `test_origin.cpp` 의
부재 강제는 그대로 선다.

**위조 방지 확인:** `StrokeSource() = delete` 그대로. origin 세터 없음.
`StrokeSource` 생성 지점 **6곳 그대로**(팩토리 4 + 게이트 1 + private 생성자 1).
`friend` 는 `agent::AgentStrokeGate` **하나 그대로**.
`IStrokeRecorder` 는 `StrokeSource` 를 **만들지 않고 받기만 한다.**

### 새로 필요한 테스트

> 🔴 **아래는 계획 당시의 가칭이다.** 실제로 들어간 이름은 11절 표에 있다 —
> 이 칸을 고치지 않고 두면 다음 사람이 없는 테스트를 찾게 된다.

| 테스트(계획 당시 가칭) | 통과 조건 |
|---|---|
| `single_publish_path` | `publish(` 호출자가 `sigan/` · `tests/` · 레코더 구현체 밖에 **0곳** |
| `synthetic_is_flagged` | `fill`/`erase`/`gradient` 후 저널에 `Synthetic` 프레임이 **정확히 2개**, 사각형이 영향 영역과 일치 |
| `synthetic_not_counted_as_stroke` | 같은 상황에서 붓질 수가 **0**, 영역 연산 수가 **1** |
| `area_is_counted` | 변경 타일 수가 실제 `changedTileCount()` 와 일치 |
| `journal_failure_rejects_draw` | 저널을 고장 내면 `fill` 이 실패하고 캔버스가 **원상복구**된다 |
| `pipe_failure_still_draws` | 싱크가 `Disconnected` 여도 `fill` 이 성공하고 `spooled` 가 는다 |
| `session_restart_keeps_segment` | 세션을 닫고 다시 열어도 같은 문서면 `seq` 가 이어지고 저널이 하나다 |

---

## 8. 선언만 두는 인터페이스

`include/mari/agent/recording.hpp` 에 **선언만** 둔다. 구현은 2단계다.
이 헤더가 지금 존재하는 이유는 하나 — **발행 지점이 하나라는 약속에 이름을 붙여 두기 위해서다.**

요지:

```cpp
class IStrokeRecorder {
public:
    virtual ~IStrokeRecorder() = default;

    // 출처는 인자로 "받기만" 한다. 고르지 않는다.
    virtual void onStrokePoint(const StrokeSource& src, const StrokePointRecord& p) noexcept = 0;
    virtual void onStrokeEnd(const StrokeSource& src, u32 changedTiles) noexcept = 0;

    // 비-스트로크 연산 — 합성 프레임 쌍으로 나간다(결정 ①).
    [[nodiscard]] virtual Result<void> onRegionOp(const StrokeSource& src,
                                                  const RegionOpRecord& op) = 0;

    [[nodiscard]] virtual bool recordingBroken() const noexcept = 0;  // 결정 ④
    [[nodiscard]] virtual const RecordingTally& tally() const noexcept = 0; // 결정 ②
};
```

`onRegionOp` 만 `Result` 를 돌려주는 것은 결정 ④ 때문이다 —
**저널이 고장 났으면 호출자가 롤백해야 하므로 알아야 한다.**
반대로 `onStrokePoint` 는 핫 패스라 `noexcept` 이고, 고장은 `recordingBroken()` 으로 본다.

---

## 9. 이 문서를 고치는 규칙

- 결정을 바꾸려면 **6절의 어느 기준을 지키려고 바꾸는지**를 같이 쓴다.
- "그게 더 편해서"는 이유가 아니다. 인증서를 보는 사람이 속지 않는가만 본다.
- 구현이 이 문서와 어긋나면 **구현이 틀린 것이다.** 단, 구현해 보고 여기가 틀렸다고
  판명되면 이 문서를 고친다 — 조용히 어기지 않는다.

---

## 10. 구현하면서 이 문서가 틀렸던 곳 (9절 규칙에 따른 정정)

> 9절: *"구현이 이 문서와 어긋나면 구현이 틀린 것이다. 단, 구현해 보고 여기가 틀렸다고
> 판명되면 이 문서를 고친다 — 조용히 어기지 않는다."* 아래가 그 기록이다.
> **다섯 결정은 하나도 바뀌지 않았다.** 바뀐 것은 파일 위치와 이름 넷이다.

| # | 7절에 적혀 있던 것 | 실제로 한 것 | 어느 기준을 지키려고 |
|---|---|---|---|
| 1 | 구현체를 `agent/src/recording.cpp` 에 두고 `agent/CMakeLists.txt` 가 `mari::sigan` 링크 | 구현체는 **`record/src/sigan_recorder.cpp`**. `agent` 도 `app` 도 sigan 을 링크하지 않는다 | 의존 방향. agent 가 sigan 을 직접 알면 **Sigan 없이는 못 그리는 빌드**가 된다 — docs/03 5.1 이 정상이라고 규정한 상태에서 제품이 죽는다. 결정 ③ 이 요구한 것은 *"구현체가 하나"* 이지 그 파일의 위치가 아니다. 새 테스트 `neither_app_nor_agent_links_sigan` 이 이 방향을 강제한다 |
| 2 | 저널 실패 시 `ErrorCode::Internal` 로 거절 | `ErrorCode::IoError` 로 거절 | `ErrorCode` 에 `Internal` 이 **없다.** 없는 것을 있다고 쓰지 않는다(0.1절과 같은 이유). 저널 쓰기 실패는 실제로 I/O 오류다 |
| 3 | `RegionOpKind` 는 fill·erase·gradient 셋 | **`Transform = 4` 를 꼬리에 더했다** | H1. `transform` 도 영역을 직접 쓴다 — 기록하지 않으면 기록 밖에서 바뀐 픽셀이 생긴다. 결정 ① 의 *"앞으로 생길 모든 영역 직접쓰기"* 가 이 경우다 |
| 4 | 획 요약에 `synthetic` · `tiles` 추가 | `synthetic` 과 **`area`**(합성 쌍의 두 점에서 계산)를 획 요약에 넣고, 타일 수는 **로그 최상단의 `changedTiles` 축**으로 뺐다 | 타일 수는 프레임에 자리가 없어 기록한 쪽이 건네줘야 한다. 그걸 획마다 붙이면 "프레임에서 나온 사실"과 섞인다. 분리하고 `"measured": true/false` 를 같이 실어 **안 잰 것을 0 으로 적어 놓고 잰 척하지 않는다** |

### 덤으로 드러난 진짜 버그 하나 — `Journal::flush()`

구현 중 `/dev/full` 로 저널 고장을 재현하다 찾았다. 고치기 전의 `flush()` 는

- `fwrite` 가 **부분 쓰기**를 해도(디스크가 도중에 찼을 때) `written == 0` 이 아니면 성공으로 봤고,
- `fflush` 가 실패하면 `false` 를 돌려주면서도 **`failed_` 를 세우지 않았다.**

즉 `Journal::failed()` 는 계속 false 였고, 결정 ④ 의 "저널 실패 = 연산 실패"가
**애초에 발동할 수 없었다.** 쓴 줄 알았는데 아무 것도 안 남은 상태로 계속 그려졌을 것이다 —
6절 H1 이 말하는 *"가장 조용하고 가장 큰 거짓"* 그 자체다. 둘 다 `failed_` 를 세우도록 고쳤고,
`a_real_unwritable_journal_is_detected` 가 실제 파일로 그것을 검사한다.

### 7절 표에 더해진 파일들

| 파일 | 몫 |
|---|---|
| `include/mari/app/stroke_entry.hpp` · `app/src/stroke_entry.cpp` | **사람·AI 공용 입구.** `StrokeEntry` 와 `recordRegionOp()` 하나씩 |
| `include/mari/record/sigan_recorder.hpp` · `record/src/sigan_recorder.cpp` | `IStrokeRecorder` 의 **유일한 구현체** + 문서마다 구간을 여는 공장 |
| `tests/record/test_recording.cpp` | 종단 증명 8건(1~8) |
| `tests/record/test_single_publish_path.cpp` | `single_publish_path` + 의존 방향 |
| `tests/cli/test_proof_out.cpp` | `--proof-out` · `--journal-dir` · MCP `doc_origins` |

### 아직 기록 밖인 것 — 정직하게 남겨 둔다

H1("기록 밖에서 바뀐 픽셀 금지")을 **완전히** 지키려면 아래도 기록해야 한다.
지금은 아니다. 되는 척하지 않기 위해 여기 적어 둔다.

- `doc.importPixels` / 붙여넣기(`PasteSource`) — 픽셀이 들어오지만 프레임이 없다.
  (이벤트로는 보고된다. 그러나 이벤트는 서명 정본이 아니다.)
- `layer.merge` · `layer.remove` — 합성 결과가 픽셀을 바꾸는데 프레임이 없다.
- 실행취소/다시실행 — 캔버스가 과거로 갔다는 사실이 기록에 남지 않는다.

셋 다 "영역 직접쓰기"로 볼 수 있으므로 결정 ① 의 틀을 그대로 쓸 수 있다.
다음 단계의 일이다.

---

## 11. 규약 ↔ 실제 테스트 이름 대조표 (2026-09-17 재검증)

7절 표의 이름은 **계획 당시의 가칭**이었다. 실제로 들어간 이름은 아래다.
왼쪽 칸을 grep 해도 안 나온다 — 오른쪽을 grep 해라.

| 7절의 가칭 | 실제 테스트 | 파일 |
|---|---|---|
| `single_publish_path` | `single_publish_path` (그대로) | `tests/record/test_single_publish_path.cpp` |
| — (10절 정정 1 이 더한 것) | `neither_app_nor_agent_links_sigan` | 〃 |
| `synthetic_is_flagged` + `synthetic_not_counted_as_stroke` + `area_is_counted` | `fill_is_a_synthetic_frame_pair_not_a_brush_stroke` (셋이 하나로 합쳐졌다 — 같은 상황을 세 번 세팅할 이유가 없다) | `tests/record/test_recording.cpp` |
| `journal_failure_rejects_draw` | `journal_failure_rejects_the_draw_and_rolls_back` · `a_real_unwritable_journal_is_detected` | 〃 |
| `pipe_failure_still_draws` | `pipe_failure_still_draws_and_drops_nothing` | 〃 |
| `session_restart_keeps_segment` | `a_new_session_on_the_same_document_does_not_split_the_segment` | 〃 |
| — (규약이 요구했으나 7절이 빠뜨린 것) | `agent_strokes_reach_the_publisher_as_agent` · `human_pen_path_reaches_the_same_publisher` · `mixed_human_and_agent_counts_are_exact_in_the_prooflog` · `agent_api_works_without_any_recorder` | 〃 |
| — (CLI·MCP 표면) | `proof_out_writes_all_three_axes` · `without_journal_dir_drawing_works_and_says_it_is_not_recorded` · `mcp_exposes_the_origin_tally_tool` | `tests/cli/test_proof_out.cpp` |

### 11.1 진짜 바이너리로 돌린 결과 — 규약이 실제로 지켜지는가

테스트는 인프로세스다. **실행파일로도 돌려 봤다.** 1024² 캔버스에
`fill`(전면) · 붓질 3 · `gradient` 를 넣고 `--journal-dir` · `--proof-out` 을 준 결과:

```json
"originCounts": {"humanPen": 0, "humanMouse": 0, "agent": 3, ...},
"humanStrokes": 0, "agentStrokes": 3, "agentStrokeRatio": 1.000,
"regionOpCounts": {..., "agent": 2, ...}, "agentRegionOps": 2,
"changedTiles": {..., "agent": 326, ..., "measured": true},
"frameCount": 13, "seqFirst": 1, "seqLast": 13, "lostFrames": 0,
"grade": "unsigned", "signed": false,
"strokes": [
  {"seqFirst": 1,  "seqLast": 2,  "points": 2, "synthetic": true,
   "area": {"x": 0, "y": 0,   "w": 1024, "h": 1024}, "origin": "agent"},
  {"seqFirst": 3,  "seqLast": 5,  "points": 3, "synthetic": false, "origin": "agent"},
  {"seqFirst": 6,  "seqLast": 8,  "points": 3, "synthetic": false, "origin": "agent"},
  {"seqFirst": 9,  "seqLast": 11, "points": 3, "synthetic": false, "origin": "agent"},
  {"seqFirst": 12, "seqLast": 13, "points": 2, "synthetic": true,
   "area": {"x": 0, "y": 900, "w": 1024, "h": 124},  "origin": "agent"}
]
```

6절 기준과 대조:

| 기준 | 확인 |
|---|---|
| **H1** 바뀐 픽셀 중 정본에 흔적 없는 것 | `fill`·`gradient` 둘 다 프레임 쌍으로 남았다. `seq` 1~13 에 구멍 없음(`lostFrames: 0`) |
| **H2** 일어나지 않은 획 | 전면 `fill` 이 프레임 **2개**다. 1,048,576 개도, 500 개도 아니다 |
| **H3** 광역 연산과 붓질을 같은 칸에 | `agentStrokes: 3` 과 `agentRegionOps: 2` 가 **다른 키**다. 면적은 셋째 축 `changedTiles: 326` |
| **H4** 하중을 받는 사실이 서명 정본 밖 | `synthetic` 비트와 두 점의 좌표가 전부 64바이트 프레임 안이다. `area` 는 그 두 점에서 **계산한 값**이지 따로 저장한 값이 아니다 |
| **H5** Mari 가 등급을 자칭 | `"grade": "unsigned"` 하나. `ai-assisted`·`human-only`·`ai-generated` 문자열 **0건**. `ES256`·`prevHash` 도 0건 |

사람·AI 를 한 문서에 섞은 실측(사람 붓질 12 / AI 붓질 4 / AI `fill` 1):

```json
"originCounts": {"humanPen": 12, "humanMouse": 0, "agent": 4, ...},
"humanStrokes": 12, "agentStrokes": 4, "agentStrokeRatio": 0.250,
"humanRegionOps": 0, "agentRegionOps": 1,
"changedTiles": {"humanPen": 24, "humanMouse": 0, "agent": 267, ..., "measured": true},
"frameCount": 86, "seqFirst": 1, "seqLast": 86, "lostFrames": 0
```

**붓질로는 AI 25%, 면적으로는 AI 91.7%.** 결정 ② 가 "하나로 합치지 않는다"고 한 이유가
이 두 숫자다. 둘 중 하나만 실었다면 그게 6절 H3 의 거짓말이다.

> 🔴 **여기서 사람 획 12개는 `app::StrokeEntry` 에 `PenSample` 을 직접 넣어 만든 것이다.**
> 결정 ③ 이 검증한 것은 *"사람 경로와 AI 경로가 같은 발행 지점 하나를 지난다"* 까지다.
> **실제 펜 하드웨어에서 들어오는 경로는 없다** — Windows Ink(WM_POINTER)는
> 컴파일된 적이 없고 GUI 도 없다(docs/04 2절·3절). 규약 3 이 약속한 대로,
> 그것이 붙는 날 새로 쓸 발행 코드는 0줄이다.
