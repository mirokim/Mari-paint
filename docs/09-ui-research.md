# 페인팅 앱 GUI 관례 조사와 Mari UI 결정 (2026-09-18)

> Krita · Clip Studio Paint · Paint Tool SAI · MediBang · Photoshop · Procreate 의 관례를 조사해
> Mari 의 최소 UI 를 정했다. 조사는 에이전트가 했고, 결정과 구현 상태는 여기 적는다.
> 제약(docs/08 3.4): 최소 UI · 콜드 스타트 < 1.5s · 설치 < 80MB · Qt 는 LGPL 모듈만.

## 1. 결정한 레이아웃 (구현됨)

```
┌ 메뉴: 파일 편집 레이어 보기 ─────────────────────────────────────────────┐
│ 옵션 툴바: [붓 프리셋 ▾] 크기 [슬라이더(지수)][24.0 px] 불투명 [슬라이더] 보정[▾] ↶ ↷ │
├──┬───────────────────────────────────────────────────────┬─────────────────┤
│붓│                                                         │ 색 (도크)        │
│지│                                                         │  휴 링 + SV 사각형  │
│스│              캔버스 — 호버 시 붓 윤곽 원                  │  [전경][배경] #hex  │
│손│                                                         │  최근 색 12칸       │
│  │                                                         ├─────────────────┤
│  │                                                         │ 레이어 (도크)     │
│  │                                                         │ [블렌드 ▾] 🔒 α    │
│  │                                                         │ 불투명 ▬▬▬▬       │
│  │                                                         │ ☑ ▣ 레이어 2       │
│  │                                                         │ ☑ ▣ 레이어 1       │
│  │                                                         │ [+][▲][▼][−]       │
├──┴───────────────────────────────────────────────────────┴─────────────────┤
│ 붓 · 6.0 px · 90% · 레이어 1        81% · 0°   (진단 켜면) 입력·저널·지연 통계   │
└──────────────────────────────────────────────────────────────────────────────┘
```

공통분모: **도구(좌) · 캔버스(중) · 색+레이어(우)**, 도킹. `Tab` 으로 전부 접힌다.
다크 테마는 Fusion + 팔레트(Qt 6.5+ 권장) — 페인팅 앱이 중간 회색 UI 를 쓰는 이유는 색 지각 중립성이다.

## 2. 단축키 표 (구현됨) — 기본값은 **포토샵**과 같다 (2026-09-19 정렬)

편집 › 단축키 설정… (`Ctrl+Alt+Shift+K`) 에서 바꾼다. 변경은 QSettings `shortcuts/<메뉴|항목>` 에 저장하고,
같은 키를 다른 동작에 주면 원래 동작의 키는 비운다(포토샵 정책). "전부 기본값" 으로 되돌린다.

| 키 | 동작 | 관례 출처 |
|---|---|---|
| `B` / `E` / `I` / `H` / `G` | 붓 / 지우개 / 스포이드 / 손 / 페인트통 | PS |
| `M` / `Shift+M` / `L` / `W` | 사각 선택 / 타원 선택 / 올가미 / 마술봉 | PS (`Shift+M` 은 PS 의 선택 도구 순환을 단순화) |
| `[` / `]` | 붓 크기 −/+ (구간 스텝: <10:1 · <50:5 · <100:10 · <300:25 · 이후 50) | PS·CSP |
| `Shift+[` / `Shift+]` | 불투명도 −/+ 10% | 변형 |
| `Space+드래그` · 가운데 버튼 | 팬 | 공통 |
| `Ctrl+Space+드래그` · 휠 | 줌 (누른 점 / 커서 기준) | PS |
| `Shift+Space+드래그` | 회전 (창 중심 기준) | Krita (PS 는 R 도구) |
| `Ctrl+Alt+[` / `Ctrl+Alt+]` · `5` | 회전 ∓15° · 회전 초기화 | 변형 (`Ctrl+[ ]` 는 PS 에서 레이어 이동) |
| `Alt+M` | 미러 보기 | 변형 (PS 기본 없음) |
| `Ctrl+0` / `Ctrl+1` / `Ctrl+=` / `Ctrl+-` | 창에 맞춤 / 100% / 확대 / 축소 | PS |
| `X` / `D` | 전경↔배경 / 검정·흰색 | PS |
| `Alt`(누른 동안) | 임시 스포이드 | PS |
| `Ctrl+Z` / `Ctrl+Shift+Z`·`Ctrl+Y` | 실행취소 / 다시실행 | PS |
| `Ctrl+A` / `Ctrl+D` / `Ctrl+Shift+I` | 전체 선택 / 해제 / 반전 | PS |
| `Alt+Backspace` / `Ctrl+Backspace` / `Delete` | 전경색 채우기 / 배경색 채우기 / 선택 영역 지우기 | PS |
| `Ctrl+Shift+N`(·`Insert`) / `Ctrl+J` | 새 레이어 / 레이어 복제 | PS |
| `Ctrl+G` / `Ctrl+Shift+G` / `Ctrl+Alt+G` | 그룹으로 묶기 / 그룹 풀기 / 클리핑 마스크 토글 | PS |
| `Ctrl+E` / `Ctrl+Shift+E` | 아래와 병합 / 이미지 평탄화 | PS |
| `Ctrl+]` / `Ctrl+[` · `Alt+]` / `Alt+[`(·`PgUp`/`PgDn`) | 레이어 위/아래로 이동 · 위/아래 레이어 선택 | PS |
| `Tab` · `F`(·`Ctrl+Shift+F`) | 패널 전부 숨김/표시 · 캔버스 전용 모드 | PS |
| `F5` | 현재 브러시 편집 | PS(브러시 설정 패널) |
| 펜 뒤집기 | 지우개(도구보다 우선) | Windows Ink |

🔴 이전 구현은 `[` `]` 를 회전에 묶어 두었다 — 모든 주요 앱에서 붓 크기다. 바꿨다.

## 3. 구현 메모 (ui/)

- **붓 크기 슬라이더**: 0..1000 → 1..500px 지수 매핑. 작은 값에 정밀도가 몰린다(Krita `KisSliderSpinBox` 방식).
- **호버 윤곽**: `CanvasWidget` 이 마우스 트래킹으로 위치를 받아 지름×줌 원을 흰/검 두 겹으로 그린다.
  이전/현재 자리의 작은 rect 두 개만 `update()` — 신호 없음(docs/08 7절 6항).
- **색 패널**: `ColorWheel`(휴 링 QConicalGradient + SV 사각형 QImage, 휴 바뀔 때만 재생성) +
  전경/배경 + HEX + 최근 색 12칸. QColorDialog 는 쓰지 않는다 — 모달은 그리기 흐름을 끊는다.
- **레이어 패널**: 블렌드 콤보(`BlendMode`, Erase 제외) · 불투명도 · 잠금/알파 잠금 · 썸네일(캔버스 비율,
  획 끝 200ms 뒤 갱신 — 같은 프레임에 두면 지연에 +40ms 가 얹힌다) · 드래그 재배열(`InternalMove` →
  `LayerTree::move`) · 더블클릭 이름 편집 · 가시성 체크.
- **뷰 캐시**: 위젯 크기의 변환 완료 이미지를 들고 paintEvent 는 블릿만 한다. 레이아웃·포커스 변화로
  캔버스 전체가 다시 그려질 때 1920×1080 을 축소 샘플링하면 40ms — 캐시면 memcpy 다.
- **Space 모드**: 눌린 동안만. 자동 반복 무시. 모드 중 WM_POINTERDOWN 은 `nativeEventFilter` 가 Qt 로
  넘겨 마우스 이벤트로 팬/줌/회전을 처리한다 — 획이 아니다.
- **스포이드**: 합성 결과(`backing_`, premultiplied)에서 읽어 straight 로 되돌린다. 투명은 흰색.

## 4. 측정 (마우스 자동 입력, 150% DPI)

- 획 중간 프레임: 펜→화면 **평균 ~2ms**.
- 획당 down/up 프레임 2개가 **~48ms** 로 찍힌다. 페인트 자체는 1ms 미만이고 **입력이 도착할 때 이미
  48ms 묵어 있다**(`slow frame: wait-before-paint 48 ms, composite 0.2, viewcache 0.4`). 마우스→포인터
  변환(EnableMouseInPointer) 단계에서 OS 가 버튼 전이를 잡는 것으로 보인다. 실제 펜(PT_PEN)은 이
  경로를 타지 않는다 — **펜으로 재확인할 것**(docs/08 4절).

## 4.5 2차 검토 (2026-09-19) — 시각 계층

1차 조사는 *동작 관례*만 다뤘고 아이콘·크기·간격은 항목 자체가 없었다. 독립 검토자가 감사한 뒤 시각 스펙을
정했고 전부 적용했다:

- **아이콘: Tabler Icons (MIT)** 31개, `ui/icons/*.svg` + `ui/icons.qrc`, 라이선스 사본 `ui/icons/LICENSE.tabler`.
  선택 이유: 커버리지 전부 · 24px 획 2px 가 16px 로 줄여도 다크 배경에서 또렷 · 파일당 ≈0.5 KB.
- **`currentColor` 는 QtSvg 가 모른다.** 파일에 #dcdcdc 를 굽고 `ui/icons.cpp` 의 `themedIcon()` 이
  상태(보통 #dcdcdc · 호버/체크 흰색 · 비활성 #6e6e6e)별로 치환해 `QSvgRenderer` 로 직접 굽는다.
  qsvgicon 플러그인에 기대지 않는다(배포 누락 위험). DPR 은 화면 최대 배율(≥2)로 한 번.
- 도구상자 32×32 · 아이콘 22 · 체크 = 파란 배경(#3d7bd9). 옵션 툴바 아이콘 20, 실행취소/다시실행·줌·회전·
  미러·패널 전부 아이콘. 슬라이더 160/120 + 스핀박스. 라벨 공백 패딩 제거, 구분선.
- 레이어 행: `LayerRowDelegate` — [눈 16][썸네일 48×36][이름 elide][🔒][α] 행 44px, 눈 클릭 = 가시성 토글
  (선택은 안 바뀜), 이름 편집기는 이름 자리에만. 하단 버튼 아이콘 16: 추가·복제·(병합: 코어에 없어 비활성) ┃
  위·아래 ┃ 삭제(오른쪽 끝). 복제는 `duplicateLayer()`(O(1) COW).
- 색 패널: 전경/배경 겹친 48×48(클릭 = 교환) + 교환 아이콘 + HEX, 최근 색 8열×2줄 고정(도크가 들썩이지 않게),
  휴 링 220 가운데 정렬, 도크 폭 280.
- 스타일시트(`gui.cpp`): 툴바/도크 제목/슬라이더/상태줄. 이모지(🔴⚠️🔒α) 전부 제거.
- 상태줄: 도구 아이콘 14 + 텍스트, 진단은 오른쪽에 elide + 툴팁.

## 5. (B) — 2026-09-19 구현됨

우클릭/배럴 팝업 팔레트(`ui/popup_palette`) · `Shift+드래그` 붓 크기 + HUD · 태블릿 대화상자(전역 압력 곡선 5점 +
테스터, QSettings 저장, `ui/tablet_dialog`) · 내비게이터 도크(`ui/navigator`) · 아래와 병합(core `mergeDown` +
`app::mergeLayerDown` 실행취소 명령, Ctrl+E) · 레이어 삭제 실행취소 · 레이어 컨텍스트 메뉴 · 캔버스 전용 모드
(Ctrl+Shift+F) · 창/도크 배치 기억(QSettings). 남은 것: 브러시 프리셋 아이콘 격자.

## 5.5 그룹 트리 · 마스크 · 단축키 편집기 · 앱 아이콘 — 2026-09-19 구현됨

레이어 패널을 QTreeWidget 으로(그룹 접기·드래그로 그룹 안팎 이동, 래스터 위에 드롭은 거부) · 그룹 행은 폴더 아이콘 ·
마스크 있는 행에 마스크 배지 · 레이어 › 레이어 마스크(전부 보이기 / 선택에서 / 선택을 보이기·가리기 / 적용 / 삭제, 전부 실행취소) ·
이미지 평탄화(실행취소 한 번에 트리 복원, `tests/app/test_layer_commands.cpp`) · 단축키 편집기(2절) · 앱 아이콘(`ui/icons/app/`, exe 리소스 + 창 아이콘, `C:\Dev\tmp\make_icon.py` 로 생성).
다음 로드맵은 docs/10-feature-gap.md.

## 6. 명시적으로 미룸 (C)

도구 옵션 도커 · 히스토리 · 참조 이미지 · 팔레트/그라디언트 라이브러리 · 틸트 상시 표시 · 브러시 편집기 ·
다중 문서 탭 · 워크스페이스 저장 · 라이트 테마 · 터치 제스처(핀치) — 터치는 무시하고 팜 리젝션은 Windows
Ink 에 맡긴다 · 애니메이션 · 벡터/텍스트 · 필터 대화상자(8bf 는 M5).

## 출처

- Krita 도커: https://docs.krita.org/en/reference_manual/dockers.html · 내비게이션: https://docs.krita.org/en/user_manual/getting_started/navigation.html · 레이어: https://docs.krita.org/en/reference_manual/dockers/layers.html · 팝업 팔레트: https://docs.krita.org/en/reference_manual/popup-palette.html · 태블릿: https://docs.krita.org/en/reference_manual/preferences/tablet_settings.html
- 붓 크기 스텝 논의: https://krita-artists.org/t/increase-decrease-brush-size-exponentially/151601
- CSP 도구 단축키: https://help.clip-studio.com/en-us/manual_en/780_shortcuts/Tool_Shortcuts.htm
- SAI 2 레이아웃: https://design.tutsplus.com/tutorials/a-beginners-guide-to-paint-tool-sai--cms-25089
- MediBang: https://medibangpaint.com/en/tutorial/pc/other-tools/
- Photoshop 단축키: https://helpx.adobe.com/content/dam/help/en/photoshop/using/default-keyboard-shortcuts/photoshop-keyboard-shortcuts.pdf
- Procreate 제스처: https://help.procreate.com/procreate/handbook/interface-gestures/gestures
- Windows Ink 펜 플래그: https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-pointer_pen_info
- Qt 6.5 다크 모드·Fusion: https://www.qt.io/blog/dark-mode-on-windows-11-with-qt-6.5 · High-DPI: https://doc.qt.io/qt-6/highdpi.html
