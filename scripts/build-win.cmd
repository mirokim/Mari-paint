@echo off
rem Mari Paint — Windows 로컬 빌드 래퍼 (docs/08 2절)
rem
rem 사용:   scripts\build-win.cmd [configure|build|test|all]   (기본 all)
rem
rem 이 파일이 하는 일 세 가지:
rem   1. vcvars64 를 불러 MSVC·midl·rc 를 PATH 에 올린다 (platform/win 이 midl.exe 를 찾는다)
rem   2. TMP/TEMP 를 ASCII 경로로 바꾼다.
rem      🔴 사용자 프로필 경로에 한글이 있으면 cl.exe 가 D8050 (c1.dll 실행 불가)로 죽는다.
rem         vcpkg 가 환경을 깎아서 넘길 때 특히 잘 터진다. 실측.
rem   3. vcpkg 툴체인을 넘긴다 (zlib · libpng · sqlite3).
setlocal
set MODE=%1
if "%MODE%"=="" set MODE=all

set VS_BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
if not exist "%VS_BT%\VC\Auxiliary\Build\vcvars64.bat" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS_BT=%%i
)
call "%VS_BT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem vcvars 가 VCPKG_ROOT 를 VS 동봉본(공백 있는 경로)으로 덮어쓴다. 그 뒤에 우리 것으로 고정.
set VCPKG_ROOT=C:\Dev\vcpkg

rem 🔴 콘솔 코드페이지를 고정한다(UTF-8). ninja 는 cl 의 /showIncludes 줄("참고: 포함 파일:")을
rem    CMake 가 구성 때 기록한 접두어와 **바이트로** 비교한다. 구성과 빌드의 코드페이지가 다르면
rem    (예: 구성은 cmd(CP949), 빌드는 PowerShell(UTF-8)) 헤더 의존성이 하나도 추적되지 않는다
rem    (실측: ninja -t deps → #deps 0 → 헤더를 고쳐도 일부만 재빌드 → 레이아웃이 섞여 힙 손상).
rem    구성·빌드가 전부 이 파일을 지나므로 여기서 한 번 고정하면 항상 같다.
chcp 65001 >nul

if not exist C:\Dev\tmp mkdir C:\Dev\tmp
set TMP=C:\Dev\tmp
set TEMP=C:\Dev\tmp

cd /d "%~dp0.."

if "%MODE%"=="configure" goto configure
if "%MODE%"=="build" goto build
if "%MODE%"=="test" goto test

:configure
rem Qt 6 (LGPL 모듈만 설치: qtbase · qtsvg). 없으면 GUI 없이 빌드된다.
if "%QT_DIR%"=="" set QT_DIR=C:\Qt\6.9.3\msvc2022_64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" ^
    "-DCMAKE_PREFIX_PATH=%QT_DIR%" || exit /b 1
if "%MODE%"=="configure" exit /b 0

:build
cmake --build build || exit /b 1
if "%MODE%"=="build" exit /b 0

:test
ctest --test-dir build --output-on-failure
exit /b %errorlevel%
