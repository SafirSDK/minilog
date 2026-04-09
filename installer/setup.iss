; minilog Inno Setup installer script
;
; Build via CMake:
;   cmake --build --preset windows-release --target package
;
; Or manually:
;   ISCC /DSourceDir=<build-output-dir> /DAppVersion=<version> setup.iss
;
; Defines accepted on the ISCC command line:
;   SourceDir      — directory containing minilog.exe and minilog.pdb
;   WebViewerDir   — directory containing minilog-web-viewer.exe
;   ConfigDir      — directory containing the default minilog.conf
;   AppVersion     — version string, e.g. "0.1.0"
;   OutputDir      — where to write the installer .exe (default: SourceDir)
;   WebViewerAddr  — listen address for the web viewer (default: :8080)

#ifndef SourceDir
  #define SourceDir "..\build\windows-release"
#endif
#ifndef WebViewerDir
  #define WebViewerDir "{#SourceDir}"
#endif
#ifndef ConfigDir
  #define ConfigDir "."
#endif
#ifndef AppVersion
  #define AppVersion "1.3.0"
#endif
#ifndef OutputDir
  #define OutputDir "{#SourceDir}"
#endif
#ifndef WebViewerAddr
  #define WebViewerAddr ":8080"
#endif

[Setup]
AppName=minilog
AppVersion={#AppVersion}
AppPublisher=Saab AB
AppPublisherURL=https://github.com/SafirSDK/minilog
AppSupportURL=https://github.com/SafirSDK/minilog/issues
AppUpdatesURL=https://github.com/SafirSDK/minilog/releases
DefaultDirName={autopf}\minilog
DefaultGroupName=minilog
OutputDir={#OutputDir}
OutputBaseFilename=minilog-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
; Program group page not needed — shortcuts are managed via the components tree.
DisableProgramGroupPage=yes
SetupIconFile=..\artwork\minilog.ico
WizardImageFile=wizard-image.png
WizardSmallImageFile=wizard-header.png

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Components]
Name: "main";       Description: "minilog Syslog Server";  Types: full compact custom; Flags: fixed
Name: "webviewer";  Description: "minilog Web Viewer";     Types: full
Name: "webviewer\shortcuts"; Description: "Start Menu and Desktop Shortcuts"; \
    Types: full
Name: "pdb";        Description: "Debug Symbols (.pdb)"

[Dirs]
; Create the log directory so the service can write logs immediately.
Name: "{commonappdata}\minilog\logs"; Components: main
; Tools directory for PATH-accessible utilities.
Name: "{app}\tools"; Components: main

[Files]
; Main binary.
Source: "{#SourceDir}\minilog.exe"; DestDir: "{app}"; Components: main; Flags: ignoreversion

; CLI viewer script — installed to tools subdirectory (will be in PATH).
Source: "..\src\cli-viewer\minilog-cli-viewer.py"; DestDir: "{app}\tools"; Components: main; Flags: ignoreversion

; Web viewer binary.
Source: "{#WebViewerDir}\minilog-web-viewer.exe"; DestDir: "{app}"; Components: webviewer; \
    Flags: ignoreversion skipifsourcedoesntexist

; Debug symbols — optional component, skipped if the file doesn't exist.
Source: "{#SourceDir}\minilog.pdb"; DestDir: "{app}"; Components: pdb; \
    Flags: ignoreversion skipifsourcedoesntexist

; Default configuration — only written if the file does not already exist,
; so upgrades never overwrite a user-modified config.
Source: "{#ConfigDir}\minilog.conf"; \
    DestDir: "{commonappdata}\minilog"; \
    Components: main; Flags: onlyifdoesntexist uninsneveruninstall

; Viewer configuration — only written if the file does not already exist.
Source: "..\src\cli-viewer\minilog-cli-viewer.conf.example"; \
    DestDir: "{commonappdata}\minilog"; \
    DestName: "minilog-cli-viewer.conf"; \
    Components: main; Flags: onlyifdoesntexist uninsneveruninstall

[Icons]
; Web viewer shortcuts — open the viewer URL in the default browser.
Name: "{autoprograms}\minilog Web Viewer"; Filename: "http://localhost{#WebViewerAddr}"; \
    IconFilename: "{app}\minilog-web-viewer.exe"; IconIndex: 0; \
    Components: webviewer\shortcuts
Name: "{autodesktop}\minilog Web Viewer"; Filename: "http://localhost{#WebViewerAddr}"; \
    IconFilename: "{app}\minilog-web-viewer.exe"; IconIndex: 0; \
    Components: webviewer\shortcuts

[Registry]
; Add tools directory to system PATH so CLI utilities are accessible from anywhere.
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\tools"; \
    Check: NeedsAddPath('{app}\tools')

[Run]
; Register the Windows service, pointing it at the installed config.
Filename: "{app}\minilog.exe"; \
    Parameters: "--install ""{commonappdata}\minilog\minilog.conf"""; \
    Flags: runhidden waituntilterminated; \
    StatusMsg: "Registering service..."

; Start the minilog syslog service.
Filename: "{sys}\sc.exe"; Parameters: "start minilog"; \
    Flags: runhidden waituntilterminated; \
    StatusMsg: "Starting service..."

; Register the web viewer service (only if the component was selected).
Filename: "{app}\minilog-web-viewer.exe"; \
    Parameters: "--install --config ""{commonappdata}\minilog\minilog.conf"" --addr ""{#WebViewerAddr}"""; \
    Flags: runhidden waituntilterminated; \
    Components: webviewer; \
    StatusMsg: "Registering web viewer service..."

; Start the web viewer service.
Filename: "{sys}\sc.exe"; Parameters: "start minilog-web-viewer"; \
    Flags: runhidden waituntilterminated; \
    Components: webviewer; \
    StatusMsg: "Starting web viewer service..."

[UninstallRun]
; Stop and remove the web viewer service before files are deleted.
Filename: "{app}\minilog-web-viewer.exe"; Parameters: "--uninstall"; \
    Flags: runhidden waituntilterminated skipifdoesntexist; \
    RunOnceId: "UninstallWebViewer"

; Stop and remove the syslog service before files are deleted.
Filename: "{app}\minilog.exe"; Parameters: "--uninstall"; \
    Flags: runhidden waituntilterminated; \
    RunOnceId: "UninstallService"

[Code]
// Check if a path needs to be added to the system PATH.
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath)
  then begin
    Result := True;
    exit;
  end;
  // Check if our path already exists (case-insensitive)
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

// On upgrade installs, stop and remove existing services before new binaries
// are copied, so the files are not locked.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Ignore errors — on a fresh install the exes don't exist yet.
    Exec(ExpandConstant('{app}\minilog-web-viewer.exe'), '--uninstall', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{app}\minilog.exe'), '--uninstall', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
