# Mari Paint — 구현 현황

> 기준: 2026-09-17 · g++ 13.3 / clang 18 · Linux 컨테이너
> 목적: **무엇이 실제로 도는지 정직하게 적는다.** 설계 문서(01~03)가 "하려는 것"이라면
> 이 문서는 "지금 되는 것"이다. 둘을 섞으면 아무도 리포를 믿지 않는다.

---

## 0. 한 줄 요약

**플랫폼 중립 코어는 다 됐고 테스트로 증명된다. 그 위에 에이전트 능력 계층(docs/05)과
헤드리스 CLI · MCP 서버가 올라갔고 Linux 에서 실제로 돈다. Windows 레이어는 코드가 있을
뿐 한 번도 컴파일되지 않았다. GUI 는 없다 — 다만 `mari-paint` 헤드리스 실행파일은 있다.**

```
빌드   : g++ 13.3 ✅  ·  clang 18 ✅   (경고 0 · 오류 0)
테스트 : 55개 실행파일 / 425 케이스 — 양쪽 컴파일러에서 100% 통과
         skip·disable 한 테스트 없음   (2026-09-17 클린 빌드 재검증: 55/55 · 경고 0)
```

| 모듈 | ctest 실행파일 | 상태 |
|---|---|---|
| foundation | 5 | ✅ 검증됨 |
| core | 6 | ✅ 검증됨 (**선택 마스크** 포함) |
| stroke | 6 | ✅ 검증됨 |
| io/ora | 3 | ✅ 검증됨 |
| io/brush | 4 | ✅ 검증됨 |
| sigan | 7 | ✅ 검증됨 (Windows 파이프 분기만 미검증) |
| crypto | 2 | ✅ 검증됨 (NIST 벡터 · 해시 규약) |
| app | 2 | ✅ 검증됨 (플랫폼 중립 브리지 구현체 · **이벤트 버스 푸시·역압**) |
| agent | 6 | ✅ 검증됨 (docs/05 능력 계층 · 세션 푸시 구독 · **선택 마스크 적용**) |
| mcp | 1 | ✅ 검증됨 (도구 목록은 연산 표에서 생성) |
| cli | 5 | ✅ 검증됨 (실행파일 왕복 · `headless_parity` · docs/02 8절 실측 · **`--serve` 푸시 실소켓**) |
| win | 4 | ⚠️ Win32 비의존 부분만 |
| integration | 1 | ✅ 검증됨 |

---

## 1. 구현됐고 Linux에서 실제로 검증된 것

### 1.0a core — 선택 마스크 (docs/05 8.3 이 적어 둔 한계를 메웠다)

선택이 **8비트 알파 마스크**가 됐다. 저장 구조는 캔버스와 같다 —
64×64 Gray8 타일의 희소 맵(COW). 새 자료구조를 만들지 않았다.

```
tests/core/test_selection.cpp :: full_and_empty_selection_allocate_no_tiles
  전체 선택도 빈 선택도 tileCount() == 0. 메모리를 한 바이트도 안 쓴다.
  (타일이 없는 자리의 값을 0 이 아니라 outsideValue() 로 뒀기 때문이다)
tests/core/test_selection.cpp :: invert_does_not_allocate_the_canvas
  사각형 선택(타일 1개)을 반전해도 타일이 1개 그대로다. 캔버스 16타일을 잡지 않는다.
```

되는 것: 사각형 · 타원 · 올가미(폴리곤, 짝수-홀수) · 색상 범위 · 내용(nonEmpty) ·
레이어 알파에서 만들기, union/subtract/intersect/xor 결합, 반전 · **원형** 팽창/수축
(정확한 유클리드 거리 변환) · 가우시안 페더.

🔴 **핫 패스 비용 0.** 엔진은 `beginStroke()` 에서 `isAll()` 을 한 번 보고 포인터를
끈다. `tests/stroke/test_bench.cpp :: bench_selection_is_free_when_there_is_no_selection`
가 같은 일감을 세 번 재서 숫자를 찍는다(아래 4절 6번).

아직 안 되는 것도 적는다: **선택은 `.ora` 에 저장되지 않는다.** 문서 세션 안에서만 산다.

### 1.1 core — 타일 캔버스 (docs/02 3절)

64×64 타일의 희소 맵, Copy-on-Write, 레이어 트리, 블렌드 모드 19종, 더티 영역 합성,
타일 단위 실행취소.

**docs/02 3절의 핵심 주장이 테스트로 증명된다:**

```
tests/core/test_tile.cpp :: single_dot_on_4000px_canvas_allocates_one_tile
  4000×4000 캔버스(전부 잡으면 3969 타일)에 구석 점 하나 → tileCount() == 1
  반대편 구석에 하나 더 → 2. 사이는 메모리조차 없다.
tests/core/test_compositor.cpp :: sparse_layer_only_touches_allocated_tiles
  같은 조건에서 합성기도 할당된 타일만 건드린다.
```

COW 스냅샷 독립성(양방향), 실행취소가 변경된 타일만 보관하는 것도 테스트가 덮는다.

### 1.2 stroke — 파이프라인 (docs/02 4절)

`[1] 정규화 → [2] 스무딩 → [3] 보간 → [4] 엔진 stamp() → 더티 타일`.
엔진은 `mari::brush::createEngine("native")` 팩토리로 붙는다. 핫 패스는 예외를 던지지 않는다.

`tests/stroke/test_bench.cpp` 가 처리량을 재지만 — **이건 성능 목표 달성 증명이 아니다.**
컨테이너에서 잰 알고리즘 처리량일 뿐, docs/02 8절의 "펜 입력 → 화면 표시 16ms"는
화면이 없어서 잴 수 없다.

### 1.3 io/ora — OpenRaster (docs/02 M0)

자체 ZIP(zlib) + libpng + 자체 XML 파서. 외부 의존성을 끌어오지 않았다.
`mimetype` 무압축 선두, `stack.xml`, `data/layer*.png`, `mergedimage.png`, 썸네일,
그리고 Mari 확장인 `mari/prooflog.json` 통로까지.

왕복 테스트가 픽셀 · 불투명도 · 블렌드 모드 · **레이어 순서 뒤집기**(stack.xml 첫 자식이
맨 위)를 검증한다. 희소성도 왕복 후에 유지된다.

### 1.4 io/brush — .abr / .sut (docs/02 5절)

- **.abr**: 버전 6/7/10 지원. 버전 1/2는 명시적으로 거절한다(추측해서 읽지 않는다).
  팁 비트맵, descriptor 파라미터, 패턴.
- **.sut**: SQLite. **컬럼 이름을 하드코딩하지 않는다** — `PRAGMA table_info` 로 런타임
  조회 후 있는 것만 매핑한다(docs/02 5.1). `*effector` 빅엔디안 blob은 레이아웃 가설을
  여러 개 시도하고, 전부 실패해도 팁·기본 파라미터는 살린다.

**docs/02 5절 "정직하게 실패한다"가 실제로 동작한다.** 임포터가 번역하지 못한 키는
`ImportReport` 에 `Dropped`(버림) / `Degraded`(근사) / `Info` 로 남는다.
테스트가 이걸 강제한다:

```
tests/io/brush/test_abr.cpp  : 일부러 번역 안 한 "Nose"·"Wtdg" 가 Dropped 로 뜨는지,
                               제대로 번역한 "Dmtr"·"Spcn" 은 Dropped 로 안 뜨는지
tests/io/brush/test_sut.cpp  : 모르는 컬럼이 Dropped 로 뜨고 아는 컬럼은 안 뜨는지
```

### 1.5 sigan — 증거 발행 (docs/03)

**docs/03 2절 경계선이 코드에서 지켜진다.** 리포 전체를 grep 해도
해시체인 봉인 · ES256 · ECDSA · 서명 코드가 **없다.** 등급은 `unsigned` 하나뿐이고,
`tests/sigan/test_prooflog.cpp` 가 생성된 JSON에 `"signature"` 도 `"ES256"` 도
들어가지 않는 것을 확인한다. Mari는 증명하지 않는다.

**docs/03 4.2 오버플로 정책도 코드에 실제로 있다.** `PublisherStats` 에는
`Dropped` 필드가 **일부러 없다.** `publish()` 는 순서가 이렇다:

```
1. 저널에 먼저 쓴다   ← 파이프 상태와 무관하게 정본은 남는다
2. 파이프가 붙어 있고 순서가 안 밀렸으면 그때 보낸다
3. 못 보낸 건 spooled 로 센다. pump() 가 나중에 순서대로 밀어넣는다
   published == sent + spooled  (유실 0)
```

테스트: `no_frame_drop`, `sigan_restart_survives`, `reconnect_honors_peer_resume_point`,
`version_skew_keeps_recording`, `local_mode_records_everything`,
`mari_crash_survives_truncated_tail`(잘린 꼬리를 버리고 마지막 완성 획까지 복구),
`journal_detects_bit_rot_and_stops`.

재연결이 새 Segment를 만들지 않는 것(docs/03 5.3), 단조 시계(5.6)도 덮인다.

### 1.6 Windows 레이어 중 떼어낸 부분

Win32에 의존하지 않는 로직을 헤더로 분리해서 Linux에서 돌린다 (38 케이스):

| 파일 | 검증 내용 |
|---|---|
| `tests/win/test_view_transform.cpp` | **캔버스 좌표 불변성** — 줌·회전·팬·미러 5종 뷰에서 같은 획이 같은 캔버스 좌표가 된다 (docs/03 4.1의 핵심 주장) |
| `tests/win/test_pen_normalize.cpp` | WM_POINTER 원본값 → 장치 독립 필압/틸트. QPC → ns |
| `tests/win/test_shm_layout.cpp` | 8bf 공유 메모리 구조체 바이트 레이아웃 고정 (x86 호스트와 x64 본체가 같은 바이트를 봐야 한다) |
| `tests/win/test_pipl.cpp` | PIPL 리소스 파서 — 적대적/잘린 입력 방어 |

### 1.6a 이벤트 푸시 — 진짜 스트리밍 (docs/05 2.7 → docs/07)

`events.poll` 밖에 없던 자리에 **서버 푸시**가 생겼다. 버스는 늘리지 않았다 —
Sigan 기록이 쓰는 `mari::app::EventHub` 하나에 관찰 경로를 덧댔다(docs/07 1절).

```
addListener(ptr)  → 기록 경로: 동기 · 큐 없음 · 🔴 드롭 불가 (Sigan · COM · 세션 폴링 큐)
subscribe(fn)     → 관찰 경로: 구독자마다 스레드+유한 큐 · 넘치면 그 구독자만 요약/절단
```

- **인프로세스**: `EventHub::subscribe(fn)` · `AgentSession::subscribePush(kinds, fn)`.
- **`--serve`**: 연결을 유지하고 `{"push":true,"event":{...}}` 줄을 밀어낸다.
  `poll(2)` 로 소켓과 아웃박스를 함께 기다린다. **진짜 fd 로 검증된다.**
- **MCP**: ⚠️ 폴링 그대로. 규약에 응용 이벤트를 모델에게 밀어 넣는 채널이 없다(docs/07 5절).
  안내문에 그 사실을 적어 둔다 — 있는 척하지 않는다.
- 이벤트 종류는 docs/03 4절 표 8종 + `progress` 하나. 전부 `seq` 를 달고 다녀서
  요약당한 구독자가 **몇 건을 놓쳤는지 스스로 안다.**

🔴 제일 중요한 케이스: `slow_subscriber_drop_never_touches_the_sigan_record` —
구독자를 일부러 막고 300건을 쏴도 `SiganPublisher` 는 300/300 이고 seq 구멍이 0 이다.
같은 실행에서 구독자는 `summarized > 0` 이다. 한쪽은 버렸고 한쪽은 안 버렸다.

### 1.7 integration — 모듈 간 이음매

`tests/integration/test_endtoend.cpp` 4 케이스. 각 모듈 테스트는 가짜(`FakeTileMap`,
`FakeSink`)를 쓰므로, 진짜 타입끼리 맞물리는지는 여기서만 본다.

- 스트로크 엔진이 core의 **실제 `TileMap`** 에 그린다 (더티 목록이 과대 보고되지 않는다)
- 더티 타일이 합성기로 흘러 실제 색이 나온다
- 파이프라인 샘플 → sigan 저널 (seq 단조 · 구멍 0 · `published == sent + spooled`)
- 그린 문서의 .ora 왕복

### 1.8 WinTab 미사용 (docs/03 3절)

`grep -ri wintab` 결과: 리포 안의 모든 히트가 **주석 · 문서 · CI 검사 스크립트**다.
`wintab32` 를 로드하는 코드도, 링크 목록의 `wintab32.lib` 도 없다.
`scripts/ci/check-no-wintab.ps1` 이 산출물의 임포트 테이블 · 지연 로드 · 문자열을
3중으로 검사한다 — 단, **이 스크립트 자체가 Windows 러너에서만 돌아서 아직 실행된 적 없다.**

---

## 2. 작성됐지만 **컴파일조차 못 한 것**

> 🔴 이 절이 이 문서의 존재 이유다. 아래는 전부 "코드가 있다"이지 "된다"가 아니다.
> 개발·CI 환경이 Linux 컨테이너라 Windows SDK · MIDL · 펜 하드웨어가 없다.

| 항목 | 파일 | 왜 검증 못 했나 |
|---|---|---|
| COM 인터페이스 정의 | `platform/win/com/mari.idl` (477줄) | MIDL 컴파일러 필요. 타입 라이브러리 생성 안 해봄 |
| COM dual/IDispatch 구현 | `platform/win/com/application.cpp` (1051줄), `dual_base.cpp`, `com_ptr.cpp` | Windows SDK 헤더 필요 |
| COM 이벤트 싱크 | `platform/win/com/event_sink.cpp` | 연결점(connection point) 동작 미확인 |
| `LocalServer32` 등록 | `platform/win/com/registration.cpp`, `server.cpp` | 레지스트리 왕복 미실행 |
| Windows Ink 입력 | `platform/win/input/pointer_input.cpp` | WM_POINTER · 실제 펜 필요 |
| 8bf 격리 호스트 | `hosts/host/*`, `hosts/client/*` | 실제 .8bf 플러그인 · x86 툴체인 필요 |
| `sigan-native` 명명 파이프 | `sigan/src/pipe_sink.cpp` 의 `#ifdef _WIN32` 분기 | POSIX(AF_UNIX) 분기만 테스트됨 |

Windows 빌드 잡은 `.github/workflows/ci.yml` 에 **작성돼 있다.** 그 잡이 한 번이라도
초록이 되기 전까지 위 항목은 전부 "미검증"이다.

또 하나 정직하게: COM 쪽 `IMariDocumentBridge` / `IMariAppBridge` 는 여전히 구현체가
없다. 다만 같은 메서드 집합을 Win32 없이 다시 세운 **플랫폼 중립 구현체 `mari::app`
(`mari::app::Document` / `Application`)은 있고 `tests/app/test_bridge.cpp` 가 Linux 에서
검증한다.** 남은 것은 **COM 래퍼 ↔ `mari::app` 어댑터 한 겹**이고, 그건 Windows 빌드가
처음 초록이 될 때 쓸 자리다.

---

## 3. 아직 아예 없는 것

| 항목 | 설계 위치 | 메모 |
|---|---|---|
| 🔴 **사람의 실제 펜 입력** | docs/03 3절 · docs/06 결정 ③ | 기록 배선은 사람 경로를 **받을 준비가 끝났다**(`StrokeSource::humanPen()` → `app::StrokeEntry` → 같은 발행 지점). 그러나 그 입구에 점을 넣어 주는 것은 **테스트와 하네스뿐**이다. Windows Ink(WM_POINTER)는 2절에 있고 컴파일된 적이 없다. **"사람 획 기록이 된다"를 "실제 펜 입력이 된다"로 읽으면 안 된다** |
| **UI (Qt 6)** | docs/02 2절 `ui/` | 리포에 Qt 코드 0줄. 그래서 docs/02 8절 중 **화면이 필요한 목표(펜 입력 → 화면 표시 16ms)는 여전히 측정 불가**다. 나머지 넷은 헤드리스로 실측했다(4절 6번). 헤드리스 CLI `mari-paint` 는 Linux 에서 실제로 돈다 — `--script` / `--exec` / `--serve` / `--mcp --stdio` / `--capabilities` |
| ~~**에이전트 획 → Sigan 배선**~~ | docs/05 2.3 · 3.2 | ✅ **2026-09-17 완료.** `record/` 모듈이 잇는다 — 발행 지점은 `record/src/sigan_recorder.cpp` **하나뿐**이고 `app`·`agent` 는 여전히 sigan 을 링크하지 않는다. 규약은 docs/06, 검증은 `tests/record/` |
| **libmypaint 어댑터** | docs/02 2절 `engines/mypaint` | 지금 엔진은 자체 `native` 하나. `createEngine()` 에 한 줄 더하면 붙는 자리는 만들어 뒀다 |
| **.psd 읽기/쓰기** | docs/02 M3 | 시작 안 함 |
| **색 관리 (LittleCMS)** | docs/02 2절 `color/` | 지금은 RGBA8 sRGB 고정. 16/32bit · CMYK 없음 |
| **GPU 합성** | docs/02 M6 | 전부 CPU |
| **레이어 마스크의 .psd/.ora 흡수** | — | 규약 자체가 미결(5절) |
| **선택의 파일 저장** | — | `SelectionMask` 는 있지만 `.ora` 에 쓰지 않는다. 문서 세션 안에서만 산다(스냅샷/되돌리기는 탄다) |

---

## 4. 알려진 한계

1. **Sigan 와이어 식별자 폭.** `LayerId`/`BrushId` 는 u64인데 docs/03 4.1 프레임은 u32다.
   2^32를 넘으면 잘린다. 조용히 넘어가지 않고 `PublisherStats::idTruncations` 로 센다.
   표를 넓힐지는 Sigan 쪽과 합의가 필요하다.
2. **리포트 타입이 두 벌이다.** io/ora의 `Document::warnings` 는 `vector<string>` 이고,
   io/brush의 `ImportReport` 는 severity/key/message 구조다. io 공용으로 올리는 게 맞다.
2-1. ~~**선택은 사각형 하나뿐.** 마스크 저장소가 없어 `select.invert`·`select.feather` 는
   미지원 표시 + MCP 도구 미노출.~~ → **해결됐다.** `include/mari/core/selection.hpp`.
   넷 다 supported 가 되어 MCP 도구가 **자동으로** 나타났다(도구 38개 = 연산 38개).
   남은 한계는 둘이고 숨기지 않는다: ⓐ 선택이 `.ora` 에 저장되지 않는다,
   ⓑ 브리지(COM)의 `selection()` 은 여전히 `Rect` 하나라 **경계 상자**만 넘어간다.
3. **레이어 마스크 규약이 미결.** 현재: 없는 타일 = 0 = 가림. **Krita와 반대다.**
   .ora/.psd 임포터가 이 차이를 흡수해야 하는데 아직 안 했다.
4. **`duplicateLayer()`/`setLayerTiles()` 의 자리가 미결.** `layer_ops.hpp` 자유 함수로
   둘지 인터페이스로 올릴지.
5. **.sut effector blob은 추측이다.** 여러 레이아웃 가설을 시도하는 구조라 실제 CSP
   파일에서 커브가 맞는지는 검증되지 않았다. 실패해도 팁은 살아남는다.
6. **성능 수치 — 5개 중 4개는 이제 실측했다. 1개는 여전히 못 잰다.**
   `tests/cli/headless_main.cpp` 가 매 ctest 마다 재고 숫자를 출력하며, 목표를 넘기면
   **테스트가 깨진다**(CI 게이트가 생겼다는 뜻이다). 아래는 g++ 13.3 · Linux 컨테이너 실측:

   아래는 **2026-09-17 재측정**이다(g++ 13.3 · Linux 컨테이너 · RelWithDebInfo).

   | docs/02 8절 목표 | 목표치 | 실측 | 판정 |
   |---|---|---|---|
   | 콜드 스타트 | < 1.5초 | **3.0 ms** (`--capabilities`, 5회 최소. 셸 경유 포함) | ✅ |
   | 빈 캔버스 메모리 1920×1080 | < 150MB | **6.30 MB** (프로세스 전체 RSS. 세션+문서 증가분은 0.43MB) | ✅ |
   | 8K 캔버스 열기 | < 3초 | **605 ms** (8192² 전면 채색 .ora) | ✅ |
   | 설치 용량 | < 80MB | **29.36 MB** 미스트립 / **1.28 MB** strip 후 | ⚠️ 부분 |
   | 펜 입력 → 화면 표시 | < 16ms | **측정 불가** | 🔴 |

   ⚠️ **부분**과 🔴 를 흐리지 않는다:
   · 설치 용량 29.36MB 는 **GUI·COM·8bf 호스트가 하나도 안 들어간** RelWithDebInfo
     빌드다. 대부분이 디버그 심볼이라 `strip` 하면 1.28MB 다. 붙으면 는다.
     zlib·sqlite3·libpng 는 시스템 공유 라이브러리라 여기 안 잡힌다.
     (지난 회차 24.94MB → 29.36MB. 선택 마스크와 기록 배선이 들어왔다.)
   · 16ms 는 **화면이 없어서 절반만 잴 수 있다.** 잰 절반(입력 → 픽셀)은
     agent-api 를 통과하는 점 하나당 **119.2 us**, 획(점 64개) 하나당 평균 **7.63 ms**,
     최악 **16.07 ms** 다.
     표시 쪽을 붙이지 않은 숫자이므로 **"16ms 목표를 달성했다"고 쓰지 않는다.**
   · 목표에 없던 값 하나가 눈에 띈다: **8K .ora 저장이 8.9초**다. 여는 건 605ms 인데
     쓰는 게 15배 느리다(8192² PNG 디플레이트). 목표가 없어 통과/실패를 말할 수 없고,
     그래서 게이트도 걸지 않았다. 숫자만 남긴다.

   `stroke_bench` 는 별개다 — UI·에이전트를 거치지 않은 순수 파이프라인 처리량이고
   회귀 감지용이다: 스탬프 **143,000~153,000개/초**, 이벤트당 **70 us**,
   **2차 획 힙 할당 0회.**
   (컨테이너라 회차마다 ±6% 흔들린다. 한 번의 수치를 정밀한 값처럼 적지 않는다)

   **선택 마스크 비용**(`bench_selection_is_free_when_there_is_no_selection`, **6회 실행**):

   | 설정 | 스탬프 10,000개 | 선택 없음 대비 |
   |---|---|---|
   | 선택 없음(포인터 null) | 67.8 ~ 70.0 ms | 기준 |
   | 전체 선택 마스크를 붙임 | 67.0 ~ 69.9 ms | **−2.3% ~ +2.1%** (같은 코드 경로다 — 잡음) |
   | 진짜 마스크를 붙임 | 74.0 ~ 82.3 ms | **+8.2% ~ +18.2%** |

   🔴 셋째 줄을 감추지 않는다. **선택을 실제로 쓰면 비용이 든다.** 규약은
   "선택이 **없을 때** 0"이지 "언제나 0"이 아니다.

   🔴 **지난 회차가 적어 둔 폭을 이번 실측이 고쳤다.** 옛 문구는 둘째 줄을
   "−0.4% ~ −0.2%", 셋째 줄을 "+16% ~ +21%" 이라고 적었다. 3회 실행의 최솟값이었고
   **폭이 실제보다 좁았다.** 6회를 재니 둘째 줄은 ±2.3% 안에서 부호가 바뀌고(잡음이
   맞다 — 같은 코드 경로이므로 방향이 정해질 이유가 없다) 셋째 줄은 +8.2% 까지
   내려온다. 좁은 폭을 그대로 두면 다음 사람이 잡음을 회귀로 읽는다.

   **기록 배선 비용**(2048² 캔버스에 8점 획 2,000개, 3회 중 최선 · 리포 밖 하네스):

   | 설정 | 시간 | 처리량 |
   |---|---|---|
   | 레코더 없음(널 레코더) | 761.6 ms | 2,626 획/초 |
   | 로컬 저널 기록 | 756.7 ms | 2,643 획/초 (**−0.6%**) |

   차이가 음수인 것은 잡음이라는 뜻이다. 저널 append 는 획당 64바이트 프레임 몇 개라
   붓질 비용에 묻힌다. **다만 이건 CI 게이트가 아니다** — 하네스가 리포 밖에 있다.
   게이트로 삼으려면 `tests/` 안으로 들여와야 하고, 아직 안 했다.

7. ✅ **[2026-09-17 해소] 에이전트 획이 Sigan 으로 흘러간다.**
   배선은 `record/` 모듈이고, 규약은 `docs/06-recording-contract.md` 다.
   지금 서 있는 것:

   | 잇는 겹 | 증명하는 테스트 |
   |---|---|
   | agent-api 획이 발행기까지 전부 도착하고 전부 `origin=Agent` | `agent_strokes_reach_the_publisher_as_agent` |
   | 사람 경로 입구(`app::StrokeEntry`)가 **같은** 발행기로 간다 | `human_pen_path_reaches_the_same_publisher` |
   | 섞어 그린 M/N 이 prooflog JSON 에 실제로 찍힌다 | `mixed_human_and_agent_counts_are_exact_in_the_prooflog` |
   | `fill` 이 "AI 획 1개"로 축소되지 않는다(합성 프레임 쌍 + 다른 칸) | `fill_is_a_synthetic_frame_pair_not_a_brush_stroke` |
   | 파이프가 끊겨도 그리기는 성공하고 유실 0 | `pipe_failure_still_draws_and_drops_nothing` |
   | 저널이 고장 나면 연산 실패 + 캔버스 원상복구 | `journal_failure_rejects_the_draw_and_rolls_back` · `a_real_unwritable_journal_is_detected` |
   | 레코더 없이도 agent-api 가 돈다(널 레코더) | `agent_api_works_without_any_recorder` |
   | 구간을 만드는 것은 세션이 아니라 문서다 | `a_new_session_on_the_same_document_does_not_split_the_segment` |
   | **발행 지점이 하나다** | `single_publish_path` · `neither_app_nor_agent_links_sigan` |

   `fill`·`erase`·`gradient`·`transform` 의 프레임 표현도 정해졌다(docs/06 결정 ①):
   `Down|Synthetic` 좌상단 + `Up|Synthetic` 우하단 **두 프레임**. 두 점이 영향 영역을
   프레임 바이트 안에 담고, 붓질 수와는 **다른 칸**에 센다(docs/06 결정 ②).
   아직 기록 밖인 것(붙여넣기·레이어 병합·실행취소)은 docs/06 10절에 적어 뒀다.

   **테스트 말고 진짜 바이너리로도 돌려 봤다**(2026-09-17). 1024² 캔버스에
   `--agent-id claude-opus-5` 로 `fill` 1 · 붓질 3 · `gradient` 1 을 넣고
   `--journal-dir` · `--proof-out` 을 준 결과 `mari/prooflog.json` 의 실제 내용:

   ```
   "humanStrokes": 0, "agentStrokes": 3, "agentRegionOps": 2,
   "changedTiles": {..., "agent": 326, "measured": true},
   "frameCount": 13, "lostFrames": 0, "grade": "unsigned", "signed": false
   strokes[0] : synthetic=true,  area 0,0 1024×1024   ← 캔버스 전면 fill
   strokes[4] : synthetic=true,  area 0,900 1024×124  ← gradient
   ```

   🔴 **캔버스 전체를 칠한 `fill` 이 "AI 획 1개"가 되지 않았다.** 붓질 칸은 3 그대로이고
   `fill` 은 `agentRegionOps` 라는 다른 칸에 있으며, 얼마나 넓었는지가 `area` 로 남았다
   — docs/06 6절 H3 가 요구한 그대로다. 사람·AI 를 한 문서에 섞은 실측
   (사람 붓질 12 / AI 붓질 4 / AI fill 1)에서는 `agentStrokeRatio: 0.250` 이고
   변경 타일은 사람 24 / AI 267 이다. **붓질로는 25%, 면적으로는 91.7%** —
   한 줄로 줄이지 않는 이유가 이 두 숫자다. README 의 "사람 획 · AI 획을 기록으로
   남긴다" 절에 출력 전문이 있다.

   <details><summary>옛 기록(배선이 없던 시절)</summary>

   ~~**에이전트 획이 Sigan 으로 흘러가지 않는다.**~~ docs/05 2.3 은 "Sigan 이 AI 획도
   똑같이 기록한다"고 썼지만 **배선이 없다.**
   `grep -rn "Publisher\\|publish(" --include=*.cpp .` 의 호출자가 `sigan/` 과 `tests/`
   밖에 **0곳**이고, `agent/` `app/` `cli/` `mcp/` 어느 CMakeLists 도 `mari::sigan` 을
   링크하지 않는다. 지금 서 있는 것은 조각 셋이고 잇는 한 겹이 비어 있다:

   | 있는 것 | 증명하는 테스트 |
   |---|---|
   | origin 을 위조할 수 없다 | `no_origin_override` · `agent_origin_forced` |
   | origin 이 프레임 바이트(오프셋 56) 안에 있다 | `origin_in_signature` |
   | 발행기가 유실 없이 기록한다 | `no_frame_drop` · `published == sent + spooled` |
   | **없는 것 — 에이전트 획을 발행기로 넘기는 코드** | — |

   그래서 docs/05 3.2 의 "사람 획 1,847 / AI 획 213" 은 **아직 인증서에 못 찍힌다.**
   지금 나오는 건 세션 로컬 집계(`StrokeOriginStats`)뿐이고 CLI 가 실행 끝에 stdout 으로
   돌려줄 뿐이다. 덧붙여 `fill`·`erase`·`gradient` 는 집계에는 잡히지만 스트로크 프레임을
   만들지 않는다(타일에 직접 쓴다) — 배선할 때 "획 하나 = 프레임 하나"를 어떻게 맞출지
   먼저 정해야 한다. 아직 안 정했다.

   </details>
8. **스트리밍의 절반은 아직 실측 대상이 없다.** 이벤트 푸시·역압은 Linux 에서 돌고
   테스트가 덮는다(docs/07 8절) — 인프로세스 콜백과 `--serve` 푸시는 **진짜 소켓**으로
   검증된다. 하지만 ⓐ **MCP 는 폴링 그대로**이고(규약에 응용 이벤트를 모델에게 밀어 넣는
   채널이 없다. 이유는 docs/07 5절), ⓑ "사람이 그리는 동안 에이전트가 그 사건을 받는다"는
   시나리오는 **사건을 만드는 사람 손이 없어서** 절반만 실측됐다(GUI 없음, 3절),
   ⓒ `fireProgress` 를 실제로 부르는 긴 작업이 아직 없다 — 자리만 있다.
   `--serve` 에 인증도 동시 연결도 없다는 사실은 푸시가 생겨도 그대로다.

9. **docs/03 10절 검증 8종 중 자동으로 도는 건 2개**(`canvas_xy_invariant`,
   `wintab_never_loaded` — 후자는 Windows 잡이 돌아야). 나머지는 실제 Sigan 바이너리 ·
   펜 하드웨어 · VASE9 리포와의 공유 골든 픽스처가 필요하다. `ci.yml` 의
   `verification-matrix` 잡이 이 현황을 매 빌드마다 출력한다.

---

## 5. 다음 단계 (우선순위 순)

1. **Windows CI 잡을 한 번 초록으로 만든다.** 2절 전체가 여기에 걸려 있다.
   이게 되기 전까지 COM·8bf·Windows Ink는 전부 추측이다.
2. **미결 규약 2건을 정한다** — 레이어 마스크 방향, `layer_ops` 의 자리.
   둘 다 .psd 임포터를 시작하기 전에 정해야 한다.
3. ~~**SHA-256을 구현한다.**~~ → **완료.** `mari::crypto`(FIPS 180-4, 외부 의존 0)가
   NIST 벡터로 검증되고, 캔버스/레이어/파일 해시 바이트 순서 규약이
   `include/mari/crypto/canvas_hash.hpp` 헤더에 언어 중립적으로 적혀 있다.
   docs/03 S3(`StartCanvasHash`/`EndCanvasHash` 가 v0 null 을 벗어나는 것)의 전제가
   이제 갖춰졌다. **계산만 한다 — 체인 봉인은 여전히 Sigan 몫이다.**
3-1. **COM 래퍼 ↔ `mari::app` 어댑터.** 2절의 마지막 빈칸이다.
3-2. ~~🔴 **에이전트 획 → `SiganPublisher` 배선.**~~ → **완료.** `record/` 모듈,
   규약 docs/06. 남은 것은 H1 의 마지막 구멍 셋 — 붙여넣기(`importPixels`),
   레이어 병합/삭제, 실행취소가 아직 기록 밖이다(docs/06 10절). 셋 다 결정 ① 의
   합성 프레임 쌍 틀을 그대로 쓸 수 있다.
4. **리포트 타입을 io 공용으로 올린다.**
5. **UI 레이어(Qt)를 시작한다.** 성능 목표를 측정할 대상이 그때 생긴다.
6. **libmypaint 어댑터.** 자리는 이미 있다.
7. **Sigan 와이어 폭을 VASE9 쪽과 합의한다.**

---

## 6. 이 문서를 고치는 규칙

- 테스트가 덮지 않는 것을 "됨"으로 옮기지 않는다.
- 컴파일 안 해본 것은 "작성됨, 미검증"으로 쓴다. "완료"라고 쓰지 않는다.
- 항목을 올릴 때는 **어느 테스트가 그걸 증명하는지** 파일명을 같이 적는다.
