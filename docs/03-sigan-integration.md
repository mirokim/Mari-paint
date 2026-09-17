# Mari Paint ↔ Sigan (VASE9) 연동 설계

> 대상: [`mirokim/VASE9`](https://github.com/mirokim/VASE9) — 제품명 **Sigan**, "작업 과정 증명 서비스"
> 조사 기준 커밋: `d5267fa` (2026-09-17)

---

## 1. Sigan이 무엇인지 (확인한 사실)

붓터치·필압·스트로크를 기록해서 **"이 그림이 사람이 그린 과정으로 만들어졌다"** 를 증명하는 인증 인프라다.

- **기록기**: C# WPF (`recorder/Vase9.Recorder`, net8.0-windows)
- **웹**: Next.js + Supabase (발급·공개검증 `/verify/[serial]`·대시보드)
- **계약 포맷**: `.sigan` (= `vase9.log/0.2`, gzip JSON)
- **신뢰 구조**: 획 단위 sha256 **해시체인** + 기기 개인키 **ECDSA P-256(ES256) 서명**
- 상태: 기록기 → 자동 업로드 → 웹 인증서 → 공개 검증까지 **프로덕션에서 동작 중**

### 1.1 Sigan이 지금 겪고 있는 근본 문제

Sigan은 포토샵·클립스튜디오 **안으로 들어갈 수 없다.** 그래서 밖에서 훔쳐본다.

| 방식 | 대상 | 상태 |
|---|---|---|
| **RawInput(HID) + `RIDEV_INPUTSINK`** | Windows Ink 모드 앱 | 주 경로 |
| **Wintab32.dll 프록시 주입 → 명명 파이프** | WinTab 모드 앱(CSP 등) | 보조 경로 |

그 결과 문서에 이렇게 적혀 있다:

> 🔴 **WinTab 탈락**: CSP도 WinTab을 써서 CSP가 그리는 동안 태블릿 컨텍스트를 가로챔 → 우리 시스템
> 컨텍스트가 굶음. WinTab 대 WinTab은 원리적 충돌.
>
> ✅ **CSP 확인됨**: CSP를 Windows Ink 모드로 두면 정상 캡처. (WinTab 모드면 CSP가 펜 HID를 죽여 못 봄
> → 사용자에게 "CSP는 Windows Ink 권장" 안내 필요.)

**밖에서 보기 때문에 못 채우는 칸들이 스키마에 그대로 남아 있다:**

| 필드 | 현재 | 이유 |
|---|---|---|
| `Segment.StartCanvasHash` / `EndCanvasHash` | **v0: null** | 남의 앱 픽셀을 못 읽는다 |
| `Segment.CanvasContinuous` | **v0: null** | 같은 이유 |
| `Stroke.Points` 의 `x, y` | **화면 좌표** | 캔버스 좌표를 모른다 |
| `ViewEvents` (줌·회전) | 별도 프로브로 **추정** | `Vase9.ViewProbe`, `Vase9.TabOcrProbe` 가 화면을 뜯어본다 |
| `LogEvent(paste)` 의 `Src` | 클립보드로 **추정** | 앱 내부 붙여넣기를 직접 못 본다 |
| `Outputs` (작품 파일 해시) | 사용자가 **수동 지정** | CSP 창 제목에 파일명이 없어서 자동 감지 폐기 |

---

## 2. 핵심 통찰 — Mari Paint는 "밖"이 아니라 "안"이다

**Sigan이 남의 앱에서 훔쳐봐야 했던 정보를, Mari Paint는 그냥 알고 있다.**

```
 [기존]  펜 ──HID──► CSP/PS (블랙박스) ──?──► Sigan 이 밖에서 추측
                                                · 화면 좌표뿐
                                                · 픽셀 못 봄
                                                · 줌/회전은 OCR로 추정

 [Mari]  펜 ──► Mari Paint ──직접 발행──► Sigan
                                                · 캔버스 좌표 (정확)
                                                · 레이어·브러시 ID
                                                · 픽셀 해시 (진짜)
                                                · 줌/회전 (추정 아님)
                                                · 저장 파일 해시 (자동)
```

이건 기능 하나가 아니라 **두 제품 공통의 차별점**이다.

- Mari Paint 쪽: *"과정 증명이 내장된 유일한 페인트 툴"*
- Sigan 쪽: *"추정이 아니라 원본에서 나온 최상위 신뢰 등급 기록"*

Sigan 문서의 `supervised`(감독 세션) 상위 신뢰 등급 개념이 이미 있으므로,
**`native` 등급**을 하나 더 두는 게 자연스럽다.

---

## 3. 🔴 지금 당장 반영해야 할 제약 — WinTab 금지

[02-architecture.md](./02-architecture.md) 5절에서 "Wintab / Windows Ink 둘 다 구현하고 사용자가 고른다"고
썼는데, **Sigan을 고려하면 이건 틀렸다.**

> **Mari Paint가 WinTab 모드로 돌면 Sigan의 RawInput 경로가 침묵한다.**
> CSP에서 이미 실측된 현상이다. WinTab 앱은 펜 HID를 독점해서 죽인다.

그러면 Sigan은 Mari Paint를 기록하려고 **자기 앱에 Wintab32.dll 프록시를 주입**해야 하는,
말이 안 되는 상황이 된다.

**결정:**

| | 정책 |
|---|---|
| **기본 입력 경로** | **Windows Ink (WM_POINTER)** — 고정 기본값 |
| WinTab | 구현하되 **"고급" 설정에 숨김**. 켤 때 *"Sigan 기록이 중단됩니다"* 경고 |
| 근거 | Sigan RawInput 호환 + Mari는 어차피 직접 발행하므로 WinTab 정밀도 이점이 필요 없다 |

이 한 줄이 Sigan 리포를 안 봤으면 절대 안 나왔을 결정이다.

---

## 4. 연동 아키텍처 — 두 채널로 나눈다

전부 COM으로 하면 안 된다. **스트로크 점은 초당 수백 개**가 나오는데 COM 마샬링을 태우면
레이턴시 예산(16ms)이 무너진다.

```
┌──────────────────────┐                          ┌──────────────────────┐
│    Mari Paint        │                          │   Sigan Recorder     │
│                      │   ① 제어 (저빈도)          │   (C# WPF)           │
│  COM: IMariApp    ◄──┼──────────────────────────┼── C#은 COM 인터롭이   │
│                      │   세션 시작/종료, 페어링,    │   언어에 내장        │
│                      │   문서 열기, 상태 조회      │                      │
│                      │                          │                      │
│  파이프 클라이언트 ───┼──────────────────────────┼─► 명명 파이프 서버     │
│                      │   ② 스트로크 (고빈도)       │   (기존 패턴 재사용)  │
│                      │   고정 길이 프레임 스트림   │                      │
│                      │                          │                      │
│                   ◄──┼──────────────────────────┼── ③ 이벤트 싱크       │
│                      │   IMariEventSink         │   (COM 연결점)        │
└──────────────────────┘                          └──────────────────────┘
```

### ① 제어 채널 — COM (`IMariApplication`)
저빈도. 세션 라이프사이클, 문서 조작, 상태 조회.
C#은 COM 인터롭이 언어에 내장돼 있어서 Sigan 쪽 작업이 거의 없다.

### ② 스트로크 채널 — 명명 파이프
**Sigan이 이미 쓰고 있는 방식을 그대로 따른다.** (`recorder/Vase9.Recorder/Core/WinTabBridge.cs`)

기존 `sigan-wintab` 파이프의 검증된 설계:
- 고정 길이 프레임(48B)을 백그라운드 스레드가 읽어 큐에 적재 → UI 스레드가 `Drain`
- **파이프 ACL을 현재 사용자로 제한** + **세션 토큰 32바이트 핸드셰이크** (위조 프레임 주입 방어)
- 범위 이탈 프레임 sanity 검사 → 스트라이드 밀림 감지
- 큐 오버플로 시 드롭 카운터

→ Mari는 **`sigan-native` 라는 별도 파이프**에 같은 규약(토큰 + 고정 프레임)으로 붙는다.
기존 파이프를 재사용하지 않는 이유: 프레임에 담을 정보가 더 많다(캔버스 좌표·레이어·브러시).

**Mari 네이티브 프레임 (제안)**

| 필드 | 크기 | 비고 |
|---|---|---|
| `t` | 8B | 세션 기준 ms |
| `cx, cy` | 8B | **캔버스 좌표** (화면 아님) — 줌·회전·팬과 무관한 불변값 |
| `p` | 4B | 필압 0~1 |
| `tiltX, tiltY` | 8B | 각도(°) |
| `rotation, velocity` | 8B | 펜 회전, 속도 |
| `layerId` | 4B | |
| `brushId` | 4B | |
| `flags` | 4B | down / up / move / eraser |

**캔버스 좌표가 핵심이다.** 지금 Sigan은 화면 좌표만 있어서 작가가 캔버스를 회전하거나
줌하면 같은 획이 다른 궤적으로 기록된다. 그래서 `ViewProbe`/`TabOcrProbe` 같은 보정 장치를
따로 만들어야 했다. **캔버스 좌표로 주면 그 문제 자체가 사라진다.**

### ③ 이벤트 채널 — COM 연결점 (`IMariEventSink`)
Mari가 발생시키는 의미 있는 사건을 Sigan이 수신한다.

| 이벤트 | Sigan의 어느 칸을 채우나 |
|---|---|
| `OnDocumentOpened(path, hash, w, h)` | `Work.OpenHash`, `Work.OpenedAt`, `Canvas` |
| `OnDocumentSaved(path, hash, size)` | `Outputs[]` — **수동 지정이 없어진다** |
| `OnCanvasSnapshot(hash)` | `Segment.StartCanvasHash` / `EndCanvasHash` — **v0 null 해소** |
| `OnViewChanged(zoom, rotation)` | `ViewEvents[]` — **OCR 프로브 불필요** |
| `OnPaste(source)` | `LogEvent.Src` — **추정이 아니라 사실** |
| `OnUndo()` / `OnLayerChanged()` | `LogEvent` |
| `OnStrokeCompleted(id)` | 체인 진행 동기화 |

---

## 5. 두 증인 구조 — 이게 진짜 가치다

Sigan 문서가 스스로 인정한 가장 큰 약점:

> **"기록기 자체 변조"는 원리적 완전방어 불가(🟠).**
> 서명 바이너리 + 감독 세션 스팟체크로 완화.

Mari 연동은 이 문제를 **완전히는 못 풀지만, 위조 비용을 크게 올린다.**

```
              ┌── Mari 직접 발행 (캔버스 좌표, 레이어, 픽셀 해시)
   같은 획 ──┤
              └── Sigan RawInput (HID 원시 신호, 화면 좌표)

         두 경로가 서로를 대조한다
         ↓
   위조하려면 두 개를 시간·좌표까지 일치시켜 동시에 속여야 한다
```

- Mari는 Windows Ink를 쓰므로 Sigan의 RawInput 경로가 **동시에 살아 있다**(3절 결정 덕분).
- 두 기록은 **독립적으로** 만들어지는데 같은 펜 동작에서 나온다.
- 화면 좌표 ↔ 캔버스 좌표는 Mari가 보고한 뷰 변환으로 **상호 변환 가능** → 대조할 수 있다.
- 어긋나면? 정직하게 **"경로 불일치"로 표시**한다. 등급을 낮추지, 조용히 숨기지 않는다.

이건 Sigan의 기존 설계 철학과도 맞는다 —
문서 08의 *"막을 게 아니라 정직하게 드러낸다"*.

---

## 6. `.sigan` 스키마에 필요한 변경

기존 스키마를 **깨지 않는다.** v0.2 그대로 두고 선택 필드만 더한다.

```csharp
public sealed class Segment {
    // ... 기존 그대로 ...

    // 새로 — 기록 출처. 없으면 기존처럼 "밖에서 관찰"로 본다.
    public string? Source { get; set; }        // "rawinput" | "wintab-proxy" | "native"
    public string? SourceApp { get; set; }     // "mari-paint/0.1.0"

    // 새로 — native 일 때만. 화면 좌표 ↔ 캔버스 좌표 대조용.
    public List<ViewTransform>? Transforms { get; set; }
}

public sealed class Stroke {
    // 기존 Points = [t, x, y, p, tiltX, tiltY]  ← 그대로 둔다
    // native 는 여기에 뒤로 더 붙인다: [..., layerId, brushId]
    // ⚠ 서명 정본(SigPayload)이 배열 길이에 의존하면 웹 signature.ts 와 바이트 동치가 깨진다.
    //    반드시 C#/TS 양쪽 골든 테스트로 먼저 확인할 것.
}
```

> ⚠️ **서명 정본 주의.** VaseLog.cs 주석이 경고하고 있다 — C#(`Core/Recording.cs`)과
> 웹(`lib/signature.ts`)이 **같은 입력에 같은 바이트**를 내야 한다. 예전에 `DeviceInfo`가
> null이냐 `new()`냐로 두 언어가 갈린 전례가 있다. 스키마를 건드리면 **공유 골든 테스트부터** 고친다.
> 필드를 더할 때는 **있을 때만 서명에 붙이는** 방식(기존 `:src` 선례)을 따라 옛 인증서 바이트를 보존한다.

---

## 7. 단계별 계획

| 단계 | 내용 | Mari 마일스톤 | 검증 |
|---|---|---|---|
| **S0** | Mari 입력을 Windows Ink 고정 | M1 | Sigan RawInput이 Mari 획을 잡는다 (= CSP와 동급) |
| **S1** | `sigan-native` 파이프 발행 | M1~M4 | 캔버스 좌표 획이 `.sigan`에 들어간다 |
| **S2** | COM 이벤트 싱크 | M4 | 저장 시 `Outputs`가 **자동으로** 채워진다 |
| **S3** | 픽셀 스냅샷 해시 | M4 | `StartCanvasHash`/`EndCanvasHash`가 **v0 null을 벗어난다** |
| **S4** | 두 증인 대조 + `native` 등급 | M5 | 웹 검증 페이지에 경로 일치 여부 표시 |

**S0가 제일 중요하고 제일 싸다.** Mari가 Windows Ink만 쓰면, 코드 한 줄 연동 안 해도
Sigan이 오늘 당장 Mari를 기록할 수 있다. 나머지는 품질 향상이다.

---

## 8. 남은 질문

1. **`.sigan` 서명 키를 누가 쥐나?**
   Sigan 기록기가 기기 키로 서명한다. Mari가 직접 `.sigan`을 쓰는 게 아니라
   **Sigan에게 공급만 하는** 구조가 맞다고 본다 — 신뢰 앵커를 한 곳에 모아야 한다. 동의하는지?

2. **Mari가 Sigan 없이도 자체 기록을 남겨야 하나?**
   (Sigan 미설치 사용자용 `.ora` 내장 로그 → 나중에 Sigan에 제출)
   가능하지만 서명 없는 기록이라 신뢰 등급이 낮다. 할지 말지.

3. **파이프 vs 공유 메모리.**
   픽셀 스냅샷(S3)은 파이프로 보내기엔 크다. 8bf 호스트와 같은 **공유 메모리**를 쓸지.

4. **Sigan의 앱 화이트리스트** (`Core/Foreground.cs`)에 Mari Paint를 등록해야 한다.
   Mari 프로세스명 확정 필요 — `mari-paint.exe` 로 갈지.
