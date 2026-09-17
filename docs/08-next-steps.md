# 다음 할 일 — Qt UI (로컬 머신)

> 작성 2026-09-17. 이 컨테이너는 Linux + Qt 없음이라 **여기서 더 진행할 수 없는 지점**에 도달했다.
> 이 문서는 로컬(Windows + Qt) 작업을 위한 인수인계다.

---

## 1. 지금 어디까지 왔나

```
빌드: g++ 13.3 · clang 18   경고 0 · 오류 0
테스트: ctest 55/55 통과 (425 케이스)
소스: 11개 모듈
```

| 모듈 | 상태 |
|---|---|
| `core` 타일 캔버스(COW)·레이어·블렌드 19종·더티 합성·실행취소 | ✅ 검증됨 |
| `stroke` 정규화→스무딩→보간→스탬프 + 네이티브 엔진 | ✅ 검증됨 |
| `crypto` SHA-256(NIST 벡터)·canvasHash·fileHash | ✅ 검증됨 |
| `io/ora` ZIP+PNG+stack.xml 왕복 | ✅ 검증됨 |
| `io/brush` .abr(ActionDescriptor)·.sut(SQLite) + ImportReport | ✅ 검증됨 |
| `sigan` 프레임·저널·재연결·버전협상 | ✅ 검증됨 |
| `app` 문서·**StrokeEntry(획의 유일한 입구)** | ✅ 검증됨 |
| `agent` 연산 38종·시각피드백·O(1)스냅샷·batch | ✅ 검증됨 |
| `record` sigan 배선 (mari::sigan 링크 유일 지점) | ✅ 검증됨 |
| `cli` 헤드리스 · `mcp` MCP 서버 | ✅ 검증됨 |
| `platform/win` COM·WM_POINTER, `hosts` 8bf | ⚠️ **작성됨, 컴파일 안 됨** |
| **GUI** | 🔴 **없음 (Qt 0줄)** |

**GUI가 없어서 막힌 것:**
- docs/02 8절 **펜→화면 16ms 측정 불가** (절반만 쟀다: 입력→픽셀 획당 7.2ms)
- 실제 펜으로 그려본 적 없음 — 필압·틸트가 진짜 태블릿에서 맞는지 미확인
- **Sigan이 실제로 Mari를 기록하는지 확인 못 함** (제품 목표의 최종 증명)

---

## 2. 로컬 환경 준비

### Qt 설치
**Qt 6.5 이상.** 온라인 설치 관리자에서 **오픈소스(LGPL)** 선택.

🔴 **쓸 모듈을 LGPL 범위로 제한해라** (docs/01 5절):

| 쓴다 | 쓰지 마라 |
|---|---|
| QtCore · QtGui · QtWidgets (LGPLv3) | **QtCharts** (GPL/상용) |
| QtSvg | QtDataVisualization |
| | 기타 GPL 전용 애드온 |

GPL 전용 모듈을 하나라도 링크하면 **Mari 전체가 GPL이 된다.** Apache-2.0 계획이 깨진다.
CI에 링크된 Qt 모듈 라이선스 검사를 넣어라(docs/02 10절 위험 표).

### 빌드
```powershell
# 기존(GUI 없이) — 먼저 이게 통과하는지 확인해라
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure    # 55/55 나와야 한다

# Windows 레이어까지 (여기서 처음으로 컴파일된다)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

⚠️ `platform/win`과 `hosts`는 **한 번도 컴파일된 적이 없다.** 오류가 쏟아질 것을 예상해라.
그게 정상이다 — 거기부터 고치고 시작하는 게 순서다.

---

## 3. M1 — Qt UI 작업 순서

**순서를 지켜라.** 3단계까지만 해도 16ms를 잴 수 있다.

### 3.1 캔버스 위젯 (제일 먼저)
- `QWidget` 서브클래스. `paintEvent`에서 **더티 영역만** 그린다.
  `core`의 `Compositor`가 이미 더티 타일을 준다 — 전체 재합성 코드를 새로 쓰지 마라.
- 뷰 변환(줌·회전·팬)은 **위젯이 소유**한다. 캔버스는 화면을 모른다.
- `QImage`로 타일을 감싸 `drawImage`. 처음엔 CPU 합성으로 충분하다. GPU는 M6다.

### 3.2 🔴 펜 입력 — Windows Ink만
```cpp
// nativeEvent() 에서 WM_POINTER 직접 처리
case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP:
    GetPointerPenInfo(pointerId, &penInfo);   // 필압·틸트·회전
```
Qt의 `QTabletEvent`는 **wintab32.dll을 요구한다.** 쓰지 마라.
`platform/win/input/`에 이미 정규화 코드가 있다 — 그걸 쓰고, 새로 쓰지 마라.

🔴 **금지:**
- `wintab32.dll` 로드 — CI가 `scripts/ci/check-no-wintab.ps1`로 막는다
- `QTabletEvent` 경로 사용
- **이유:** WinTab 앱은 펜 HID를 독점해 죽여 Sigan의 기록을 침묵시킨다
  (CSP·포토샵·Krita에서 실측). 목표는 Sigan의 `WinTabKeywords` 목록에 영원히 안 오르는 것.

**화면 좌표 → 캔버스 좌표 변환**은 뷰 변환 역행렬로. `platform/win`에 테스트된 코드가 있다
(`win_view_transform` 테스트 38케이스).

### 3.3 🔴 획을 `app::StrokeEntry`로 흘려라 — 새 경로를 만들지 마라

```cpp
// 사람 펜 획
auto src = StrokeSource::humanPen();          // core/origin.hpp 팩토리
app::StrokeEntry entry(doc, src, layerId, brushId, /*eraser=*/false);
entry.add(PenSample{ .pos = canvasPt, .pressure = p, .tiltX = tx, .tiltY = ty });
// ... 획이 끝나면 소멸자가 마무리
```

`include/mari/app/stroke_entry.hpp` 주석이 이미 못박고 있다:

> **GUI가 붙는 날 여기 말고 다른 곳에 발행 코드를 한 줄이라도 새로 쓰게 된다면
> 그건 기록이 두 갈래로 갈라졌다는 뜻이고, 설계가 어긋난 신호다.**

**WM_POINTER가 붙을 때 새로 쓸 발행 코드는 0줄이어야 한다.** `PenSample`로 정규화해서
넘기기만 하면 된다. 이게 지켜지면 사람 획이 자동으로 Sigan에 기록된다.

### 3.4 최소 UI
툴바(브러시 선택·크기·색) · 레이어 패널 · 캔버스. 그 이상은 나중에.
**docs/02 8절 목표를 지켜라** — 콜드 스타트 1.5초, 설치 80MB. UI를 키우면 바로 깨진다.

### 3.5 창 방식
docs/02는 COM 프로세스 분리를 말하지만 **창 레이아웃은 아직 안 정했다.**
초안에서 물었던 세 후보(플로팅 / 단일 창 / 도킹) 중 고르면 된다. 지금은 열려 있다.

---

## 4. 여기서 못 잰 것 — 로컬에서 재라

| 항목 | 목표 | 여기서 | 로컬에서 할 일 |
|---|---|---|---|
| **펜→화면** | **< 16ms** | 🔴 측정 불가 | **고속 카메라 또는 `QElapsedTimer`로 WM_POINTER 수신→`paintEvent` 완료까지.** 최우선 |
| 콜드 스타트 | < 1.5초 | 3.8ms (GUI 없이) | GUI 포함해서 다시 |
| 설치 용량 | < 80MB | 24.94MB (GUI·COM·8bf 없이) | Qt 런타임 포함해서 다시. **여기서 깨질 가능성이 높다** |
| 빈 캔버스 메모리 | < 150MB | 6.06MB | GUI 포함 |
| 8K 열기 | < 3초 | 550ms | 그대로일 것 |

**목표가 없는데 눈에 띈 것:** 8K `.ora` **저장이 9.0초**다. 여는 건 550ms인데 쓰는 게 16배 느리다.
게이트를 안 걸어 뒀으니 필요하면 최적화해라(PNG 압축 레벨이 유력한 원인).

---

## 5. Sigan 연동 최종 증명 (GUI가 생긴 뒤)

이게 **제품 목표의 최종 검증**이다. docs/03 10절.

1. Mari를 띄우고 **실제 펜으로** 그린다
2. Sigan 기록기를 켠다 → **RawInput이 Mari 획을 잡는지** 확인
   - 잡히면: Mari가 Windows Ink를 쓴다는 증명 (CSP와 동급 이상)
   - 안 잡히면: 어딘가 WinTab이 끼어든 것. `check-no-wintab.ps1`부터 돌려라
3. `sigan-native` 파이프로 **직접 발행**이 도착하는지 (VASE9 쪽 수신부 필요 — 아직 없음)
4. 🔴 **두 증인 대조** (docs/03 7절) — RawInput 기록과 네이티브 기록이 일치하는지
5. **AI 획 섞기** — agent-api로 몇 획 그리고 사람 획과 섞어서
   `--proof-out`의 비율이 맞는지. 이미 헤드리스에선 검증됐다:
   ```
   AI 붓질 2 · 영역 연산 1 · 변경 타일 83 · 유실 0 · grade=unsigned
   ```

**VASE9 쪽에 할 일:** `sigan-native` 파이프 수신부가 아직 없다. Mari 쪽은 발행 준비가 끝났다.

---

## 6. 그다음

| | 내용 | 비고 |
|---|---|---|
| **M3** | `.psd` 읽기/쓰기 | `psd_sdk` 읽기 + 자체 쓰기 (docs/01 3.4) |
| **M4** | COM 레이어 실제 동작 | MIDL 컴파일부터. Python/C#에서 제어 확인 |
| **M5** | 8bf 격리 호스트 | x86/x64 두 벌. 대표 필터로 크래시 없이 |
| **M6** | GPU 합성 · libmypaint 어댑터 | 성능 목표 전부 통과 |

---

## 7. 함정 — 미리 알고 가라

1. **`platform/win`·`hosts`는 컴파일된 적이 없다.** Adobe SDK 헤더가 없어 8bf 구조체를
   공개 문서 기준으로 **직접 선언**했다. 추측한 오프셋에 주석을 달아 뒀다 — 거기부터 의심해라.
2. **`IMariDocumentBridge` 구현체는 있지만 COM 래퍼와 연결은 미검증.**
3. **스트리밍은 콜백·소켓 푸시까지 됐지만 MCP는 여전히 폴링이다** (규약 제약, docs/07에 근거).
4. **사람 획 경로는 인터페이스만 검증됐다.** 테스트에서 `StrokeSource::humanPen()`을 주입해
   확인했을 뿐, **실제 펜 입력이 지나간 적은 없다.**
5. **선택 마스크가 그리기 핫패스에 들어갔다.** 전체 선택일 때 비용 0을 유지하도록 짰지만,
   Qt를 붙인 뒤 스탬프 벤치마크(현재 150,056/초)를 다시 재라.
6. **Qt 시그널/슬롯을 그리기 핫패스에 쓰지 마라.** 획당 7.2ms 예산이 금방 사라진다.
7. **`origin` 위조 방지를 약화시키지 마라.** 컴파일을 통과시키려고
   `StrokeSource()` 기본 생성자를 되살리거나 `friend`를 늘리고 싶어질 때가 온다.
   그건 설계를 고칠 신호지 우회할 신호가 아니다. 현재: 생성 지점 6곳, friend 1개.

---

## 8. 미결 (아직 안 정함)

1. **창 방식** — 플로팅 / 단일 창 / 도킹 (3.5절)
2. **레이어 마스크 규약** — 없는 타일 = 0 = 가림. Krita와 반대다. 이대로 갈지
3. **sigan 와이어 식별자 폭** — u32/u64. 지금은 u32이고 `idTruncations`로 세는 중
4. **8K `.ora` 저장 9초** — 게이트를 걸지, 그냥 둘지
