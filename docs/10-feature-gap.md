# 기능 격차 분석 — Krita · Clip Studio Paint 대비 (2026-09-19)

> 에이전트가 Krita 공식 문서와 CSP 매뉴얼을 조사해 만든 표. 코어(`core`·`agent` 38 연산)와 UI 를 따로 본다.
> 범례: ● 있음 · ◐ 부분(코어만/제한적) · ○ 없음. 이 문서의 "다음 15" 가 로드맵 후보다.

## 0. 핵심 관찰

1. **격차의 절반은 "UI 만"이다.** 선택·그룹·마스크·fill/gradient·브러시 임포트·스냅샷은 코어와 에이전트 API 에
   이미 있는데 화가가 쓸 수 없다.
2. **합성기를 건드려야 하는 것은 딱 하나 — 클리핑 마스크.** 채색 워크플로 1순위라 합성기 변경은 이 한 건에 집중.
3. **스트로크 파이프라인을 건드리는 것은 스태빌라이저 강화와 대칭 그리기 뿐.** 둘 다 "획의 유일한 입구" 원칙과
   지연 예산 측정을 동반해야 한다.
4. Mari 만의 강점(에이전트 API/MCP · 출처 기록 · O(1) 스냅샷/분기)은 두 앱 어디에도 없다. 스냅샷 분기를 UI 에
   노출하면(히스토리 도커의 상위 호환) 차별점이 된다.

## 1. 브러시 엔진 · 편집

| 기능 | Mari | Krita | CSP |
|---|---|---|---|
| 팁 모양(원/비트맵/각도/종횡비) | ◐ IR 완비, 편집 UI 없음 | 브러시 팁 탭 | 브러시 끝(이미지 소재) |
| 경도 · 간격 · 흩뿌림 | ◐ 코어 지원, UI 없음 | 있음 | 있음 |
| 필압→크기/불투명/유량 곡선 | ◐ `DynamicLink` 곡선, 편집 UI 없음 | 센서별 곡선 편집기 | 필압 설정 곡선 |
| 텍스처/종이 질감 | ● 곱하기·빼기·어둡게·스크린·오버레이, 캔버스/스탬프 고정 (2026-09-19) | Texture 탭 | 질감 소재 |
| 스태빌라이저 | ◐ EMA 4단 | 기본/가중/스태빌라이저(지연·끝맺음) | 손떨림 보정 0–100 + 후보정 |
| 섞기/희석/번짐(수채) | ○ | Color Smudge 엔진 | 물감량/농도/색 늘이기 |
| 지우개 | ● 도구 + 펜 뒤집기 | ● | ● |
| 프리셋 저장/태그/즐겨찾기 | ◐ .mbp 저장·브러시 패널·편집기 (2026-09-19; 태그·즐겨찾기 ○) | 태그·도커·검색 | 서브툴 그룹·즐겨찾기 |
| 드래그로 크기 | ● Shift+드래그 (2026-09-19) | Shift+드래그 | Ctrl+Alt+드래그 |
| 전역 압력 곡선 · 테스터 | ● (2026-09-19) | ● | ● |

## 2. 레이어

| 기능 | Mari | Krita | CSP |
|---|---|---|---|
| 그룹/폴더 | ● 트리 UI·드래그·그룹 묶기/풀기 (2026-09-19) | ● | ● |
| 클리핑(아래 레이어에 클립) | ○ **합성기 미지원** | Inherit Alpha | 클리핑 플래그 |
| 레이어 마스크 | ● 선택에서 만들기·보이기/가리기·적용·삭제 (2026-09-19; 마스크에 직접 그리기는 ○) | ● | ● |
| 병합/평탄화 | ● 아래와 병합 · 이미지 평탄화 (2026-09-19, 실행취소 가능) | 병합·평탄화·그룹 병합 | 결합·표시 레이어 결합 |
| 투명 픽셀 잠금 | ● | ● | ● |
| 레이어 효과 · 참조/초안 · 채우기 레이어 · 검색 | ○ | ● | ● |
| 블렌드 | ● 19종 | 60+ | 27종 |

## 3. 선택 · 변형

| 기능 | Mari | Krita | CSP |
|---|---|---|---|
| 사각/타원/올가미/다각형/마술봉 | ◐ 코어+에이전트 전부, **UI 도구 0개** | ● | ● |
| add/subtract/invert/feather | ◐ 코어 완비 | ● | ● |
| 선택 표시(점선) | ○ | ● | ● |
| 이동/확대/회전/자유 변형 | ○ dx/dy 만 | 자유·원근·워프·케이지·리퀴파이 | 자유·메시·리퀴파이 |
| 자르기/캔버스 크기·회전·뒤집기 | ○ | ● | ● |

## 4. 색 · 채우기

| 기능 | Mari | Krita | CSP |
|---|---|---|---|
| 페인트통(틈 닫기·다중 참조) | ◐ 영역 fill 만, 플러드필 없음 | ● | ● |
| 그라데이션 도구 | ◐ 에이전트 선형만 | ● | ● |
| 색 조정(레벨/커브/HSL) | ○ | ● | ● |
| 팔레트/스와치 | ◐ 최근색 16칸 | 팔레트 도커 | 컬러 세트 |

## 5. 캔버스 · 뷰

| 기능 | Mari | Krita | CSP |
|---|---|---|---|
| 회전/뒤집기/줌 · 내비게이터 · 팝업 팔레트 | ● (내비·팝업 2026-09-19) | ● | ● |
| 눈금자/그리드/가이드 · 원근 자 · 대칭 그리기 · 다중 뷰 · 픽셀 격자 | ○ | ● | ● |

## 6. 선화 · 벡터 · 텍스트 — 직선/곡선/도형 ○ · 벡터 레이어 ○(미룸) · 텍스트 ○(미룸)

## 7. 파일 · 워크플로

| 기능 | Mari | Krita | CSP |
|---|---|---|---|
| 포맷 | ◐ .ora · png | .kra/.ora/.psd/… | .clip/.psd/… |
| PSD | ○ 계획만 | ● | ● |
| 자동 저장/백업/복구 | ○ | ● | ● |
| 실행취소 히스토리 패널 | ◐ 스택 있음, 도커 없음 | ● | ● |
| 스크립팅 | ● **에이전트 API/MCP 38 연산** (대체) | Python | 오토 액션 |

## 8. 입력 · 태블릿 — 필압 곡선 ● · 기울기/회전 ◐(파싱됨, 브러시 곡선 UI 없음) · 단축키 편집 ● (포토샵 기본값, 2026-09-19) · 팝업 팔레트 ●

## 9. 참조 · 보조 — 참조 이미지 ○(미룸) · 3D 포즈 ○ · 색 히스토리 ● · 색 혼합기 ○

## 10. 애니메이션 — ○ 미룸

## 화가가 가장 먼저 아쉬워할 15가지 (우선순위)

노력 S(1~2일) / M(1주) / L(2주+). 종류: UI(코어 이미 있음) / 코어 / 코어+UI.

| # | 기능 | 노력 | 함의 |
|---|---|---|---|
| 1 | **클리핑 마스크** | M · 코어+UI | `Layer` 에 `clipToBelow` + `.ora` 확장 속성 + 합성기가 아래 레이어 알파로 마스킹. CSP 방식(플래그)이 단순 |
| 2 | **선택 도구 4종 UI + 점선** | M · UI | `SelectionMask` 완비. 캔버스 오버레이 + Shift/Alt 수정키 |
| 3 | **페인트통(플러드필·틈 닫기·다중 참조)** | M · 코어+UI | 새 코어 연산 `floodFill`. 에이전트 `fill` 에 `mode:"flood"` |
| 4 | **자유 변형(이동/확대/회전)** | L · 코어+UI | 리샘플 + 변형 중 임시 레이어 미리보기 + 커밋 |
| 5 | ~~**그룹 레이어 UI + 평탄화**~~ ✅ 2026-09-19 | S · UI | 트리 뷰로 위임 교체 |
| 6 | **브러시 편집기 + 사용자 프리셋 저장** | M · UI | IR 완비. 곡선 위젯(태블릿 대화상자 것 재사용) + JSON 프리셋 폴더 |
| 7 | **스태빌라이저 강화**(지연 원·끝맺음·후보정) | M · 코어(stroke) | 지연 예산 측정과 함께 |
| 8 | **자동 저장/백업/복구** | S · 앱 | 타이머 + LOCALAPPDATA .ora, 시작 시 복구. 저널 재생도 후보 |
| 9 | **색 조정(HSL·레벨·커브) + 뒤집기/캔버스 크기** | M · 코어+UI | 타일 단위 픽셀 필터 파이프라인 |
| 10 | ~~팝업 팔레트~~ ✅ | | 2026-09-19 |
| 11 | ~~Shift+드래그 크기 + HUD~~ ✅ | | 2026-09-19 |
| 12 | **직선/곡선/도형 도구** | S~M · UI(+stroke) | 점 배열 생성 → 기존 파이프라인 |
| 13 | ~~**레이어 마스크 UI**~~ ✅ 2026-09-19 (마스크 직접 그리기 제외) | M · UI+소량 코어 | 마스크 편집 모드(Gray8 에 그리기) + .ora 저장 확인 |
| 14 | **대칭 그리기 + 그리드/가이드** | M · 코어(stroke)+UI | "획 1개 = 기록 1건" 결정 필요 |
| 15 | **PSD 읽기/쓰기** | L · io | #1 #13 #5 가 먼저 있어야 손실 없이 매핑 |

그다음: ~~실행취소 히스토리 도커(S)~~ ✅ · ~~팔레트 도커(S)~~ ✅ · ~~단축키 편집기(M)~~ ✅ · ~~참조 이미지(S)~~ ✅ · ~~그라데이션 도구 UI(S)~~ ✅ ·
~~브러시 임포트 메뉴~~ ✅ · ~~텍스처 브러시~~ ✅ (2026-09-19: 듀얼·색 변화·젖은 가장자리·개수·노이즈까지).

## 명시적으로 미루는 것

애니메이션 · 벡터 레이어/텍스트(직선·곡선 도구가 80% 대신) · 3D 소재 · 메시/리퀴파이 · 비파괴 채우기/필터 레이어 ·
레이어 스타일 · 매크로(에이전트 `batch` 가 대체) · 터치 제스처 · 다중 뷰 · 라이트 테마 · 60+ 블렌드.

## 출처

Krita: https://docs.krita.org/en/reference_manual/tools/freehand_brush.html · brush_settings/texture.html ·
brush_engines/color_smudge_engine.html · tutorials/clipping_masks_and_alpha_inheritance.html · dockers/layers.html ·
tools/transform.html · tools/fill.html · tools/assistant.html · user_manual/mirror_tools.html · popup-palette.html ·
preferences/tablet_settings.html · dockers/undo_history.html · tools/reference_images_tool.html
CSP: https://help.clip-studio.com/en-us/manual_en/240_brushes/Customizing_brush_tools.htm · 180_layers/Layer_masks.htm ·
180_layers/Layer_properties.htm · 360_transform/Liquify_tool.htm · 330_selection/Selection_area_tool.htm ·
420_fill/Fill_Tool.htm · 510_ruler/Perspective_Rulers.htm · 690_interface/Quick_Access_Palette.htm ·
720_preferences/Shortcut_Settings.htm · 210_file/Save_file.htm

## 11. 2026-09-19 브러시 라운드 — 무엇이 되고 무엇이 남았나

**엔진(native)**: 텍스처 · 듀얼 브러시 · 색 변화(전경↔배경·H·S·B·순도, 스탬프/획) · 젖은 가장자리 · Count · 노이즈.
남은 것: 에어브러시 시간 반복(멈춰 있어도 쌓임) · 색 변화 제어원(필압→전경/배경) · 개수 지터 · 듀얼 팁의 독립 간격.

**.abr**: dualBrush · clVr · Wtdg · Nose · Rpt · Cnt 번역. v1/2(팁 전용)는 여전히 ✗.
**.sut**: 팁·텍스처·기본 파라미터. effector 커브 리버싱은 그대로 불확실(리포트에 남긴다).

**라이브러리**: `.mbp`(JSON + PNG base64) · `%LOCALAPPDATA%/Mari/Mari Paint/brushes` 를 GUI 와 `--serve`/`--mcp` 가 같이 읽는다.
GUI: 브러시 도크(엔진 미리보기 격자·검색·우클릭 편집/복제/삭제/가져오기) · 브러시 편집기(팁/동작/동적/텍스처/듀얼/색 변화, 실시간 미리보기) ·
가져오기 리포트(버림/근사/정보). 에이전트: `brush.import(persist)` · `brush.save` · `brush.remove` · `brush.export`.

**에이전트 도구 동등성**(사람이 GUI 로 하는 것 = 헤드리스로 같은 코어 경로): `bucket` · `select mode=wand` · `layer.flatten` ·
`layer.mask` · `layer.group/ungroup` · 획 점에 `tx/ty/t` · `background`. 자유 변형(`transform` 확대·회전 포함)·색 보정(`adjust`)·캔버스 연산(`canvas`)도 2026-09-19 추가 — GUI 와 같은 image_ops.

### 11.1 실물 파일 검증 (2026-09-19)

**.abr** — K. M. Alexander 의 CC0 세트 두 개(Myer 148개 · Mercator 238개, Photoshop 2025 저장, v6.2)로 검증.
합성 픽스처만 믿고 있던 두 가지가 틀려 있었다:
1. `samp` 머리말 레이아웃: 실제는 `u32 길이 · 37바이트 키("$"+UUID) · (subversion 2) 264바이트 · bounds · depth · compression`.
   옛 코드(u16 spacing + 유니코드 이름)는 실물에서 "머리말이 잘렸다"로 끝나 팁이 하나도 안 붙었다.
2. 팁 극성: 실물은 **255 = 잉크**. 뒤집어 읽어서 스탬프가 검은 덩어리였다.
고친 뒤: 386/386 비트맵 팁, Dropped 0(`use*` 토글은 소비해서 꺼진 기능을 IR 에서 뺀다).
`tests/fixtures/brushes/myer-settlement-cc0.abr` 을 리포에 넣고 `abr_real_world_cc0_file_...` 테스트가 지킨다(MARI_FIXTURE_DIR).

**.sut** — ED_of_all 무료 팩 5개(bubble · flower · spray paint · Winged Jewel · zombie, 재배포 금지라 리포에 넣지 않음)로 검증.
확인한 실물 스키마: `Node.NodeVariantID → Variant.VariantID`(_PW_ID 가 아니다) · `BrushSize/BrushInterval(%)/BrushHardness/
BrushThickness(100=원)/BrushRotation(90=똑바로)/Opacity/BrushFlow/CompositeMode` · `MaterialFile.FileData` = tar(catalog.zip,
`data/material_0.layer`(독점 C2F — 못 읽음), `thumbnail/thumbnail.png`) → **팁은 썸네일(≤300px)** 에서 · effector blob =
`11×u32 머리말([3]=최솟값%, [6]=랜덤%) + (12,n,16) 블록(BE double 점)` — 첫 블록을 필압 커브로 반영, 두 번째(속도/기울기 추정)는 리포트만.
스프레이(`BrushUseSpray/SpraySize/SprayDensity`)→흩뿌림·개수, `UseDualBrush/Dual*`→듀얼, `BrushUseWaterEdge`→젖은 가장자리,
`BrushHue/Saturation/Value/SubColor`→색 변화. 남은 불확실: BrushRotationEffector 열거값(실물 5개 전부 3), 속도·기울기 커브.

### 11.2 이미지 연산 (2026-09-19)

`app/image_ops`: 색 보정 8종(선택 안, 알파 보존) · 자유 변형(내용 경계 기준 피벗, 쌍선형, 선택 안만 잘라 붙이기) ·
캔버스 뒤집기/회전/크기/자르기/이미지 크기(모든 레이어, 실행취소 하나). GUI: 이미지 메뉴 + 보정 대화상자(실시간 미리보기 =
undo→재적용이라 확인 뒤 실행취소 항목이 정확히 하나) + 곡선 편집기 + Ctrl+T 변형 모드(핸들·회전·화살표·옵션 바 숫자 입력).
에이전트: `adjust` · `transform`(확대·회전 실제 지원) · `canvas`.

### 11.3 자동 저장 · 백업 · 복구 (2026-09-19)

`Document::writeCopy`(경로·저장됨 표시를 안 건드리는 .ora 복사) 위에: 기본 3분(파일 › 자동 저장·백업 설정, 0=끔)마다
바뀐 게 있을 때만 `%LOCALAPPDATA%/Mari/Mari Paint/autosave/<id>.ora` + `.json`(원본 경로·시각·크기). 정상 저장/종료 때 지운다.
저장 때 원본을 `.bak` 로(끌 수 있음). 시작할 때 자동 저장본이 있으면 복구 대화상자(복구/삭제/나중에) — 복구본은 "저장 위치 미정"
으로 열리고 Ctrl+S 가 반드시 '다른 이름으로' 를 띄운다(자동 저장 폴더에 덮어쓰지 않게). 실측: 그리고 1분 뒤 자동 저장 → 강제 종료 →
재시작 → 복구 대화상자 → 획이 돌아옴.

### 11.4 손떨림 보정 개선 (2026-09-19)

`Smoother` 에 **데드존**("끈에 매단 펜": 원본이 반지름 밖으로 나가야만 끌려온다 — CSP 후보정/Krita stabilizer 방식)과
`StrokePipeline::end()` 의 **끝점 보정**(다듬은 점이 못 미친 거리를 펜을 뗀 자리까지 직선으로 이어 그림, 뗄 때 필압)을 넣었다.
Off 는 여전히 비트 단위 항등. GUI 옵션 바 "데드존"(화면 px, 줌으로 나눠 캔버스 px 로) · 에이전트 `stroke.deadZone/endCorrection`.
테스트: 떨림 꼭짓점이 비고, 끝점 보정 켜면 뗀 자리에 잉크가 있고 끄면 없고, 데드존 6px 에서 ±2px 떨림은 0 이동 · 30px 이동은 24px.

### 11.5 도형 · 그라데이션 · 대칭 · 격자 · 도크 (2026-09-19)

직선/사각형/타원은 **현재 붓으로 한 획**을 합성해 그린다(`strokePolyline` → LiveStroke, 필압 1, 보정 끔) — 그래서 기록·실행취소·
출처가 붓질과 같다. 그라데이션은 `app::fillGradient`(선형/원형, 전경→배경/투명, 선택 안, 소스 오버). 좌우/상하 대칭은 축마다
LiveStroke 를 하나 더 굴린다(같은 설정, 다른 시드 — 각각 기록·실행취소). 격자는 오버레이(간격 설정). 히스토리·팔레트·참조 도크.
남은 것: PSD(다음 라운드) · CSP 텍스처(샘플 없음) · 에어브러시 시간 반복 · 실제 펜 검증(사용자).
