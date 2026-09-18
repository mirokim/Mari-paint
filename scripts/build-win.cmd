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

if not exist C:\Dev\tmp mkdir C:\Dev\tmp
set TMP=C:\Dev\tmp
set TEMP=C:\Dev\tmp

cd /d "%~dp0.."

if "%MODE%"=="configure" goto configure
if "%MODE%"=="build" goto build
if "%MODE%"=="test" goto test

:configure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" || exit /b 1
if "%MODE%"=="configure" exit /b 0

:build
cmake --build build || exit /b 1
if "%MODE%"=="build" exit /b 0

:test
ctest --test-dir build --output-on-failure
exit /b %errorlevel%
