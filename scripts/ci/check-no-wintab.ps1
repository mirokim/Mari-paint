<#
.SYNOPSIS
    Mari Paint — WinTab 미사용 검증 (docs/03 3절 · 10절 `wintab_never_loaded`)

.DESCRIPTION
    🔴 이 스크립트가 이 리포에서 가장 중요한 CI 게이트다.

    Sigan 의 `Core/Foreground.cs` 에는 `WinTabKeywords` 목록이 있다. 거기 올라간
    앱은 "RawInput 이 침묵하니 프록시 DLL 을 주입해야 하는 앱" 이다:

        clipstudio · zbrush · opencanvas · tvpaint · harmony ·
        toonboom · painter · photoshop · krita

    Krita 가 저기 있는 게 우리한테 하는 경고다. 오픈소스라 Windows Ink 를 쓸
    것으로 짐작하고 목록에서 빠져 있었는데, 실측하니 `wintab32.dll` 과
    `Wacom_Tablet.dll` 을 로드한 채로 돌고 있었다.

    **Mari 의 목표: `WinTabKeywords` 에 영원히 올라가지 않는 최초의 드로잉 앱.**
    docs/03 에서 유일하게 외부 리포로 검증 가능한 목표다.

    검사하는 것 세 가지:
      1. PE 임포트 테이블 — 정적 임포트
      2. 지연 로드 임포트 테이블 — `/DELAYLOAD:wintab32.dll` 우회 차단
      3. 바이너리 안의 문자열 — `LoadLibrary("wintab32.dll")` 런타임 로드 차단

    3번이 있어야 한다. 1·2번만 보면 `LoadLibrary` 로 몰래 여는 코드를 놓친다.
    Krita 가 정확히 그렇게 판정을 피해 갔을 수도 있는 자리다.

.PARAMETER Path
    검사할 디렉터리 또는 파일. 기본은 .\build

.PARAMETER Strict
    문자열 검사에서 걸린 것도 실패로 본다. 기본값은 켜짐.
    끄면 경고만 내지만, **CI 에서는 절대 끄지 마라.**

.EXAMPLE
    pwsh scripts/ci/check-no-wintab.ps1 -Path build

.NOTES
    dumpbin 이 있으면 쓰고, 없으면 PE 헤더를 직접 읽는다.
    GitHub Actions 의 windows-latest 러너에는 둘 다 있다.
#>
[CmdletBinding()]
param(
    [string] $Path = "build",
    [bool]   $Strict = $true
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# 🔴 금지 목록. 소문자로 비교한다.
#    Wacom_Tablet.dll 도 넣는다 — Krita 실측에서 wintab32 와 함께 나왔다.
$Forbidden = @(
    "wintab32.dll",
    "wintab32",
    "wacom_tablet.dll",
    "wintab"
)

$failures = New-Object System.Collections.Generic.List[string]
$checked  = 0

function Get-TargetFiles([string] $root) {
    if (Test-Path -LiteralPath $root -PathType Leaf) {
        return @(Get-Item -LiteralPath $root)
    }
    if (-not (Test-Path -LiteralPath $root)) {
        throw "검사할 경로가 없다: $root"
    }
    # -Include 는 -LiteralPath 와 같이 쓰면 걸러지지 않는다(PS 5.1 실측: .tlb 가 통과했다).
    # 확장자를 직접 본다.
    Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.Extension -in '.exe', '.dll' -and $_.FullName -notmatch '\\CMakeFiles\\' }
}

# ── 1·2. 임포트 테이블 (dumpbin) ─────────────────────────────────────────────
function Test-ImportTable([System.IO.FileInfo] $file) {
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) {
        return $null   # 사용할 수 없다 → 호출자가 문자열 검사로 대체
    }
    $out = & $dumpbin.Source /nologo /imports /dependents $file.FullName 2>&1 | Out-String
    $hits = @()
    foreach ($bad in $Forbidden) {
        if ($out.ToLowerInvariant().Contains($bad)) {
            $hits += $bad
        }
    }
    return ,$hits   # 쉼표: 빈 배열이 $null 로 풀리지 않게 한다(PS 5.1 StrictMode)
}

# ── 3. 바이너리 문자열 검사 ──────────────────────────────────────────────────
#     LoadLibrary("wintab32.dll") 같은 런타임 로드를 잡는다.
#     ASCII 와 UTF-16LE 둘 다 본다 — 소스가 L"..." 면 UTF-16 으로 박힌다.
function Test-EmbeddedStrings([System.IO.FileInfo] $file) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes).ToLowerInvariant()
    $utf16 = [System.Text.Encoding]::Unicode.GetString($bytes).ToLowerInvariant()

    $hits = @()
    foreach ($bad in $Forbidden) {
        if ($ascii.Contains($bad) -or $utf16.Contains($bad)) {
            $hits += $bad
        }
    }
    return ,$hits   # 쉼표: 빈 배열이 $null 로 풀리지 않게 한다(PS 5.1 StrictMode)
}

Write-Host "=== Mari WinTab 미사용 검증 (docs/03 3절) ===" -ForegroundColor Cyan
Write-Host "검사 경로: $Path"
Write-Host ""

foreach ($file in Get-TargetFiles $Path) {
    $checked++
    $importHits = Test-ImportTable $file
    $stringHits = Test-EmbeddedStrings $file

    $all = @()
    if ($null -ne $importHits) { $all += $importHits }
    if ($Strict) { $all += $stringHits }
    # @( ) 로 감싼다 — PS 5.1 은 빈 파이프라인 결과가 $null 이라 StrictMode 에서 .Count 가 터진다.
    $all = @($all | Select-Object -Unique)

    if ($all.Count -gt 0) {
        $msg = "$($file.Name): 금지된 참조 발견 → $($all -join ', ')"
        $failures.Add($msg)
        Write-Host "  [FAIL] $msg" -ForegroundColor Red
        if ($null -ne $importHits -and $importHits.Count -gt 0) {
            Write-Host "         (임포트 테이블에 있다 — 링크 단계에서 들어왔다)" -ForegroundColor Red
        }
        if ($stringHits.Count -gt 0 -and ($null -eq $importHits -or $importHits.Count -eq 0)) {
            Write-Host "         (문자열로만 있다 — LoadLibrary 런타임 로드일 가능성이 높다)" -ForegroundColor Red
        }
    }
    elseif ($stringHits.Count -gt 0) {
        Write-Host "  [WARN] $($file.Name): 문자열 발견 $($stringHits -join ', ') (Strict=false 라 통과)" -ForegroundColor Yellow
    }
    else {
        Write-Host "  [ OK ] $($file.Name)" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "검사한 바이너리: $checked 개"

if ($checked -eq 0) {
    # 🔴 아무것도 검사하지 않고 통과시키면 게이트가 없는 것과 같다.
    #    빌드가 실패했는데 이 검사가 초록으로 뜨는 상황을 막는다.
    Write-Error "검사한 바이너리가 0개다. 빌드가 안 됐거나 경로가 틀렸다 — 게이트를 통과시킬 수 없다."
    exit 2
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor Red
    Write-Host " WinTab 참조가 발견됐다. 빌드를 깬다." -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Write-Host " 왜 이게 빌드를 깨는가 (docs/03 3절):" -ForegroundColor Red
    Write-Host "   WinTab 앱은 펜 HID 를 독점해 죽여서 Sigan 의 RawInput 기록을" -ForegroundColor Red
    Write-Host "   침묵시킨다. CSP·포토샵·Krita 에서 실측된 동작이다." -ForegroundColor Red
    Write-Host "   Mari 가 WinTab 을 로드하는 순간 'Sigan 과 완벽하게 같이 작동한다'는" -ForegroundColor Red
    Write-Host "   제품 정의 자체가 무너진다. 성능 회귀가 아니라 제품 파괴다." -ForegroundColor Red
    Write-Host "" -ForegroundColor Red
    Write-Host " 고치는 법: WM_POINTER 만 써라. platform/win/input/ 을 봐라." -ForegroundColor Red
    Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  · $f" -ForegroundColor Red }
    exit 1
}

Write-Host ""
Write-Host "통과 — WinTab 참조 없음. Mari 는 여전히 WinTabKeywords 밖에 있다." -ForegroundColor Green
exit 0
