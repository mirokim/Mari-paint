@echo off
rem Mari Paint — Azure Trusted Signing 으로 파일 하나에 서명한다.
rem
rem 사용:   scripts\sign-win.cmd <파일>
rem 사전:   az login 이 돼 있어야 한다(테넌트가 MFA 면 `az login --tenant <id>`).
rem         az 가 PATH 에 없을 수 있다 — 아래서 직접 붙인다.
rem
rem 계정·프로필은 installer\azure-sign.json 에 있다(비밀 없음). Dlib 은 vpk 가 깔아 둔 것을 빌려 쓴다 —
rem 다른 곳에 있으면 MARI_AZ_DLIB 로 알려 준다.
rem 🔴 /tr 타임스탬프를 빼면 안 된다. Azure 인증서 수명이 72시간이라 타임스탬프가 없으면 사흘 뒤 서명이 죽는다.
setlocal
set "PATH=C:\Program Files\Microsoft SDKs\Azure\CLI2\wbin;%PATH%"
if "%MARI_AZ_DLIB%"=="" set "MARI_AZ_DLIB=%USERPROFILE%\.dotnet\tools\.store\vpk\1.2.0\vpk\1.2.0\vendor\signing\Azure.CodeSigning.Dlib.dll"
rem SDK 여러 판이 깔려 있으면 이름순으로 훑어 마지막(최신)을 잡는다.
set "KITS=%ProgramFiles(x86)%\Windows Kits\10\bin"
if "%MARI_SIGNTOOL%"=="" for /f "delims=" %%i in ('dir /b /on "%KITS%\10.*"') do (
    if exist "%KITS%\%%i\x64\signtool.exe" set "MARI_SIGNTOOL=%KITS%\%%i\x64\signtool.exe"
)
if not exist "%MARI_AZ_DLIB%" (echo sign-win: Dlib 없음: %MARI_AZ_DLIB% & exit /b 2)
if not exist "%MARI_SIGNTOOL%" (echo sign-win: signtool 없음 & exit /b 2)
"%MARI_SIGNTOOL%" sign /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 ^
    /dlib "%MARI_AZ_DLIB%" /dmdf "%~dp0..\installer\azure-sign.json" %1
exit /b %errorlevel%
