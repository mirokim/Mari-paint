# Mari Paint

가볍고 빠른 오픈소스 페인팅 툴. **Windows COM**으로 다른 프로그램과 연동하고,
**포토샵 브러시(.abr)** 와 **클립스튜디오 브러시(.sut)** 를 한 자리에서 쓴다.

> 🚧 **현재 단계: 설계.** 아직 코드는 없다.

## 목표

- **가볍다** — 콜드 스타트 1.5초 이내, 빈 캔버스 150MB 이내. 수치는 CI로 강제한다.
- **호환된다** — .abr / .sut 브러시, .psd / .ora 파일, .8bf 포토샵 플러그인.
- **붙는다** — COM(dual/IDispatch) 인터페이스로 C#·Python·VBA·PowerShell 어디서든 제어한다.
- **안 죽는다** — 외부 플러그인은 별도 프로세스에서 격리 실행한다.

## 기술 선택

| | |
|---|---|
| 언어 / UI | C++20 / Qt 6 (LGPL 모듈 한정) |
| 브러시 엔진 | [libmypaint](https://github.com/mypaint/libmypaint) (ISC) |
| 캔버스 | 64×64 타일, Copy-on-Write |
| 네이티브 포맷 | OpenRaster (.ora) |
| 자동화 | Windows COM (out-of-process) |
| 라이선스 | Apache-2.0 *(제안, 미확정)* |

## 문서

- [01 — 오픈소스 리서치](docs/01-research.md) — 무엇을 가져오고 무엇을 피할지, 근거와 출처
- [02 — 아키텍처 설계](docs/02-architecture.md) — 모듈 구성, COM 레이어, 로드맵, 위험 요소

## 로드맵 요약

`M0` 타일 캔버스 → `M1` 그리기 → `M2` 브러시 호환 → `M3` PSD → `M4` COM → `M5` 8bf 호스트 → `M6` 성능

## 정해야 할 것

1. **sigan 앱 연동 규격** — Mari를 조종하는 쪽인지, 결과를 받는 쪽인지, 어떤 단위로 주고받는지
2. **라이선스 확정** — Apache-2.0 제안
3. **1차 타깃 OS** — Windows 선출시 후 이식 vs 처음부터 크로스플랫폼
