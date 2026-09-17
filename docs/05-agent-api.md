# Mari Paint — 에이전트 API 설계

> **"AI가 쉽게 쓸 수 있어야 한다. MCP나 헤드리스보다 더."**
> 이 문서는 그 "더"가 무엇인지를 정의한다.

---

## 1. MCP와 헤드리스는 출발선이지 목표가 아니다

| | 그게 주는 것 | 그게 못 주는 것 |
|---|---|---|
| **MCP** | AI가 함수를 호출할 수 있다 | AI가 **결과를 볼 수 없다** |
| **헤드리스** | GUI 없이 돌아간다 | 그냥 배관이다. 능력이 아니다 |

둘 다 **한다.** 하지만 그건 기본값이다. MCP는 전송 규약일 뿐이고, 몇 년 뒤 다른 게 표준이 될 수도 있다.
**진짜 자산은 능력 계층이고, MCP는 그 위에 씌우는 얇은 어댑터여야 한다.**

```
                  mari-core  (타일 캔버스 · 스트로크 · 레이어)
                        │
              ┌─────────▼──────────┐
              │   mari-agent-api   │   ← 진짜 자산. 여기에 능력이 있다
              │  세션·스냅샷·시각   │
              │  피드백·시맨틱주소  │
              └─────────┬──────────┘
        ┌───────┬───────┼────────┬─────────┐
     MCP 서버  COM    CLI      WS/JSON-RPC  UI
     (AI)   (Sigan) (헤드리스)  (원격)   (사람)
```

**사람 UI도 같은 계층을 쓴다.** 이게 핵심이다 — AI 전용 뒷문을 따로 만들면
"사람은 되는데 AI는 안 되는 것"이 생기고, 기록도 두 갈래가 된다.

---

## 2. "더"의 정체 — 8가지

### 2.1 🔴 AI는 자기가 그린 걸 볼 수 있어야 한다

**이게 제일 크다.** 눈 없는 AI는 그림을 못 그린다.
MCP 도구를 아무리 많이 줘도 결과를 못 보면 장님이 붓질하는 것이다.

모든 API 호출이 **시각 피드백을 같이 반환**할 수 있어야 한다.

```jsonc
// 요청
{ "op": "stroke", "points": [...], "view": { "mode": "dirty", "max": 512 } }

// 응답 — 그리고 바뀐 부분을 바로 보여준다
{ "ok": true, "dirtyRect": [120, 80, 340, 290],
  "image": { "png": "<base64>", "w": 220, "h": 210, "scale": 1.0 } }
```

**타일 캔버스라서 이게 공짜다.** 더티 타일을 이미 추적하고 있으므로
"바뀐 부분만" 잘라서 돌려주면 된다. 헤드리스 앱을 스크린샷 찍는 것보다
**증분적이고 의미가 있다.**

| `view.mode` | 반환 |
|---|---|
| `none` | 이미지 없음 (빠름) |
| `dirty` | **바뀐 영역만** — 기본값 |
| `full` | 캔버스 전체 (축소 가능) |
| `layer:<id>` | 특정 레이어만 |
| `region:[x,y,w,h]` | 지정 영역 |

### 2.2 🔴 O(1) 분기 — AI가 실험하고 되돌릴 수 있다

**COW 타일 구조에서 공짜로 나온다.** 이건 다른 툴이 흉내내기 어렵다 —
포토샵 스크립팅으로 하려면 파일을 복사해야 한다.

```
snapshot = doc.snapshot()          // O(1). 메모리 거의 안 씀
for 시도 in [A, B, C]:
    doc.restore(snapshot)          // O(1)
    doc.stroke(시도)
    후보.append(doc.render())      // 보고
doc.restore(snapshot)
doc.apply(가장 좋은 것)
```

AI 에이전트의 작업 방식 자체가 **시도 → 평가 → 되돌리기**다.
그걸 저렴하게 만들어주는 게 "AI가 쉽게 쓴다"의 실체다.

명명 스냅샷(`doc.snapshot("러프 완성")`)과 분기(branch)까지 지원한다.

### 2.3 스트로크로 그린다. 픽셀로 칠하지 않는다

```jsonc
// ❌ 이러면 안 된다 — 합성이지 그림이 아니다
{ "op": "drawLine", "x1": 10, "y1": 10, "x2": 100, "y2": 100 }

// ✅ 사람 펜과 같은 파이프라인을 탄다
{ "op": "stroke",
  "brush": "묵직한 잉크펜",
  "points": [ {"x":10,"y":10,"p":0.2}, {"x":55,"y":48,"p":0.9}, {"x":100,"y":100,"p":0.3} ],
  "smoothing": 0.4 }
```

이게 중요한 이유 셋:
1. **결과가 그린 것처럼 보인다.** 브러시 프리셋(.abr/.sut 임포트 포함)이 그대로 적용된다.
2. 사람과 AI가 **같은 파이프라인**을 쓴다 → 두 번째 구현이 없다.
3. **Sigan이 AI 획도 똑같이 기록한다** → 3절로 이어진다.

필압 커브를 AI가 직접 못 정하면 `pressureProfile: "taper-in-out"` 같은 프리셋도 준다.

### 2.4 시맨틱 주소 — 좌표를 외우게 하지 않는다

AI에게 "레이어 인덱스 3"을 기억시키면 안 된다. 이름과 내용으로 지목하게 한다.

```jsonc
{ "layer": "선화" }                          // 이름
{ "layer": { "role": "sketch" } }            // 역할 태그
{ "region": { "content": "nonEmpty", "layer": "선화" } }   // 내용이 있는 영역
{ "region": { "selection": "current" } }
```

`doc.describe()` 로 캔버스 상태를 **문장으로** 받을 수 있게 한다 —
"1024x1024, 레이어 4개: 배경(채워짐), 러프(12% 채워짐), 선화(비어있음), 채색(비어있음)".
AI가 JSON 트리를 파싱하는 것보다 싸고 정확하다.

### 2.5 트랜잭션 — 왕복 지연이 에이전트를 죽인다

한 획 그리고 응답 기다리고를 반복하면 500획에 500번 왕복이다.

```jsonc
{ "op": "batch",
  "atomic": true,            // 하나라도 실패하면 전부 롤백
  "ops": [ ...200개... ],
  "view": { "mode": "dirty" }   // 결과는 한 번만 렌더
}
```

원자성이 있으면 AI가 **안심하고 큰 묶음을 보낸다.** 실패해도 캔버스가 반쯤 망가지지 않는다.

### 2.6 자기 설명 — AI가 문서를 안 읽어도 된다

```jsonc
{ "op": "capabilities" }
→ { "ops": [...], "brushes": [...], "blendModes": [...],
    "limits": { "maxCanvas": 16384, "maxBatchOps": 1000 } }
```

설치된 브러시(임포트한 .abr/.sut 포함)를 **런타임에 발견**할 수 있어야 한다.
하드코딩된 브러시 목록을 문서로 주면 금방 틀린다.

### 2.7 스트리밍 — 요청/응답만으로는 부족하다

긴 작업(큰 필터, 8bf 플러그인)은 진행률을, 사람이 동시에 그리면 그 사건을 흘려보낸다.
Sigan에 쓰는 `IMariEventSink`와 **같은 이벤트 버스**를 쓴다. 두 번 만들지 않는다.

### 2.8 헤드리스는 기본값 — 같은 바이너리, 같은 코드 경로

```bash
mari-paint --headless --script paint.json
mari-paint --headless --mcp --stdio     # MCP 서버로 기동
mari-paint --headless --serve :7777     # JSON-RPC
```

GUI 없이도 **모든 기능**이 돌아간다. UI가 능력을 갖고 있으면 안 된다 — UI는 얇은 껍데기다.

---

## 3. 🔴 Sigan과의 충돌 — 이걸 안 풀면 Sigan이 죽는다

**Sigan의 존재 이유는 "사람이 그린 과정"을 증명하는 것이다.**
그런데 Mari가 AI에게 붓을 쥐여준다. 이건 정면 충돌이다.

> Mari가 AI 획을 사람 획과 구별 없이 Sigan에 넘기면,
> **Mari Paint는 Sigan 인증서 위조기가 된다.**

이건 부작용이 아니라 **설계 결함**이다. 반드시 막는다.

### 3.1 모든 획은 출처를 달고 다닌다

```cpp
enum class StrokeOrigin {
    HumanPen,      // 필압 있는 실제 펜
    HumanMouse,    // 마우스
    Agent,         // AI/스크립트 — agentId 동반
    Imported,      // 붙여넣기·외부 반입
    Filter,        // 필터·플러그인이 생성
};
```

- `origin`은 **스트로크 프레임에 들어간다**(docs/03 4.1 `flags` 확장).
- **서명 정본(SigPayload)에 포함시킨다.** 서명 밖이면 사후에 고칠 수 있어 아무 의미가 없다.
  — docs/03 8절의 `:src` 선례와 같은 교훈이다. "붙여넣기가 있었다"는 서명 안이었는데
  "어디서 왔나"는 서명 밖이라 web을 self로 고쳐도 서명이 멀쩡했던 그 사고.
- Mari는 origin을 **위조할 수단을 제공하지 않는다.** API에 origin 설정 파라미터가 없다.
  에이전트 API로 들어온 획은 **무조건** `Agent`다.

### 3.2 인증서는 비율을 정직하게 보여준다

```
이 작품: 사람 획 1,847 / AI 획 213 (10.3%)
         AI 개입: 밑색 채우기, 배경 그라데이션
```

숨기지 않는다. 낙인찍지도 않는다. **사실만 적는다.**
Sigan 문서 08의 원칙 그대로 — *"막을 게 아니라 정직하게 드러낸다."*

### 3.3 이게 오히려 기회다

AI 시대에 "AI를 아예 안 썼다"를 증명하는 것보다
**"어디에 얼마나 썼는지"를 증명**하는 게 훨씬 현실적이고 시장이 크다.

| 등급 | 의미 |
|---|---|
| `human-only` | AI 획 0 |
| `ai-assisted` | AI 획 있음, **비율과 용도 공개** |
| `ai-generated` | 대부분 AI |

의뢰인은 "AI 안 씀"이 아니라 **"거짓말 안 함"**을 원한다.
Mari + Sigan은 이걸 줄 수 있는 유일한 조합이 된다.

---

## 4. API 표면 (초안)

> 아래는 **구현된 연산 표 그대로**다(`agent::opTable()`, 37개).
> 이 문서가 정본이 아니다 — **코드의 표가 정본이고 이 표가 그것을 따라간다.**
> `capabilities` 연산이 런타임에 같은 것을 내보내므로, 어긋나면 코드 쪽이 맞다.

| 영역 | 연산 | 비고 |
|---|---|---|
| 문서 | `doc.create` `doc.open` `doc.save` `doc.close` `doc.describe` `capabilities` | `doc.open`/`doc.save` 는 `.ora`(저장은 `.png` 도). `.psd` 는 없다 |
| 스냅샷 | `snapshot` `restore` `branch` `diff` | |
| 레이어 | `layer.list` `layer.add` `layer.remove` `layer.move` `layer.duplicate` `layer.merge` `layer.setProps` | |
| 그리기 | `stroke` `fill` `erase` `gradient` `transform` | `transform` 은 **정수 평행이동만.** `scale`/`rotate` 는 리샘플러가 없어 거절한다 |
| 선택 | `select` `select.expand` / ~~`select.invert`~~ ~~`select.feather`~~ | 🔴 선택 모델이 **사각형 하나뿐**이다. 반전·페더는 마스크 저장소가 없어 **미지원으로 표시**되고 MCP 도구로도 나가지 않는다 |
| 브러시 | `brush.list` `brush.import`(.abr/.sut) `brush.set` `brush.describe` | |
| 시각 | `render` `thumbnail` `compare` | |
| 일괄 | `batch` (원자적) | |
| 이벤트 | `events.subscribe` `events.unsubscribe` `events.poll` | 초안에 `poll` 이 빠져 있었다. 폴링 없이는 큐를 꺼낼 길이 없어 실제로는 필요했다 |

**전부 MCP 도구로 자동 노출된다.** MCP 서버는 이 표를 읽어서 도구 목록을 생성할 뿐,
따로 손으로 유지하지 않는다. (2.6 자기 설명과 같은 뿌리)
단 **미지원 연산은 도구로 내보내지 않는다** — 주면 AI 가 반드시 한 번 불러 보고 실패한다.
그래서 지금 도구는 37개가 아니라 **35개**이고, 빠진 둘의 이유는 `capabilities` 가 말해 준다.

---

## 5. 설계 원칙에 추가

[02-architecture.md](./02-architecture.md) 1절에 6번째 원칙으로 넣는다:

> **6. AI는 일급 사용자다. 단, 정직하게.**
> 사람이 할 수 있는 모든 것을 AI도 할 수 있다 — 같은 명령 계층, 같은 스트로크 파이프라인,
> 같은 기록 경로로. 대신 **모든 획은 출처를 달고 다니고, 그 출처는 서명 안에 들어간다.**
> AI 전용 뒷문도, 출처를 지우는 API도 만들지 않는다.

---

## 6. 검증 (CI)

**8종 전부 실제로 존재하고 ctest 에서 돈다.** 아래 "어디서" 칸이 그 파일이다.

| 테스트 | 통과 조건 | 어디서 |
|---|---|---|
| `agent_can_see` | 획 후 응답에 더티 영역 이미지가 실제로 들어있다 | `tests/agent/test_view.cpp` |
| `snapshot_is_cheap` | 4096² 캔버스 스냅샷 100개 < 50MB, 각 < 1ms | `tests/agent/test_snapshot.cpp` |
| `agent_origin_forced` | 에이전트 API로 들어온 획의 origin이 **무조건** Agent | `tests/agent/test_origin.cpp` · `test_api.cpp` |
| `origin_in_signature` | origin을 바꾸면 **프레임 바이트**가 바뀐다 (위조 불가 증명) | `tests/sigan/test_frame.cpp` |
| `no_origin_override` | API 스키마 어디에도 origin 설정 파라미터가 없다 | `tests/agent/test_origin.cpp` · `test_api.cpp` |
| `headless_parity` | 헤드리스 표면들이 **같은 연산 표 하나**에서 나온다 | `tests/cli/headless_main.cpp` |
| `mcp_tools_generated` | MCP 도구 목록이 API 표와 자동 일치 | `tests/mcp/test_mcp.cpp` |
| `batch_atomic` | 중간 실패 시 캔버스가 원상복구된다 | `tests/agent/test_api.cpp` |

### 6.1 두 항목은 문구를 낮췄다 — 원안대로는 증명할 수 없기 때문이다

**`origin_in_signature`.** 원안은 "**서명** 바이트가 바뀐다"였다. 그런데 Mari 는 서명을
하지 않는다(docs/03 2절). 그래서 실제로 재는 것은 **서명될 프레임 바이트**다 —
origin 하나만 바꾸면 64바이트 프레임에서 **정확히 오프셋 56 한 바이트만** 달라진다.
그 바이트가 Sigan 이 서명할 정본 안에 있으므로, 사후에 고치면 서명이 깨진다.
Mari 가 증명할 수 있는 건 여기까지이고, 그 이상을 주장하면 경계선을 넘는 것이다.

**`headless_parity`.** 원안은 "헤드리스에서 **GUI 와 동일한** 연산 집합"이었다.
그런데 **이 리포에 GUI 가 없다.** 없는 것과 비교할 수는 없다. 그래서 대신
**갈라질 수 없는 구조**를 검사한다 — 진짜 바이너리의 `--capabilities`,
`DISPLAY`·`WAYLAND_DISPLAY` 를 지운 환경의 `--capabilities`, 같은 바이너리의
MCP `tools/list`, 그리고 인프로세스 `opTable()` 이 **전부 같은 표 하나**에서 나오고,
CLI·MCP 소스에 **두 번째 연산 표가 없다**는 것. 나중에 GUI 가 붙어도 같은 표를 읽는 한
parity 는 유지된다. 지금 말할 수 있는 건 그것이고, "GUI 와 맞대 봤다"는 거짓말이다.

---

## 7. 마일스톤 편입

| 단계 | 내용 | 시점 | 현황 (2026-09-17, Linux) |
|---|---|---|---|
| **A0** | `StrokeOrigin` + 서명 포함 | **M1과 동시** — 나중에 넣으면 포맷이 굳는다 | ✅ `include/mari/core/origin.hpp` · 프레임 오프셋 56 |
| **A1** | agent-api 코어 (batch·snapshot·render) | M2 | ✅ `agent/` 10개 소스 · 연산 37개 |
| **A2** | 헤드리스 모드 + CLI | M2 | ✅ `mari-paint` 실행파일이 Linux 에서 실제로 돈다 |
| **A3** | MCP 서버 어댑터 | M3 | ✅ `--mcp --stdio` · 도구 35개가 표에서 생성된다 |
| **A4** | 시맨틱 주소 + `describe` | M4 | ✅ 이름·역할 태그·`content:nonEmpty` · `doc.describe` |
| **A5** | JSON-RPC 원격 | M5 | ⚠️ `--serve` 로 **TCP 루프백 한 줄 왕복**까지. 인증도, 동시 세션도, WebSocket 도 없다 |

**A0를 M1에 못 박는 이유:** origin이 서명 정본에 들어가야 하는데,
Sigan의 서명 포맷은 한 번 배포되면 **하위 호환 때문에 못 바꾼다.**
docs/03 8절이 경고한 그대로 — `:src`를 넣을 수 있었던 건 "아직 릴리스 전이라
프로덕션 실측 0건"이었기 때문이다. **지금이 그 창문이다.**

---

## 8. 🔴 구현이 설계에 못 미치는 곳 (2026-09-17 · Linux)

> 이 절이 없으면 이 문서는 "하려는 것"과 "된 것"을 섞어 버린다.
> 아래는 **설계에 적혀 있는데 아직 코드로 이어지지 않은 것**이다.

### 8.1 에이전트 획은 **아직 Sigan 으로 흘러가지 않는다**

2.3 의 셋째 이유는 이렇게 적혀 있다:

> 3. **Sigan이 AI 획도 똑같이 기록한다** → 3절로 이어진다.

**지금은 아니다.** 확인 방법은 간단하다 —
`grep -rn "Publisher\|publish(" --include=*.cpp .` 를 돌리면
`sigan/` 과 `tests/` 밖에 호출자가 **한 곳도 없다.**
`agent/` `app/` `cli/` `mcp/` 의 CMakeLists 어디도 `mari::sigan` 을 링크하지 않는다.

즉 지금 서 있는 것은 두 조각이고, 둘을 잇는 한 겹이 비어 있다:

| 있는 것 | 증명 |
|---|---|
| origin 을 **위조할 수 없다** | `no_origin_override` · `agent_origin_forced` — 게이트 밖에 `StrokeOrigin::Agent` 를 만들 길이 없다 |
| origin 이 **프레임 바이트 안에 있다** | `origin_in_signature` — 오프셋 56 |
| 발행기가 **유실 없이 기록한다** | `no_frame_drop` · `published == sent + spooled` |
| **없는 것** | 에이전트가 그린 획을 `SiganPublisher` 로 넘기는 배선 |

그래서 3.2 의 "이 작품: 사람 획 1,847 / AI 획 213" 은 **아직 인증서에 못 찍힌다.**
지금 나오는 것은 세션 안의 집계(`StrokeOriginStats`)뿐이고,
CLI 가 실행 끝에 그 숫자를 stdout 으로 돌려준다(`run_ops_always_reports_stroke_origins`).
그건 세션 로컬 숫자이지 서명될 기록이 아니다.

또 하나: `fill` · `erase` · `gradient` 는 `countStroke()` 로 **집계에는 잡히지만**
스트로크 프레임을 만들지 않는다(타일에 직접 쓴다). 배선을 넣을 때
"획 하나 = 프레임 하나"를 어떻게 맞출지 정해야 한다. 지금 정하지 않은 상태다.

### 8.2 2.7 스트리밍은 **폴링이다**

설계는 "흘려보낸다"고 썼지만 구현은 `events.subscribe` → `events.poll` 로 큐를 꺼내는
**요청/응답**이다. 서버가 먼저 밀어 주는 경로는 없다. `--serve` 도 한 줄 요청에 한 줄
응답이라 푸시가 들어갈 자리가 아직 없다.

### 8.3 2.4 시맨틱 주소 중 **선택(selection)** 은 사각형 하나뿐이다

`{ "region": { "selection": "current" } }` 는 동작하지만, 그 "current" 가 담을 수 있는
것은 사각형 하나다. 올가미·색상 선택·반전·페더는 **마스크 저장소가 없어서** 못 한다.
`select.invert` 와 `select.feather` 가 미지원으로 표시된 이유가 이것이다(4절).

### 8.4 1절 그림의 **COM · UI** 칸은 비어 있다

`COM (Sigan)` 과 `UI (사람)` 두 칸은 Linux 에서 **컴파일조차 되지 않는다.**
`mari::app` 이 플랫폼 중립 브리지 구현체로 서 있어서 COM 래퍼가 물릴 자리는 생겼지만,
그 어댑터 한 겹은 아직 없다(docs/04 2절). 그래서 "사람 UI 도 같은 계층을 쓴다"는
**설계 의도이고, 아직 검증된 사실이 아니다.**
