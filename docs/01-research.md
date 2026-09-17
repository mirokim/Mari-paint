# Mari Paint — 오픈소스 리서치

> 조사일: 2026-09-17 · 목적: "가볍고, 호환성 좋고, 최고 성능"인 오픈소스 페인트 툴을 만들기 위해
> 기존 오픈소스에서 **무엇을 가져오고 / 무엇을 피하고 / 무엇을 새로 만들지** 결정한다.

---

## 1. 요약 — 결론부터

| 질문 | 결론 |
|---|---|
| 처음부터 다 만들어야 하나? | **아니다.** 브러시 엔진(libmypaint)과 PSD 파서(psd_sdk)는 검증된 걸 쓴다. |
| Krita 코드를 가져올 수 있나? | **사실상 못 쓴다.** GPL-3.0이라 우리 라이선스를 GPL-3.0으로 강제한다. 참고만 한다. |
| 포토샵 브러시(.abr) 쓸 수 있나? | **가능하다.** 비공식이지만 파서 구현체가 여럿 있다. |
| 클립스튜디오 브러시(.sut) 쓸 수 있나? | **가능하다. 이게 핵심 발견이다.** .sut는 그냥 **SQLite DB 파일**이다. |
| 포토샵 플러그인(.8bf) 호스팅? | **가능하지만 제일 어렵다.** Windows 전용이고 별도 프로세스로 격리해야 한다. |
| "가볍다"와 "포토샵 호환"을 동시에? | **프로세스 분리로만 가능하다.** 무거운 호환 레이어를 본체에서 떼어낸다. |

---

## 2. 기존 오픈소스 페인트 툴 분석

### 2.1 Krita (GPL-3.0, C++/Qt)
디지털 페인팅 오픈소스의 사실상 표준이다.

**배울 점**
- 브러시 엔진을 **여러 개** 두고 프리셋이 엔진을 고르는 구조. 확장성이 좋다.
- LittleCMS 기반 컬러 매니지먼트로 8/16/32비트, CMYK까지 지원.
- 타일 기반 레이어 저장 — 캔버스 전체를 한 덩어리 메모리로 잡지 않는다.

**피할 점**
- **무겁다.** 기능이 20년 쌓이면서 시작 시간과 메모리가 커졌다. 우리 목표("가볍게")와 정면 충돌.
- **GPL-3.0.** 코드를 한 줄이라도 복사하면 Mari Paint 전체가 GPL-3.0이 된다.
- .abr 임포트가 **텍스처만** 가져온다. 브러시 동작(크기/간격/필압)은 사용자가 손으로 다시 맞춰야 한다.

> **판단:** 아키텍처는 참고하되 코드는 만지지 않는다. 우리는 .abr을 "텍스처만"이 아니라 **동작까지** 가져오는 걸 차별점으로 삼는다.

### 2.2 MyPaint / libmypaint (앱은 GPL, **libmypaint는 ISC**)
- libmypaint(= "brushlib")는 브러시 스트로크 생성만 담당하는 **독립 C 라이브러리**다.
- **ISC 라이선스** — MIT급 허용적 라이선스. 우리가 어떤 라이선스를 고르든 링크할 수 있다.
- Krita조차 자체 엔진 외에 libmypaint를 별도 엔진으로 내장했다. 검증이 끝났다는 뜻이다.
- 한계: 8비트 sRGB 고정, 알파 채널 개념이 약하다. 고비트 깊이는 우리가 감싸야 한다.

> **판단: 채택.** 브러시 엔진을 맨땅에서 만드는 건 수 년짜리 일이다. libmypaint를 1차 엔진으로 쓰고,
> 그 위에 우리 엔진(고비트/GPU)을 나중에 얹는다.

### 2.3 GIMP (GPL-3.0)
- 페인팅보다 **사진 편집** 지향. 브러시 필압 표현력이 Krita/CSP에 못 미친다.
- 하지만 **PSPI 플러그인**이 우리에게 중요하다 → 3.1절.

### 2.4 Aseprite / 기타
- Aseprite는 픽셀아트 전용이고 라이선스가 오픈소스가 아니다(소스 공개형 유료). 제외.
- 다만 `aseprite/psd` 라이브러리는 별도로 쓸 만하다.

---

## 3. 호환성 — 항목별 실현 가능성

### 3.1 포토샵 플러그인 (.8bf) — ★★☆☆☆ 어려움

- `.8bf`는 Adobe Photoshop SDK 규격의 **필터 플러그인 DLL**이다.
- 플러그인은 호스트가 제공하는 **수십 개의 callback suite**를 호출한다. 이걸 전부 정확히 흉내내야 동작한다.
- 선행 사례:
  - **PSPI** (Tor Lillqvist) — GIMP용 .8bf 호스트. **Windows 전용**이다. Adobe SDK가 Windows만 지원하기 때문.
  - **spetric/Photoshop-Plugin-Host** — .8bf 로딩/실행을 DLL 엔진으로 분리한 구현. 32/64비트 각각 빌드.
- 냉정한 평가: "완전 호환"을 이룬 오픈소스는 **아직 없다.** 잘 되는 플러그인과 깨지는 플러그인이 섞인다.

> **판단:** 목표를 **"자주 쓰는 필터 플러그인이 돈다"**로 낮춘다.
> 그리고 **반드시 별도 프로세스(`mari-8bf-host.exe`)로 격리**한다.
> 이유: 남의 DLL이 죽으면 우리 앱도 같이 죽는다. 작업 중인 그림이 날아간다.
> 이게 사용자가 요청한 **COM 방식**과 정확히 맞물린다 → 4절.

### 3.2 포토샵 브러시 (.abr) — ★★★★☆ 가능

- 공식 스펙이 **없다.** Photoshop CS2 이후 6.x 레이아웃을 쓰며, 내부에 Adobe의 직렬화된
  **ActionDescriptor** 바이너리가 들어 있다.
- 구조: 헤더 → `8BIM` 섹션들 → `samp`(브러시 팁 비트맵) + `desc`(동작 파라미터).
- 참고 구현: `Pawel-9215/abr-to-krita` (descriptor 파서를 직접 구현했고, Krita 내장 임포터보다
  훨씬 많은 동작 — 크기/각도/간격, 필압·틸트 기반 size/opacity/flow/roundness/rotation/scattering — 을 보존한다).

> **판단: 채택.** Descriptor 파서를 C++로 자체 구현한다. 텍스처만이 아니라 **동작까지** 가져온다.

### 3.3 클립스튜디오 브러시 (.sut) — ★★★★☆ 가능 (핵심 발견)

**`.sut` 파일은 통째로 SQLite 데이터베이스다.** ("sut" = Sub Tool, CSP에서 브러시를 부르는 이름)

| 요소 | 위치 |
|---|---|
| 브러시 이름 | `NodeName` 컬럼 |
| 현재 설정 / 기본값 | `NodeVariantId` / `NodeInitVariantId` 가 가리키는 행 |
| 파라미터 | variant 테이블에 **행 1개 = 브러시 1개, 열 1개 = 파라미터 1개** (수백 개) |
| 브러시 텍스처 PNG | `FileData` 컬럼 안의 **비압축 tar 아카이브** |
| 필압 등 동적 커브 | `*effector` 계열 컬럼의 **빅엔디안 바이너리 blob** |

**주의점:** 컬럼 집합이 고정이 아니다. **CSP 버전마다 다르다.**
→ 파서를 하드코딩하면 안 되고, 컬럼 이름을 런타임에 조회하는 **관대한(lenient) 매핑** 방식이어야 한다.

> **판단: 채택.** SQLite를 읽을 수 있으면 되므로 기술 난이도가 낮다.
> `.abr`보다 오히려 쉽다. 다만 effector blob 리버싱에 시간이 든다.

### 3.4 파일 포맷 (.psd / .ora)

| 라이브러리 | 언어 | 읽기 | 쓰기 | 비고 |
|---|---|---|---|---|
| `MolecularMatters/psd_sdk` | C++ | ◎ | △(제한적) | 그룹·중첩 레이어·스마트오브젝트·마스크, 8/16/32비트 지원. 가장 완성도 높다. |
| `libpsd` (여러 포크) | C | ○ | ✕ | 오래됐다. 디코드 전용. |
| `aseprite/psd` | C++ | ○ | ○ | 읽기/쓰기 둘 다. 규모는 작다. |
| OpenRaster(.ora) | — | — | — | **ZIP + PNG + XML**. 스펙이 공개돼 있고 GIMP/Krita/MyPaint가 모두 지원한다. |

> **판단:** 네이티브 포맷은 **`.ora` 기반**으로 간다(개방형, 구현 쉽고, 타 오픈소스와 즉시 호환).
> `.psd`는 `psd_sdk` 읽기 + 자체 쓰기 구현.

### 3.5 타블렛 입력 — 이게 "느낌"을 좌우한다

Windows에는 펜 입력 API가 **두 개** 있고, 둘은 사실상 배타적이다.

| | Wintab | Windows Ink (WM_POINTER) |
|---|---|---|
| 출신 | 90년대 초, 와콤 등 제조사 표준 | Windows 8, 마이크로소프트 |
| 드라이버 지원 | **항상 켜져 있다. 끌 수 없다.** | 선택 사항(드라이버 설정). 최신 드라이버는 기본 ON |
| 강점 | 와콤 필압 정확, 레이턴시 낮음 | Surface 등 MS 계열, OS 통합 제스처 |
| 함정 | Qt는 wintab32.dll 없으면 필압을 못 받는다 | Qt에서 와콤 필압이 안 잡히는 사례 보고됨 |

> 🔴 **판단 (2026-09-17 변경 — Sigan 연동 조사 반영):**
> **Windows Ink를 고정 기본값으로 한다. WinTab은 고급 설정에 숨기고 경고를 띄운다.**
>
> 처음엔 "둘 다 구현하고 사용자가 고른다"로 썼으나, Sigan(VASE9) 리포를 보고 뒤집었다.
> **WinTab 모드 앱은 펜 HID를 독점해 죽인다** — CSP에서 실측된 현상이다. Mari가 WinTab을 쓰면
> Sigan의 RawInput 기록이 침묵한다. 근거: [03-sigan-integration.md](./03-sigan-integration.md) 3절.
>
> "필압이 안 먹어요"는 여전히 페인트 툴 최다 불만이므로 **진단 화면**은 그대로 만든다.

---

## 4. "COM 방식 창" — 어떻게 해석했나

사용자 요구: *창은 COM 방식*, *Windows COM 기반 플러그인 호스팅*.

이걸 다음과 같이 설계에 반영한다.

```
┌─────────────────────────────┐
│  mari-paint.exe  (본체)      │   ← 가볍다. Qt/C++. COM 서버로 자기를 등록.
│  · 캔버스 / 레이어 / 브러시   │
│  · IMariDocument  (COM)     │   ← 외부 앱이 붙는 지점
│  · IMariLayer     (COM)     │
└──────────┬──────────────────┘
           │ COM (out-of-process, LocalServer32)
   ┌───────┴────────┬──────────────────┐
   │                │                  │
┌──▼───────────┐ ┌──▼────────────┐ ┌───▼──────────┐
│mari-8bf-host │ │ sigan 앱      │ │ 사용자 스크립트 │
│  .exe (32/64)│ │               │ │ (Python 등)   │
│ 포토샵 플러그인 │ │ COM으로 연동   │ │               │
│ 격리 실행      │ │               │ │               │
└──────────────┘ └───────────────┘ └──────────────┘
```

**왜 이 구조인가**

1. **가볍다** — 본체는 그리기만 한다. 호환 레이어는 안 쓰면 아예 로드되지 않는다.
2. **안 죽는다** — 남의 .8bf 플러그인이 크래시해도 별도 프로세스라 본체는 산다.
3. **32/64비트 둘 다** — 32비트 플러그인은 32비트 호스트가, 64비트는 64비트 호스트가 맡는다.
   COM은 프로세스 경계를 넘나드는 게 원래 일이라 이게 공짜로 된다.
4. **다양한 프로그램과 호환** — COM 인터페이스 하나로 C#, Python(pywin32), VBA, Delphi,
   그리고 sigan 앱까지 전부 붙는다. 언어별 바인딩을 따로 안 만들어도 된다.

**대가:** Windows에 강하게 묶인다. macOS/Linux에서는 COM 레이어가 빠진 채 동작한다
(코어는 Qt라 이식 가능). 이건 감수해야 하는 트레이드오프다.

---

## 5. 라이선스 — 지금 결정해야 한다

| 후보 | 장점 | 단점 |
|---|---|---|
| **GPL-3.0** | Krita 코드/플러그인 재사용 가능 | 상용 플러그인 생태계가 안 생긴다 |
| **LGPL-3.0** | Qt LGPL 부분과 궁합이 좋다 | 정적 링크 제약 |
| **Apache-2.0 / MIT** | 누구나 쓴다. 생태계가 가장 크게 자란다 | Krita 코드 절대 못 씀(어차피 안 쓸 계획) |

주의:
- **Qt 오픈소스판은 LGPL-2.1/3 + 일부 모듈은 GPL-3 전용**이다. GPL 전용 모듈(예: Qt Charts 일부)을
  쓰면 앱 전체가 GPL이 된다. → **쓰는 Qt 모듈을 LGPL 범위로 제한**해야 한다.
- libmypaint는 **ISC**라 어떤 선택과도 충돌하지 않는다.

> **권장: Apache-2.0.** 처음부터 Krita 코드를 안 쓸 거라면 GPL을 고를 이유가 없다.
> 특허 조항이 있어서 기업이 붙기에도 안전하다.

---

## 6. 채택 결정 요약

| 영역 | 결정 | 근거 |
|---|---|---|
| UI 프레임워크 | **Qt 6 (LGPL 모듈만)** | COM 연동 + 크로스플랫폼 코어 + 가벼움 |
| 브러시 엔진 | **libmypaint (ISC)** 를 1차 엔진 | 검증됨, 라이선스 자유, 시간 절약 |
| 캔버스 저장 | **타일 기반** 자체 구현 | 큰 캔버스 메모리 문제 해결 (Krita 방식 참고) |
| 네이티브 포맷 | **OpenRaster(.ora) 확장** | 개방형, 타 툴과 즉시 호환 |
| PSD | `psd_sdk` 읽기 + 자체 쓰기 | 가장 완성도 높은 오픈소스 리더 |
| .abr | **자체 Descriptor 파서** | Krita보다 나은 호환성 = 차별점 |
| .sut | **SQLite 직접 읽기** | 형식이 SQLite라 난이도 낮음 |
| .8bf | **별도 프로세스 + COM** | 크래시 격리, 32/64비트 동시 지원 |
| 자동화 API | **COM (IMariDocument 등)** | 사용자 요구 + sigan 앱 연동 통로 |
| 라이선스 | **Apache-2.0 (제안)** | 생태계 확장성 |

---

## 7. 아직 안 정해진 것 (사용자 확인 필요)

1. ~~sigan 앱 연동 규격~~ → **해결됨.** sigan = [VASE9](https://github.com/mirokim/VASE9).
   설계는 [03-sigan-integration.md](./03-sigan-integration.md) 참조.
2. **라이선스 최종 결정** — 위 권장안(Apache-2.0)에 동의하는지.
3. **1차 타깃 OS** — Windows 전용 선출시 후 이식? 아니면 처음부터 3플랫폼?
4. **`.sigan` 서명 키 소유** — Mari는 공급만 하고 서명은 Sigan이 쥔다(제안). 동의하는지.

---

## 출처

- [Krita Manual — Brush Engines](https://docs.krita.org/en/reference_manual/brushes/brush_engines.html)
- [Krita Manual — MyPaint Brush Engine](https://docs.krita.org/en/reference_manual/brushes/brush_engines/mypaint_engine.html)
- [Krita Manual — Brush Tips (.abr 지원 범위)](https://docs.krita.org/en/reference_manual/resource_management/resource_brushtips.html)
- [Krita — License (GPL-3.0)](https://krita.org/en/about/license/)
- [mypaint/libmypaint (ISC)](https://github.com/mypaint/libmypaint)
- [mypaint/mypaint — Licenses.md](https://github.com/mypaint/mypaint/blob/master/Licenses.md)
- [spetric/Photoshop-Plugin-Host (.8bf 호스트 엔진)](https://github.com/spetric/Photoshop-Plugin-Host)
- [draekko/gimp-pspi (PSPI)](https://github.com/draekko/gimp-pspi)
- [GIMP.cc — Photoshop Plugins in GIMP: 호환성 한계](https://gimp.cc/faq/photoshop-plugins-in-gimp.html)
- [Pawel-9215/abr-to-krita (.abr Descriptor 파서)](https://github.com/Pawel-9215/abr-to-krita)
- [Brushfactory — .abr 포맷 설명](https://brushfactory.co/formats/abr)
- [Brushfactory — .sut 포맷 설명 (SQLite 구조)](https://brushfactory.co/formats/sut)
- [MolecularMatters/psd_sdk](https://github.com/MolecularMatters/psd_sdk)
- [aseprite/psd](https://github.com/aseprite/psd)
- [TheNicker/libpsd](https://github.com/TheNicker/libpsd)
- [7P Drawing tablets — WinTab vs Windows Ink](https://docs.thesevenpens.com/drawtab/developers/wintab-vs-windows-ink)
- [devnotes — Wintab and Windows Ink Coexistence](https://docs.sevenpens.com/devnotes/pen-input-on-windows/implementation-notes/wintab-vs-windows-ink-driver-conflict)
- [Wacom Developer Support — Wintab](https://developer-support.wacom.com/hc/en-us/articles/12844524637975-Wintab)
- [Qt Licensing (Qt 6)](https://doc.qt.io/qt-6/licensing.html)
- [Qt Open Source Licensing FAQ](https://www.qt.io/faq/qt-open-source-licensing)
