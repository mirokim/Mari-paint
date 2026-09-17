# Mari Paint — 스트리밍 (docs/05 2.7 의 구현)

> 기준: 2026-09-17 · Linux · g++ 13.3 / clang 18
> 대상: `app/events.cpp`(버스) · `agent/src/session.cpp`(세션 푸시) · `cli/src/server.cpp`(`--serve`)
> 이전 상태: **폴링뿐이었다**(docs/05 8.2 · docs/04 4절). 이 문서는 그것이 어떻게 바뀌었고,
> **무엇이 여전히 폴링으로 남았는지**를 적는다.

---

## 0. 한 줄 요약

**인프로세스 콜백과 `--serve` 연결은 이제 진짜로 밀어받는다. MCP 는 폴링으로 남는다 —
규약에 응용 이벤트를 모델에게 밀어 넣는 채널이 없기 때문이고, 그 이유를 5절에 적었다.**

| 표면 | 이전 | 지금 | 증명 |
|---|---|---|---|
| 인프로세스 | `events.poll` | ✅ `EventHub::subscribe(fn)` · `AgentSession::subscribePush(...)` | `callback_subscriber_is_pushed_not_polled` · `agent_receives_events_without_polling` |
| `--serve` TCP | 한 줄 요청 → 한 줄 응답 | ✅ 연결 유지 + 서버→클라이언트 JSON Lines 푸시 | `serve_pushes_events_over_a_real_socket` (**진짜 소켓**) |
| MCP stdio | `events.poll` | ⚠️ **그대로 폴링** | 5절 |

---

## 1. 버스는 하나다 — 두 번 만들지 않았다

docs/05 2.7 이 요구한 것: *"Sigan 에 쓰는 `IMariEventSink` 와 **같은 이벤트 버스**를 쓴다."*

그래서 새 버스를 만들지 않았다. 이미 있던 `mari::app::EventHub` (docs/03 4절 ③ 채널의
플랫폼 중립 절반)에 **관찰 경로를 덧댔다.** Sigan 이 받을 이벤트와 에이전트가 받을 이벤트는
같은 `fire*` 호출 하나에서 갈라져 나간다.

```
                    Document / Application
                            │  fire*()   ← 발사 지점은 여전히 하나다
                            ▼
                    ┌───────────────┐
                    │   EventHub    │  seq = ++seq_   (버스 전역 단조)
                    └───┬───────┬───┘
         ① 기록 경로 ◄──┘       └──► ② 관찰 경로
       addListener(ptr)            subscribe(fn)
       동기 · 큐 없음               구독자마다 스레드 + 유한 큐
       🔴 드롭 불가                 넘치면 **그 구독자만** 요약/절단
       COM 중계 · Sigan 기록        에이전트 푸시 · --serve · 진단
```

`AgentSession` 의 폴링 큐(`events.poll`)도 ① 에 붙어 있다. 그래서 폴링 클라이언트와
푸시 클라이언트가 **같은 사건을 같은 순서로** 본다.

---

## 2. 🔴 두 정책을 코드에서 분리한 방법

말로만 "sigan 은 드롭 금지"라고 적으면 언젠가 누군가 같은 함수에 조건문을 하나 더 넣는다.
그래서 **정책이 아니라 구조로** 갈랐다.

| | ① 기록 경로 | ② 관찰 경로 |
|---|---|---|
| 등록 | `addListener(IAppEventListener*)` | `subscribe(EventCallback, SubscribeOptions)` |
| 호출 스레드 | **부른 스레드 그대로**(동기) | 그 구독자의 전용 스레드 |
| 큐 | **없다** | 구독자마다 하나, 유한(`capacity`) |
| 용량 인자 | **없다** | 있다 |
| 넘침 | **개념 자체가 없다** | `Summarize`(오래된 것부터 버리고 **센다**) / `Detach`(그 구독자만 끊는다) |
| 느리면 | 그리는 쪽이 느려진다 — 증거를 버리느니 붓이 느린 게 낫다 | 자기 큐만 찬다 |
| 쓰는 곳 | Sigan 기록(docs/03 4.2) · COM 중계 · 세션 폴링 큐 | agent 푸시 · `--serve` |

**기록 경로에는 버릴 수 있는 자료구조가 없다.** 큐가 없으니 넘칠 수 없고, 넘칠 수 없으니
"드롭 정책"이라는 것이 존재할 수 없다. `recording_listener_has_no_queue_to_overflow` 가
이 구조를 못박는다.

반대로 관찰 경로는 **절대 기다리지 않는다.** `Subscriber::offer()` 는 잠금을 짧게 잡고
큐에 넣거나 버리고 곧바로 돌아온다. 콜백은 잠금 **밖에서** 불린다 — 그래야 느린 콜백이
발사 지점을 붙잡지 못한다.

---

## 3. `--serve` 의 줄 규약

한 연결에 두 종류의 줄이 흐른다. 구분자는 `push` 키 하나다.

```jsonc
// 응답 — 요청 한 줄에 대한 답. 요청 순서대로 나간다. push 키가 없다.
{"ok":true,"op":"fill","result":{...}}

// 푸시 — 요청하지 않았는데 나간다.
{"push":true,"event":{"kind":"layerChanged","data":{"seq":7,"layer":1,"change":"pixels"}}}

// 놓친 게 있으면 그 줄에 숫자를 얹는다. 조용히 넘어가지 않는다.
{"push":true,"dropped":12,"event":{...}}
```

- 푸시는 `events.subscribe` 이후에만 나가고 `events.unsubscribe` 로 멎는다.
- `events.poll` 은 **그대로 남는다.** 푸시를 읽을 생각이 없는 단순 클라이언트가 여전히 있고,
  없애면 그들이 깨진다. 능력 표는 하나도 늘지 않았다 — 푸시는 전송 계층의 일이다.

전송은 `poll(2)` 로 **소켓과 아웃박스를 함께 기다린다.** 그래서 요청이 하나도 없는 동안에도
서버가 먼저 줄을 밀어낼 수 있다. 소켓은 논블로킹이고, 상대가 안 읽으면 나갈 바이트는
버퍼에 남고 서버는 계속 돈다(`a_client_that_never_reads_does_not_hang_the_server`).

### 3.1 왜 스레드를 하나 더 쓰지 않았나

구독자 콜백은 그 구독자의 스레드에서 불리고, 소켓 쓰기는 서브 루프 스레드가 한다.
콜백이 직접 `write()` 하면 그 스레드가 소켓에 묶이고, 묶인 동안 자기 큐가 차고,
결국 **버스의 요약 정책이 전송 상태에 좌우된다.** 그래서 콜백은 아웃박스에 넣고
깨움 파이프에 1바이트만 쓴다. 기다리는 곳이 없다.

---

## 4. 역압(backpressure) — 놓친 것을 숨기지 않는다

이 설계의 정직성은 숫자 두 개에 걸려 있다.

1. **`AppEvent::seq` 는 버스 전역 단조 증가다.** 구독자는 seq 구멍만 보고
   자기가 몇 건을 못 봤는지 **정확히** 안다. docs/03 4.2 가 seq 구멍으로 유실을 드러내라고
   한 것과 같은 사고방식이다.
2. **`SubscriberStats::summarized` 가 버린 건수를 센다.** `delivered + summarized` 가
   그 구독자가 구독한 동안 버스를 지나간 전체와 같아야 한다 —
   `overflow_touches_only_the_slow_subscriber` 가 이 등식을 검사한다.

`--serve` 는 여기에 한 겹(아웃박스)이 더 있지만 **정책은 같은 하나**다: 이 연결만 요약하고
센 뒤, 그 숫자를 다음 푸시 줄의 `dropped` 로 실어 보낸다.

### 4.1 🔴 sigan 기록은 이 절과 무관하다

위의 모든 것은 관찰 경로 이야기다. 기록 경로는 큐가 없어서 이 절이 적용될 대상이 없다.
`slow_subscriber_drop_never_touches_the_sigan_record` 가 이것을 실측한다 —
구독자를 일부러 막고 300건을 쏜 뒤, `SiganPublisher` 의 `published == 300`,
`published == sent + spooled`, seq 구멍 0 을 확인한다. 같은 실행에서 구독자는
`summarized > 0` 이다. **한쪽은 버렸고 한쪽은 하나도 안 버렸다. 그게 설계다.**

---

## 5. 🔴 MCP 는 왜 폴링으로 남았나

되는 척하지 않기 위해 규약을 뜯어보고 적는다. MCP(2025-06-18)에서 서버가 클라이언트에게
먼저 보낼 수 있는 것은 넷뿐이고, **넷 다 응용 이벤트 스트림에는 못 쓴다.**

| 채널 | 왜 못 쓰나 |
|---|---|
| `notifications/progress` | **진행 중인 요청에 묶인다.** 클라이언트가 요청 `_meta` 에 `progressToken` 을 넣어야 살아나고, 그 요청이 끝나면 끝난다. 사람이 그리는 사건처럼 **어떤 요청에도 속하지 않는 이벤트**를 담을 자리가 없다 |
| `notifications/resources/updated` | 알림에 **내용이 없다.** "그 리소스가 바뀌었다"만 알리고 클라이언트가 `resources/read` 로 다시 가져가야 한다 — 이름만 바꾼 폴링이고, 왕복은 오히려 는다. 게다가 클라이언트가 `resources/subscribe` 를 구현해야 한다 |
| `notifications/message` (로깅) | 임의 JSON 을 실을 수 있어 가장 가깝다. 그러나 이것은 **로그 채널**이다 — 클라이언트가 모델에게 보여 줄 의무가 없고 실제로 대부분 안 보여 준다. 모델이 못 보는 이벤트는 스트리밍이 아니다 |
| `sampling/createMessage` · `elicitation/create` | 서버가 클라이언트에게 **묻는** 요청이다. 이벤트 통지가 아니다 |

덧붙여 전송 쪽 사실 하나: 우리 stdio 루프는 `getline` 한 줄에 응답 한 줄인 단일 스레드다.
위 채널 중 하나를 억지로 쓰려면 출력에 잠금을 걸고 쓰기 스레드를 하나 더 세워야 한다.
**그 값을 치르고 얻는 것이 "클라이언트가 안 보여 줄 수도 있는 로그 줄"이라면 치를 이유가 없다.**

### 결정

MCP 는 `events_poll` 로 둔다. `initialize` 의 안내문에 **그 사실을 적는다** —
AI 가 "구독했으니 알아서 오겠지"라고 기다리다 아무것도 못 받는 것이 제일 나쁜 결말이다.

> `· 이벤트는 밀어 주지 못한다(MCP 규약의 한계다). events_poll 로 당겨 가라.`

규약이 바뀌어 응용 이벤트용 알림이 생기면 그때 붙인다. 붙일 자리는 이미 있다 —
`McpServer` 가 같은 `AgentSession` 을 들고 있으므로 `subscribePush()` 한 줄이면 된다.
**없는 것은 우리 쪽 배선이 아니라 상대 쪽 수신 규약이다.**

---

## 6. 이벤트 종류

docs/03 4절 표의 8종 + 진행률 하나. `app::AppEventKind` 가 정본이고,
`app::appEventKindName()` 이 와이어에 나가는 이름을 준다.

| kind | 언제 | data |
|---|---|---|
| `documentOpened` | `.ora` 를 열었다 | `path` `fileHash` `width` `height` |
| `documentSaved` | 저장했다 | `path` `fileHash` `sizeBytes` |
| `canvasSnapshot` | 캔버스 해시를 냈다 | `canvasHash` |
| `viewChanged` | 줌·회전이 바뀌었다 | `zoom` `rotationDeg` |
| `paste` | 붓으로 그리지 않은 픽셀이 들어왔다 | `source` |
| `undo` | 되돌렸다 | `steps` (>0 undo, <0 redo) |
| `layerChanged` | 레이어가 바뀌었다 | `layer` `change` |
| `strokeCompleted` | 획이 끝났다 | `layer` `firstSeq` `lastSeq` `pointCount` |
| `progress` | 긴 작업의 진척 | `task` `fraction`(0..1) `done` |

모든 `data` 에 **`seq` 가 함께 실린다**(4절).

### 6.1 `progress` 는 기록이 아니다

진행률은 픽셀을 설명하지 않는다. 그래서 Sigan 정본에 들어갈 사실이 아니고,
프레임도 만들지 않는다. 버스를 같이 쓰되 **의미를 섞지 않는다** —
docs/06 의 H4("하중 받는 사실이 서명 정본 밖에 있기 금지")의 뒷면이다:
하중을 받지 않는 것을 정본에 밀어 넣지도 않는다.

---

## 7. 경계선·위조방지 확인 (docs/03 2절 · docs/05 3.1)

- ES256 · 해시체인 · `prevHash` · 신뢰 등급 문자열 **없음.** 이 변경은 SHA-256 도 안 부른다.
- `StrokeSource() = delete` **그대로.** origin 세터 · `withOrigin` · `setOrigin` **없음.**
- `StrokeSource` 생성 지점 **6곳 그대로.** 이 변경은 `StrokeSource` 를 만들지 않는다 —
  이벤트는 출처를 **모른다**(이벤트에 origin 칸이 없다. 출처는 프레임의 일이다).
- `friend` 는 `agent::AgentStrokeGate` 하나 그대로.
- 능력 표와 MCP 도구 개수가 **그대로다**(지금 둘 다 38개). **푸시는 전송 계층에서만 일어난다.**

---

## 8. 테스트

| 테스트 | 무엇을 증명하나 | 어디서 |
|---|---|---|
| `callback_subscriber_is_pushed_not_polled` | poll 을 한 줄도 안 부르는데 콜백이 불린다 | `tests/app/test_stream.cpp` |
| `progress_travels_the_same_bus` | 진행률도 같은 버스다 | 〃 |
| `slow_subscriber_does_not_block_the_drawing_thread` | 10ms 콜백 × 200건인데 발사는 즉시 끝난다 | 〃 |
| `overflow_touches_only_the_slow_subscriber` | 빠른 구독자는 300/300, 느린 쪽만 요약됨. `delivered+summarized == 300` | 〃 |
| `detach_policy_cuts_only_that_subscriber` | 끊는 정책도 옆 구독자를 안 건드린다 | 〃 |
| 🔴 `slow_subscriber_drop_never_touches_the_sigan_record` | 구독자를 막아도 발행기는 300/300, seq 구멍 0 | 〃 |
| `recording_listener_has_no_queue_to_overflow` | 기록 경로에 넘칠 큐 자체가 없다 | 〃 |
| `agent_receives_events_without_polling` | 세션 푸시. 폴링 큐는 비어 있다 | `tests/agent/test_push.cpp` |
| `push_filter_selects_kinds` | 종류 필터 | 〃 |
| `slow_push_subscriber_does_not_slow_down_drawing` | 느린 구독자 × 60 연산인데 그리기는 안 느려진다 | 〃 |
| `push_subscription_dies_with_the_session` | 세션이 죽을 때 구독자 스레드도 합류된다 | 〃 |
| `serve_pushes_events_over_a_real_socket` | **진짜 fd** 로 push 줄이 온다. poll 을 안 보냈다 | `tests/cli/test_serve_push.cpp` |
| `unsubscribe_stops_the_push_stream` | 멎는다 | 〃 |
| `a_client_that_never_reads_does_not_hang_the_server` | 안 읽는 클라이언트가 서버를 못 매단다 | 〃 |

---

## 9. 아직 아닌 것 — 정직하게

- **WebSocket 도, 인증도 없다.** `--serve` 는 여전히 루프백 평문이고 한 번에 한 연결이다
  (docs/05 7절 A5 의 ⚠️ 는 그대로다). 푸시가 생겼다고 원격에 열어도 된다는 뜻이 아니다.
- **동시 연결이 없다.** 세션이 "한 스레드 하나" 규약이라 연결을 동시에 받으면 그 규약이 깨진다.
  여러 에이전트가 같은 문서를 보는 것은 별개 설계다.
- **사람 GUI 가 없으므로** "사람이 그리는 동안 에이전트가 그 사건을 흘려받는다"는 시나리오는
  **절반만 실측됐다.** 버스·푸시·역압은 돌지만, 반대편에서 사건을 만드는 사람 손이 아직 없다
  (docs/04 3절 UI 칸). 이벤트를 만드는 쪽이 붙어도 이 경로는 그대로 쓴다 — 발사 지점이 같아서다.
- **진행률을 실제로 쏘는 긴 작업이 아직 없다.** `fireProgress` 는 있고 버스를 타지만,
  8bf 필터·대형 저장 쪽에서 부르는 코드는 없다. 자리를 만들어 둔 것이지 쓰고 있는 게 아니다.
