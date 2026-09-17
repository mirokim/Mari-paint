# Mari Paint — 아키텍처 설계

> 전제: [01-research.md](./01-research.md) 의 채택 결정
> 목표: **가볍고 빠르다 · Windows COM으로 어디든 붙는다 · 포토샵/클립스튜디오 자산을 그대로 쓴다**

---

## 1. 설계 원칙 4가지

1. **코어는 UI를 모른다.** `mari-core`는 Qt에 의존하지 않는 순수 C++. 테스트와 이식이 쉬워진다.
2. **호환 레이어는 전부 선택적이다.** PSD/abr/sut/8bf는 플러그인 모듈. 안 쓰면 로드조차 안 한다.
   → 이게 "가볍다"를 실제로 보장하는 유일한 방법이다.
3. **남의 코드는 내 프로세스에 들이지 않는다.** .8bf 플러그인은 무조건 별도 프로세스.
4. **입력 지연이 최우선이다.** 펜이 닿고 픽셀이 보이기까지가 제품의 품질이다. 여기에 예산을 몰아준다.
5. **증명 가능하게 그린다. 증명하지는 않는다.** 획·좌표·픽셀 해시는 Mari가 낸다.
   체인 봉인·서명·등급 판정은 **Sigan의 몫**이고 Mari에 들어오지 않는다.
   → [03-sigan-integration.md](./03-sigan-integration.md) 2절 경계선.

---

## 2. 모듈 구성

```
mari-paint/
├── core/           mari-core      Qt 의존 없음. 순수 C++20
│   ├── tile/         타일 기반 레이어 저장 (64×64 타일, COW)
│   ├── color/        LittleCMS 래핑. 8/16/32bit, sRGB/CMYK
│   ├── layer/        레이어 트리, 블렌드 모드, 마스크
│   ├── stroke/       스트로크 파이프라인 (입력 → 보간 → 스탬프)
│   └── brush/        브러시 추상화 (IBrushEngine)
│
├── engines/
│   ├── mypaint/      libmypaint 어댑터 (ISC)  ← 1차 엔진
│   └── native/       자체 GPU 엔진            ← 2차, 나중
│
├── io/             선택적 로드
│   ├── ora/          OpenRaster — 네이티브 포맷
│   ├── psd/          psd_sdk 읽기 + 자체 쓰기
│   ├── abr/          Photoshop 브러시 → IBrushPreset
│   └── sut/          Clip Studio 브러시 → IBrushPreset  (SQLite)
│
├── input/
│   ├── wintab/       Wintab 백엔드
│   └── winink/       WM_POINTER 백엔드
│
├── ui/             mari-ui        Qt 6. LGPL 모듈만 사용
│
├── com/            mari-com       COM 서버 등록, 인터페이스 구현
│
└── hosts/
    ├── mari-8bf-host-x64.exe
    └── mari-8bf-host-x86.exe      32비트 플러그인 전용
```

**의존 방향은 한 방향이다.** `ui → core`, `com → core`, `io → core`.
core는 아무것도 의존하지 않는다. 역방향 의존이 생기면 리뷰에서 막는다.

---

## 3. 캔버스 — 타일 기반

전체 캔버스를 한 덩어리로 잡으면 8K 캔버스 하나에 수 GB가 날아간다.

```
캔버스 = 타일의 희소 맵(sparse map)
  · 타일 크기: 64×64 px
  · 빈 타일은 메모리를 안 쓴다 (null 타일 공유)
  · Copy-on-Write: 레이어 복제 / 실행취소 스냅샷이 O(1)
  · 실행취소 = 변경된 타일만 보관
```

효과: 4000×4000 캔버스에서 구석에 점 하나 찍으면 타일 **1개**만 메모리에 잡힌다.
Krita가 쓰는 방식이고, "가볍다"를 만드는 가장 큰 한 수다.

---

## 4. 스트로크 파이프라인 — 지연 시간이 전부

```
펜 이벤트 (Wintab / WinInk)
   │  ← 별도 입력 스레드. UI 스레드를 절대 기다리지 않는다
   ▼
[1] 정규화    좌표·필압·틸트·회전 → 장치 독립 형식
   ▼
[2] 스무딩    떨림 보정 (끌 수 있어야 한다. 선 느낌을 바꾼다)
   ▼
[3] 보간      이벤트 사이를 스플라인으로 채운다 (스탬프 간격 기준)
   ▼
[4] 엔진      IBrushEngine::stamp() → 더티 타일 목록 반환
   ▼
[5] 합성      더티 타일만 블렌딩
   ▼
[6] 표시      더티 영역만 화면 갱신
```

지켜야 할 것:
- **[1]~[4]는 UI 스레드 밖**에서 돈다. UI가 버벅여도 선은 끊기지 않는다.
- **[5],[6]은 더티 영역만** 건드린다. 전체 캔버스를 다시 그리는 코드는 절대 넣지 않는다.
- 목표: 펜 입력 → 화면 표시 **16ms 이내** (60Hz 기준 1프레임).

---

## 5. 브러시 호환 — 공통 중간 표현

포토샵과 클립스튜디오 브러시를 "같이 쓴다"는 건 **하나의 공통 모델로 번역**한다는 뜻이다.

```
 .abr  ──┐
 .sut  ──┼──►  MariBrushPreset  ──►  IBrushEngine (libmypaint 등)
 .myb  ──┤      (공통 중간 표현)
 .kpp  ──┘
```

`MariBrushPreset` 이 담는 것:

| 항목 | 내용 |
|---|---|
| 팁 | 비트맵 또는 절차적 모양, 크기, 각도, 종횡비 |
| 간격 | spacing (% 단위) |
| 동적 반응 | `{입력} → {출력}` 커브 맵 |
| 입력 | 필압, 틸트, 방위각, 속도, 랜덤 |
| 출력 | 크기, 불투명도, flow, 원형도, 회전, 흩뿌림 |
| 텍스처 | 페이퍼 텍스처, 혼합 모드 |

**번역 시 원칙 — 정직하게 실패한다.**
포맷마다 표현력이 다르므로 100% 재현은 불가능하다.
번역할 수 없는 파라미터는 조용히 버리지 말고 **임포트 리포트에 남긴다.**
"이 브러시의 X, Y 설정은 반영되지 않았습니다" 라고 사용자에게 보여준다.
Krita의 .abr 임포트가 욕먹는 이유가 정확히 이걸 안 해서다.

### 5.1 .sut 파서 주의사항
`.sut`는 SQLite다. 다만 **컬럼 집합이 CSP 버전마다 다르다.**

```cpp
// 하면 안 되는 것 — 컬럼 이름 하드코딩
stmt.bind("SELECT BrushSize, BrushOpacity FROM Variant");

// 해야 하는 것 — 런타임 조회 후 있는 것만 매핑
auto columns = queryColumnNames(db, "Variant");   // PRAGMA table_info
for (auto& [sutName, mariField] : kMapping)
    if (columns.contains(sutName)) map(...);
    else report.missing.push_back(sutName);
```

텍스처는 `FileData` 컬럼 안의 **비압축 tar**에서 PNG를 꺼낸다.
필압 커브는 `*effector` 컬럼의 **빅엔디안 blob** — 리버싱이 필요하고, 일정을 넉넉히 잡아야 한다.

---

## 6. COM 레이어

### 6.1 인터페이스 초안

```idl
[object, uuid(...), dual]
interface IMariApplication : IDispatch {
    HRESULT Documents([out, retval] IMariDocuments** ppDocs);
    HRESULT Open([in] BSTR path, [out, retval] IMariDocument** ppDoc);
    HRESULT CreateDocument([in] LONG w, [in] LONG h, [out, retval] IMariDocument** ppDoc);
    [propget] HRESULT Version([out, retval] BSTR* pVal);
}

[object, uuid(...), dual]
interface IMariDocument : IDispatch {
    [propget] HRESULT Layers([out, retval] IMariLayers** ppVal);
    [propget] HRESULT Width([out, retval] LONG* pVal);
    [propget] HRESULT Height([out, retval] LONG* pVal);
    HRESULT SaveAs([in] BSTR path, [in] BSTR format);
    HRESULT ExportPixels([in] LONG layerIndex, [out, retval] SAFEARRAY(BYTE)* pData);
    HRESULT ImportPixels([in] BSTR layerName, [in] SAFEARRAY(BYTE) data,
                         [in] LONG w, [in] LONG h);
}
```

`dual` 인터페이스(= IDispatch 상속)로 만드는 게 중요하다.
그래야 C++/C#뿐 아니라 **Python(pywin32), VBA, PowerShell, AutoHotkey** 까지 전부 붙는다.
이게 "다양한 프로그램과 호환"의 실체다.

### 6.2 8bf 호스트 격리

```
mari-paint.exe                       mari-8bf-host-x86.exe
     │                                       │
     │── COM: IMari8bfHost::Apply() ────────►│
     │   (공유 메모리로 픽셀 전달)              │── LoadLibrary("plugin.8bf")
     │                                       │── PIPL 리소스 읽기 → 메뉴 구성
     │                                       │── FilterRecord + suite callback 제공
     │◄─────── 결과 픽셀 + 상태 ───────────────│
     │                                       │
     │   호스트가 죽으면? → 에러만 표시, 본체는 멀쩡
```

- 픽셀은 COM 마샬링이 아니라 **공유 메모리(파일 매핑)**로 넘긴다. 대용량 복사를 피한다.
- 호스트 프로세스에 **타임아웃**을 건다. 무한루프 도는 플러그인을 강제 종료할 수 있어야 한다.

### 6.3 등록
`LocalServer32` 로 등록한다 (`InprocServer32` 아님). 프로세스 분리가 설계의 핵심이므로.

---

## 7. Sigan (VASE9) 연동

**Mari Paint의 목적 중 하나다.** 부가 기능이 아니다.
전체 설계는 **[03-sigan-integration.md](./03-sigan-integration.md)**. 요약만 적는다.

- Sigan = 작업 과정 증명 서비스. C# WPF 기록기 + Next.js/Supabase 웹. 프로덕션 동작 중.
- 지금은 포토샵·CSP를 **밖에서** RawInput으로 훔쳐본다 → 캔버스 좌표·픽셀을 모른다.
- Mari는 **안에 있다.** 그래서 Sigan이 완전한 증거를 얻을 수 있는 유일한 페인트 툴이 된다.
- **분리 유지.** 내장하지 않는다 — Sigan은 비공개·유료 상품이고, 오픈소스에 기록기를 넣으면
  위조 방어(서명 바이너리)가 무너진다.

**채널 분리 (중요):**

| 채널 | 방식 | 이유 |
|---|---|---|
| 제어 (세션·문서·페어링) | **COM** `IMariApplication` | 저빈도. C#은 COM 인터롭 내장 |
| 스트로크 (초당 수백 점) | **명명 파이프** `sigan-native` | COM 마샬링은 16ms 예산을 깬다 |
| 이벤트 (저장·뷰변경·붙여넣기) | **COM 연결점** `IMariEventSink` | Sigan의 빈 스키마 칸을 채운다 |

**🔴 입력 API 제약:** Mari는 **Windows Ink 고정, WinTab 미구현**이다.
WinTab 앱은 펜 HID를 독점해 죽여 Sigan의 기록을 침묵시킨다(CSP·포토샵·**Krita**에서 실측됨).
→ [01](./01-research.md) 3.5절, [03](./03-sigan-integration.md) 3절.

**🔴 오버플로 정책:** Sigan의 기존 HID 파이프는 큐가 차면 프레임을 버린다(`Dropped++`).
샘플링 경로에선 맞지만 **네이티브 경로는 정본 기록이라 버리면 안 된다.**
막히면 로컬 저널로 스풀한다 → [03](./03-sigan-integration.md) 4.2절.

## 8. 성능 목표 (측정 가능한 수치로)

| 항목 | 목표 | 참고 |
|---|---|---|
| 콜드 스타트 | **< 1.5초** | Krita는 보통 5~10초 |
| 빈 캔버스 메모리 (1920×1080) | **< 150MB** | |
| 펜 입력 → 화면 표시 | **< 16ms** | 60Hz 1프레임 |
| 8K 캔버스 열기 | < 3초 | |
| 설치 용량 (호환 모듈 제외) | **< 80MB** | |

이 수치들을 CI에서 자동 측정하고, **회귀하면 빌드를 깬다.**
"가볍다"는 의지가 아니라 테스트로 지키는 것이다.

---

## 9. 개발 로드맵

| 단계 | 산출물 | 검증 기준 |
|---|---|---|
| **M0** 기반 | 타일 캔버스 + 레이어 + .ora 저장/열기 | 그린 걸 저장하고 다시 연다 |
| **M1** 그리기 | libmypaint 연결 + Wintab/WinInk + 최소 UI | 필압 있는 선이 16ms 안에 그려진다 |
| **M2** 자산 호환 | .abr / .sut 임포트 + 임포트 리포트 | 실제 배포 브러시 20종이 비슷하게 재현된다 |
| **M3** 파일 호환 | .psd 읽기/쓰기 | Photoshop이 우리가 쓴 psd를 연다 |
| **M4** COM | IMariApplication 등록, Python/C#에서 제어 | 외부 스크립트로 문서 생성·저장 |
| **M5** 8bf | 격리 호스트 (x86/x64) | 대표 필터 플러그인이 크래시 없이 돈다 |
| **M6** 성능 | GPU 합성, 자체 엔진 | 8절 목표치 전부 통과 |

M0~M2가 "페인트 툴"이고, M3~M5가 "호환성"이다. 순서를 바꾸면 안 된다 —
그릴 수 없는 툴에 호환 기능을 붙여봐야 아무도 안 쓴다.

---

## 10. 위험 요소

| 위험 | 정도 | 대응 |
|---|---|---|
| .8bf 완전 호환은 아무도 못 했다 | 높음 | 목표를 "주요 필터 동작"으로 한정. 지원 목록을 공개한다 |
| .sut effector blob 리버싱 실패 | 중간 | 실패해도 팁+기본 파라미터는 살린다. 커브는 기본값 |
| CSP 버전업으로 .sut 스키마 변경 | 중간 | 관대한 매핑 + 알려진 스키마 테스트 픽스처 유지 |
| Wintab/WinInk 드라이버 충돌 | 높음 | 둘 다 구현. 자동 감지 + 수동 전환. 진단 화면 제공 |
| Qt GPL 전용 모듈 오염 | 낮음 | CI에서 링크된 Qt 모듈 라이선스 검사 |
| 기능 비대화로 "가볍다" 상실 | **높음** | 8절 수치를 CI 게이트로. 호환 모듈은 항상 선택적 |
| Mari가 WinTab을 로드해 Sigan 기록이 침묵 | 높음 | WinTab 미구현. CI에서 `wintab32.dll` 로드 검사 |
| Sigan 자동 업데이트 재시작으로 획 유실 | 중간 | 저널 + seq 연속성으로 재연결. 구간 분리 금지 ([03](./03-sigan-integration.md) 5.3) |
| 스키마 변경이 서명 바이트를 깸 | 중간 | 선택 필드만 추가. 공유 골든 테스트 선행 ([03](./03-sigan-integration.md) 8절) |
