; Mari Paint — Windows 설치판 (Inno Setup 6)
;
; 직접 부르지 말고 scripts\package-win.cmd 를 쓴다. 그쪽이 build\cli 에서 dist\stage 를 꾸리고
; 버전·서명 명령을 /D, /S 로 넘겨 준다:
;   /DMyAppVersion=0.1.0  /DStageDir=..\dist\stage  /DOutDir=..\dist  /DWithSign  /Sazure="...sign-win.cmd" $f
;
; 사용자별 설치(관리자 권한 없음, %LocalAppData%\Programs\Mari Paint). 앱은 콘솔 서브시스템이라
; 인자 없이 뜨면 GUI 인데(cli/CMakeLists.txt), 바로가기로 띄우면 콘솔 창이 같이 뜬다 — 알고 있는 것.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif
#define MyAppName "Mari Paint"
#define MyAppExe "mari-paint.exe"

[Setup]
AppId={{7B2E9C4A-5D31-4F8E-9A6B-2C1D0E3F4A5B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=MMWO
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=MariPaint-{#MyAppVersion}-Setup
SetupIconFile=..\ui\icons\app\mari-paint.ico
UninstallDisplayIcon={app}\{#MyAppExe}
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 서명 명령은 package-win.cmd 가 /Sazure=... 로 준다. 없으면 미서명으로 그냥 만든다.
#ifdef WithSign
SignTool=azure
SignedUninstaller=yes
#endif

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
