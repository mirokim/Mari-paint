# Mari Paint — 구현 현황

> 기준: 2026-09-17 · g++ 13.3 / clang 18 · Linux 컨테이너
> 목적: **무엇이 실제로 도는지 정직하게 적는다.** 설계 문서(01~03)가 "하려는 것"이라면
> 이 문서는 "지금 되는 것"이다. 둘을 섞으면 아무도 리포를 믿지 않는다.

---

## 0. 한 줄 요약

**플랫폼 중립 코어는 다 됐고 테스트로 증명된다. Windows 레이어는 코드가 있을 뿐 한 번도
컴파일되지 않았다. UI는 없다.**

```
빌드   : g++ 13.3 ✅  ·  clang 18 ✅   (경고 0 · 오류 0)
테스트 : 35개 실행파일 / 219 케이스 — 양쪽 컴파일러에서 100% 통과
         skip·disable 한 테스트 없음
```

| 모듈 | ctest 실행파일 | 상태 |
|---|---|---|
| foundation | 5 | ✅ 검증됨 |
| core | 5 | ✅ 검증됨 |
| stroke | 6 | ✅ 검증됨 |
| io/ora | 3 | ✅ 검증됨 |
| io/brush | 4 | ✅ 검증됨 |
| sigan | 7 | ✅ 검증됨 (Windows 파이프 분기만 미검증) |
| win | 4 | ⚠️ Win32 비의존 부분만 |
| integration | 1 | ✅ 검증됨 |

---

## 1. 구현됐고 Linux에서 실제로 검증된 것

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

또 하나 정직하게: **`IMariDocumentBridge` / `IMariAppBridge` 는 순수 가상 인터페이스인데
구현체가 리포 어디에도 없다.** COM 레이어는 주입받을 앱 객체를 기다리는 껍데기다.
UI/앱 레이어가 생겨야 채워진다.

---

## 3. 아직 아예 없는 것

| 항목 | 설계 위치 | 메모 |
|---|---|---|
| **UI (Qt 6)** | docs/02 2절 `ui/` | 리포에 Qt 코드 0줄. 그래서 docs/02 8절 성능 목표(콜드 스타트 · 메모리 · 16ms)를 **측정할 대상 자체가 없다** |
| **libmypaint 어댑터** | docs/02 2절 `engines/mypaint` | 지금 엔진은 자체 `native` 하나. `createEngine()` 에 한 줄 더하면 붙는 자리는 만들어 뒀다 |
| **.psd 읽기/쓰기** | docs/02 M3 | 시작 안 함 |
| **색 관리 (LittleCMS)** | docs/02 2절 `color/` | 지금은 RGBA8 sRGB 고정. 16/32bit · CMYK 없음 |
| **픽셀 해시 (SHA-256) 실제 계산** | docs/03 2절 · S3 | 🔴 IDL과 브리지 인터페이스에 `PixelHash`/`CanvasHash` 가 **선언만** 돼 있고 SHA-256 구현이 리포에 없다. docs/03 표에서 "해시 계산"은 Mari의 몫인데 아직 안 했다 |
| **GPU 합성** | docs/02 M6 | 전부 CPU |
| **레이어 마스크의 .psd/.ora 흡수** | — | 규약 자체가 미결(5절) |

---

## 4. 알려진 한계

1. **Sigan 와이어 식별자 폭.** `LayerId`/`BrushId` 는 u64인데 docs/03 4.1 프레임은 u32다.
   2^32를 넘으면 잘린다. 조용히 넘어가지 않고 `PublisherStats::idTruncations` 로 센다.
   표를 넓힐지는 Sigan 쪽과 합의가 필요하다.
2. **리포트 타입이 두 벌이다.** io/ora의 `Document::warnings` 는 `vector<string>` 이고,
   io/brush의 `ImportReport` 는 severity/key/message 구조다. io 공용으로 올리는 게 맞다.
3. **레이어 마스크 규약이 미결.** 현재: 없는 타일 = 0 = 가림. **Krita와 반대다.**
   .ora/.psd 임포터가 이 차이를 흡수해야 하는데 아직 안 했다.
4. **`duplicateLayer()`/`setLayerTiles()` 의 자리가 미결.** `layer_ops.hpp` 자유 함수로
   둘지 인터페이스로 올릴지.
5. **.sut effector blob은 추측이다.** 여러 레이아웃 가설을 시도하는 구조라 실제 CSP
   파일에서 커브가 맞는지는 검증되지 않았다. 실패해도 팁은 살아남는다.
6. **성능 수치는 하나도 측정되지 않았다.** docs/02 8절의 5개 목표 전부 미측정이고,
   CI 게이트도 없다. `stroke_bench` 는 회귀 감지용 알고리즘 처리량일 뿐이다.
7. **docs/03 10절 검증 8종 중 자동으로 도는 건 2개**(`canvas_xy_invariant`,
   `wintab_never_loaded` — 후자는 Windows 잡이 돌아야). 나머지는 실제 Sigan 바이너리 ·
   펜 하드웨어 · VASE9 리포와의 공유 골든 픽스처가 필요하다. `ci.yml` 의
   `verification-matrix` 잡이 이 현황을 매 빌드마다 출력한다.

---

## 5. 다음 단계 (우선순위 순)

1. **Windows CI 잡을 한 번 초록으로 만든다.** 2절 전체가 여기에 걸려 있다.
   이게 되기 전까지 COM·8bf·Windows Ink는 전부 추측이다.
2. **미결 규약 2건을 정한다** — 레이어 마스크 방향, `layer_ops` 의 자리.
   둘 다 .psd 임포터를 시작하기 전에 정해야 한다.
3. **SHA-256을 구현한다.** docs/03 S3(`StartCanvasHash`/`EndCanvasHash` 가 v0 null을
   벗어나는 것)의 전제이고, 지금 선언만 있는 상태가 제일 오해를 부른다.
4. **리포트 타입을 io 공용으로 올린다.**
5. **UI 레이어(Qt)를 시작한다.** 성능 목표를 측정할 대상이 그때 생긴다.
6. **libmypaint 어댑터.** 자리는 이미 있다.
7. **Sigan 와이어 폭을 VASE9 쪽과 합의한다.**

---

## 6. 이 문서를 고치는 규칙

- 테스트가 덮지 않는 것을 "됨"으로 옮기지 않는다.
- 컴파일 안 해본 것은 "작성됨, 미검증"으로 쓴다. "완료"라고 쓰지 않는다.
- 항목을 올릴 때는 **어느 테스트가 그걸 증명하는지** 파일명을 같이 적는다.
