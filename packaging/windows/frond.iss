; Inno Setup script. make-package.sh passes AppVersion, SourceDir and OutputDir.
[Setup]
AppName=Frond
AppVersion={#AppVersion}
AppPublisher=Frond
DefaultDirName={autopf}\Frond
DefaultGroupName=Frond
OutputDir={#OutputDir}
OutputBaseFilename=Frond-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\frond-desktop.exe

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Frond"; Filename: "{app}\frond-desktop.exe"
Name: "{autodesktop}\Frond"; Filename: "{app}\frond-desktop.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\frond-desktop.exe"; Description: "Start Frond"; Flags: nowait postinstall skipifsilent
