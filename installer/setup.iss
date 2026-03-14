; minilog Inno Setup installer script
;
; Build via CMake:
;   cmake --build --preset windows-release --target package
;
; Or manually:
;   ISCC /DSourceDir=<build-output-dir> /DAppVersion=<version> setup.iss
;
; Defines accepted on the ISCC command line:
;   SourceDir   — directory containing minilog.exe and minilog.pdb
;   ConfigDir   — directory containing the default minilog.conf
;   AppVersion  — version string, e.g. "0.1.0"
;   OutputDir   — where to write the installer .exe (default: SourceDir)

#ifndef SourceDir
  #define SourceDir "..\build\windows-release"
#endif
#ifndef ConfigDir
  #define ConfigDir "."
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef OutputDir
  #define OutputDir "{#SourceDir}"
#endif

[Setup]
AppName=minilog
AppVersion={#AppVersion}
AppPublisher=Saab AB
AppPublisherURL=https://github.com/SafirSDK/minilog
AppSupportURL=https://github.com/SafirSDK/minilog/issues
AppUpdatesURL=https://github.com/SafirSDK/minilog/releases
DefaultDirName={autopf}\MiniLog
DefaultGroupName=MiniLog
OutputDir={#OutputDir}
OutputBaseFilename=minilog-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
; No Start Menu entries — this is a service, not a GUI application.
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Components]
Name: "main"; Description: "minilog Syslog Server"; Types: full compact custom; Flags: fixed
Name: "pdb";  Description: "Debug Symbols (.pdb)";  Types: full

[Dirs]
; Create the log directory so the service can write logs immediately.
Name: "{commonappdata}\MiniLog\logs"; Components: main

[Files]
; Main binary.
Source: "{#SourceDir}\minilog.exe"; DestDir: "{app}"; Components: main; Flags: ignoreversion

; Debug symbols — optional component, skipped if the file doesn't exist.
Source: "{#SourceDir}\minilog.pdb"; DestDir: "{app}"; Components: pdb; \
    Flags: ignoreversion skipifsourcedoesntexist

; Default configuration — only written if the file does not already exist,
; so upgrades never overwrite a user-modified config.
Source: "{#ConfigDir}\minilog.conf"; \
    DestDir: "{commonappdata}\MiniLog"; \
    Components: main; Flags: onlyifdoesntexist uninsneveruninstall

[Run]
; Register the Windows service, pointing it at the installed config.
Filename: "{app}\minilog.exe"; \
    Parameters: "--install ""{commonappdata}\MiniLog\minilog.conf"""; \
    Flags: runhidden waituntilterminated; \
    StatusMsg: "Registering service..."

; Start the service.
Filename: "{sys}\sc.exe"; Parameters: "start minilog"; \
    Flags: runhidden waituntilterminated; \
    StatusMsg: "Starting service..."

[UninstallRun]
; Stop and remove the service before files are deleted.
Filename: "{app}\minilog.exe"; Parameters: "--uninstall"; \
    Flags: runhidden waituntilterminated; \
    RunOnceId: "UninstallService"

[Code]
// On upgrade installs, stop and remove the existing service before the new
// binary is copied, so the file is not locked.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Ignore errors — on a fresh install the exe doesn't exist yet.
    Exec(ExpandConstant('{app}\minilog.exe'), '--uninstall', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
