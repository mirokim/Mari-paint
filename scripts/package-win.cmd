@echo off
rem Mari Paint — Windows 설치판 만들기 (Inno Setup + Azure Trusted Signing)
rem
rem 사용:   scripts\package-win.cmd [nosign]
rem 사전:   scripts\build-win.cmd 가 끝나 build\cli\mari-paint.exe 와 Qt 런타임이 있을 것.
rem         Inno Setup 6 (winget install JRSoftware.InnoSetup), az login (서명할 때).
rem 결과:   dist\MariPaint-<버전>-Setup.exe
rem
rem 하는 일: build\cli → dist\stage 로 실행에 필요한 것만 복사(테스트 exe·pdb·ilk 제외) → VC++ 런타임
rem DLL 을 옆에 놓는다(windeployqt 를 --no-compiler-runtime 으로 돌리므로) → mari-paint.exe 서명 →
rem ISCC 가 Setup 과 언인스톨러를 sign-win.cmd 로 서명 → 결과 서명·타임스탬프 검증.
setlocal
chcp 65001 >nul
cd /d "%~dp0.."
set NOSIGN=0
if /i "%1"=="nosign" set NOSIGN=1

if not exist build\cli\mari-paint.exe (echo package-win: build\cli\mari-paint.exe 없음 — 먼저 build-win.cmd & exit /b 1)
if not exist build\cli\Qt6Widgets.dll (echo package-win: Qt 런타임 없음 — GUI 없는 빌드는 설치판을 만들지 않는다 & exit /b 1)

rem 버전은 CMakeLists.txt 의 project(... VERSION x.y.z) 한 곳에서만 읽는다.
for /f "tokens=3" %%v in ('findstr /r /c:"^project(MariPaint VERSION" CMakeLists.txt') do set VERSION=%%v
if "%VERSION%"=="" (echo package-win: CMakeLists.txt 에서 버전을 못 읽음 & exit /b 1)
echo package-win: 버전 %VERSION%

set ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" (echo package-win: Inno Setup 6 없음 — winget install JRSoftware.InnoSetup & exit /b 1)

rem --- 1. 스테이징 ---------------------------------------------------------------
if exist dist\stage rmdir /s /q dist\stage
mkdir dist\stage
robocopy build\cli dist\stage /e /njh /njs /ndl /nfl ^
    /xd CMakeFiles ^
    /xf cli_binary.* cli_headless.* *.pdb *.ilk *.lib *.cmake CTestTestfile.cmake >nul
if errorlevel 8 (echo package-win: robocopy 실패 & exit /b 1)

rem --- 2. VC++ 런타임 (app-local) --------------------------------------------------
set VS_BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
if not exist "%VS_BT%\VC\Redist" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS_BT=%%i
)
set CRT=
for /d %%d in ("%VS_BT%\VC\Redist\MSVC\14.*") do if exist "%%d\x64\Microsoft.VC143.CRT" set "CRT=%%d\x64\Microsoft.VC143.CRT"
if "%CRT%"=="" (echo package-win: VC++ 런타임 폴더를 못 찾음 & exit /b 1)
for %%f in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_1.dll msvcp140_2.dll msvcp140_atomic_wait.dll msvcp140_codecvt_ids.dll) do copy /y "%CRT%\%%f" dist\stage\ >nul || exit /b 1

rem --- 3. 실행 파일 서명 ------------------------------------------------------------
if %NOSIGN%==1 goto pack
call scripts\sign-win.cmd dist\stage\mari-paint.exe || (echo package-win: mari-paint.exe 서명 실패 — az login 확인 & exit /b 1)

:pack
rem --- 4. Inno Setup ------------------------------------------------------------------
set SIGNARGS=
if %NOSIGN%==0 set SIGNARGS=/DWithSign "/Sazure=$q%CD%\scripts\sign-win.cmd$q $f"
"%ISCC%" /Q "/DMyAppVersion=%VERSION%" "/DStageDir=%CD%\dist\stage" "/DOutDir=%CD%\dist" %SIGNARGS% installer\mari-paint.iss || exit /b 1

set OUT=dist\MariPaint-%VERSION%-Setup.exe
if not exist "%OUT%" (echo package-win: 결과물이 없다: %OUT% & exit /b 1)

rem --- 5. 검증 ---------------------------------------------------------------------------
if %NOSIGN%==1 (echo package-win: 미서명 설치판 %OUT% & exit /b 0)
powershell -NoProfile -Command "$s=Get-AuthenticodeSignature '%OUT%'; if($s.Status -ne 'Valid' -or -not $s.TimeStamperCertificate){Write-Host ('package-win: 서명 검증 실패 ' + $s.Status + ' ts=' + [bool]$s.TimeStamperCertificate); exit 1}; Write-Host ('package-win: 서명 OK ' + $s.SignerCertificate.Subject)" || exit /b 1
for %%f in ("%OUT%") do echo package-win: 완료 %OUT% (%%~zf bytes)
exit /b 0
