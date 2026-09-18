# 다음 할 일 — Qt UI (로컬 머신)

> 작성 2026-09-17. 이 컨테이너는 Linux + Qt 없음이라 **여기서 더 진행할 수 없는 지점**에 도달했다.
> 이 문서는 로컬(Windows + Qt) 작업을 위한 인수인계다.
>
> **2026-09-18 갱신 — 로컬(Windows) 작업 결과.** 0절에 지금 상태를 적었다. 1~8절은 원래 인수인계이고,
> 이미 끝난 항목은 ✅ 로 표시했다.

---

## 0. 2026-09-18 현재 상태 (Windows 11 · MSVC 19.44 · Qt 6.9.3)

```
빌드: MSVC 19.44 + Ninja + vcpkg   경고 0 · 오류 0   (scriptsuild-win.cmd all)
테스트: ctest 55/55 통과 (Windows 에서 처음)
게이트: scripts/ci/check-no-wintab.ps1 통과 — 63개 바이너리
GUI: ui/ 컴파일·링크 됨. 🔴 실행 미검증 (아래)
```

| 항목 | 상태 |
|---|---|
| `platform/win` · `hosts` 컴파일 | ✅ 처음으로 컴파일·링크된다 (커밋 c684c25) |
| Qt 설치 (LGPL 모듈만) | ✅ `C:\Qt.9.3\msvc2022_64` — aqtinstall 로 qtbase·qtsvg 만. 계정 불필요 |
| 3.1 캔버스 위젯 | ✅ `ui/canvas_widget.*` — 더티만 합성, 뷰 변환은 위젯 소유 |
| 3.2 WM_POINTER | ✅ `nativeEvent()` → `platform/win` PointerInput. QTabletEvent 0줄 |
| 3.3 StrokeEntry 로 흘리기 | ✅ `app::LiveStroke` 가 입구. **새 발행 코드 0줄** |
| 3.4 최소 UI | ✅ 툴바·레이어 도크·메뉴·상태 표시줄 (`ui/main_window.*`) |
| 3.5 창 방식 | 단일 창 + 도킹으로 시작했다. 캔버스는 이 결정을 모른다 |
| LGPL 게이트 | ✅ `ui/CMakeLists.txt` 의 `mari_check_qt_license()` — 허용 밖 Qt 모듈이면 FATAL_ERROR |
| **GUI 실행** | 🔴 **미검증.** 창은 떴으나 몇 초 뒤 0xc0000409(abort). 원인 미상 — 아래 0.2 |
| 펜→화면 16ms | 🔴 못 쟀다. 측정 코드는 들어가 있다(`CanvasWidget::latency()`, 상태 표시줄) |

### 0.1 빌드하는 법 (Windows)

```powershell
scriptsuild-win.cmd all        # configure + build + ctest
scriptsuild-win.cmd build      # 빌드만
build\cli\mari-paint.exe         # 인자 없음 = GUI. 인자 있음 = 헤드리스 CLI (같은 바이너리)
```

`build-win.cmd` 가 하는 것: vcvars64 → `TMP=C:\Dev	mp` → vcpkg 툴체인 → `CMAKE_PREFIX_PATH=%QT_DIR%`.
vcpkg 는 `C:\Devcpkg` (zlib · libpng · sqlite3, x64-windows). 다른 곳이면 `set VCPKG_ROOT=`.

🔴 **Windows 에서 처음 만난 함정** (전부 고쳐 뒀지만 다시 만날 수 있다):
1. **사용자 프로필 경로에 한글이 있으면 cl.exe 가 D8050(c1.dll 실행 불가)로 죽는다.** vcpkg 가
   환경을 깎아 넘길 때 특히. `TMP` 를 ASCII 경로로 — 래퍼가 한다.
2. **MIDL 은 문자열 리터럴에 UTF-8 한글을 못 읽는다.** 렉서가 CP949 DBCS 로 읽어 닫는 따옴표를
   삼키고 엉뚱한 줄에서 MIDL2025 가 난다. `/cpp_opt "/utf-8"` 로도 안 된다. `mari.idl` 의
   helpstring 은 전부 ASCII 영어다. 주석은 한글이어도 된다.
3. **tlb 를 박는 .rc 가 tlb 변경을 못 봤다.** IDL 을 고쳐도 옛 tlb 가 exe 에 남았다. `OBJECT_DEPENDS` 로
   걸어 뒀다(`hosts/CMakeLists.txt`). 게이트가 옛 문자열을 잡아서 알게 됐다.
4. **PowerShell 5.1 은 BOM 없는 .ps1 을 ANSI 로 읽는다.** `check-no-wintab.ps1` 에 BOM 을 넣었다.
   빈 배열이 `$null` 로 풀려 StrictMode 에서 `.Count` 가 터지는 것, `-Include` 가 `-LiteralPath` 와
   같이 안 걸러지는 것도 고쳤다. `.gitattributes` 가 `.cmd`·`.ps1` 을 CRLF 로 고정한다.
5. **check-no-wintab 이 진짜로 잡은 것:** helpstring 의 "WinTab" 단어가 tlb → exe 에 박혀 게이트가
   빨개졌다. 바이너리에 들어가는 문자열에 그 단어를 쓰지 마라. 게이트는 살아 있다.
6. **std::system / cmd.exe 는 테스트에 못 쓴다.** 반환값 규약이 다르고(rc 그대로) 작은따옴표를 모른다.
   `include/mari/test/sys.hpp` 의 `runProcess()`(CreateProcessW) · `rssBytes()`(GetProcessMemoryInfo) 로
   갈았다. POSIX 경로는 예전 그대로다.

### 0.2 🔴 지금 막힌 것 — 다음 사람이 제일 먼저 할 일

**(a) Smart App Control 이 로컬 빌드를 전부 막는다.** 이 머신에서 작업 도중 SAC 가 평가 모드 → 강제로
넘어갔다(`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy` 의 `VerifiedAndReputablePolicyState=1`,
`SAC_PreviousState=2`). 그 뒤로 서명 없는 exe 는 **전부** 4551(Device Guard) 로 막힌다 — `mari-paint.exe`
도, `core_tile.exe` 같은 테스트도. 그 전에 ctest 가 가끔 `BAD_COMMAND` 로 흔들린 것도 평가 모드의
같은 현상이었다.
→ Windows 보안 → 앱 및 브라우저 컨트롤 → 스마트 앱 컨트롤 → **끄기.** (끄면 다시 켤 수 없다 —
사용자가 정할 일이라 여기서 건드리지 않았다.) 끄기 전엔 이 리포의 **아무것도 실행할 수 없다.**

**(b) GUI 가 몇 초 만에 abort 한다.** 창이 뜬 뒤 3~8초 사이에 0xc0000409(FAST_FAIL_FATAL_APP_EXIT).
SAC 가 켜지기 전 두 번 재현됐다. 원인 후보:
- `Result::value()` 를 실패 결과에 부르면 `std::bad_variant_access` → terminate → 이 증상이다.
  새 코드(`ui/`, `app/src/live_stroke.cpp`)에서 확인 안 한 `.value()` 는 못 찾았다.
- Qt 가 `EnableMouseInPointer` 이후의 마우스 WM_POINTER 를 어떻게 다루는지(qFatal?) 미확인.
다음 실행이 이유를 말하게 해 뒀다:
```powershell
$env:MARI_GUI_TRACE='1'; build\cli\mari-paint.exe 2> gui_err.txt     # 마일스톤 + 예외 문구 + Qt 로그
```
`gui.cpp` 의 terminate 핸들러가 예외 `what()` 을 찍고, `QT_LOGGING_TO_CONSOLE=1` 을 강제해 Qt 경고가
파일로 나온다(안 하면 OutputDebugString 으로 사라진다).

**(c) 그다음:** 4절의 측정. 펜→화면은 상태 표시줄 오른쪽에 `펜→화면 X ms (평균 · 최대 · >16ms n/N)` 로
바로 뜬다. 지금은 파이프라인 [1]~[4] 가 UI 스레드에서 돈다 — **먼저 재고** 넘겨야 하면 넘긴다.

### 0.3 GUI 구조 (한눈에)

```
cli/src/main.cpp        인자 없음 → ui::runGui()  /  있음 → cli::runCli()   (같은 바이너리)
ui/gui.cpp              QApplication · SiganRecorderFactory(LOCALAPPDATA\Mari\Mari Paint\journal) · Application
ui/main_window.*        툴바 · 레이어 도크 · 메뉴 · 상태 표시줄. 붓 목록은 brush::builtinPresets() (agent 와 동일)
ui/canvas_widget.*      HWND 소유 · nativeEvent → PointerInput → LiveStroke · 더티만 합성 · 뷰 변환 · 지연 측정
app/live_stroke.*       엔진 → 실행취소(증분) → 파이프라인 → StrokeEntry → finish → 고장이면 롤백
```

좌표계는 세 개다 — 캔버스 px · 물리 클라이언트 px(WM_POINTER, ViewTransform) · Qt 논리 px.
물리↔논리 변환(`devicePixelRatio`)은 `CanvasWidget` 안에서만 곱한다. Qt 6 은 Per-Monitor-V2 가 기본이다.

---

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

### 3.1 캔버스 위젯 (제일 먼저) ✅ 2026-09-18
- `QWidget` 서브클래스. `paintEvent`에서 **더티 영역만** 그린다.
  `core`의 `Compositor`가 이미 더티 타일을 준다 — 전체 재합성 코드를 새로 쓰지 마라.
- 뷰 변환(줌·회전·팬)은 **위젯이 소유**한다. 캔버스는 화면을 모른다.
- `QImage`로 타일을 감싸 `drawImage`. 처음엔 CPU 합성으로 충분하다. GPU는 M6다.

### 3.2 🔴 펜 입력 — Windows Ink만 ✅ 2026-09-18 (실행 미검증)
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

### 3.3 🔴 획을 `app::StrokeEntry`로 흘려라 — 새 경로를 만들지 마라 ✅ 2026-09-18 (`app::LiveStroke`)

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

### 3.4 최소 UI ✅ 2026-09-18
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

1. ~~**`platform/win`·`hosts`는 컴파일된 적이 없다.**~~ ✅ 2026-09-18 컴파일된다(0절). 8bf 구조체 오프셋 추측은 그대로다 — Adobe SDK 헤더가 없어 8bf 구조체를
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
