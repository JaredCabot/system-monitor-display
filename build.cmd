@echo off
rem Builds systemmonitordisplay.exe with MinGW-w64 (gcc on PATH) or, if
rem there is none, MSVC (run this from a Developer Command Prompt), then
rem runs the self-check.
cd /d "%~dp0"
where gcc >nul 2>nul
if not errorlevel 1 (
  windres app.rc -O coff -o app.res || exit /b 1
  rem Newer MinGW-w64 builds link a default manifest of their own, which
  rem clashes with app.manifest. An empty stand-in, found first, keeps it out.
  if not exist obj mkdir obj
  echo int smd_own_manifest; > obj\nomanifest.c
  gcc -c obj\nomanifest.c -o obj\default-manifest.o || exit /b 1
  gcc -B obj/ -std=c99 -O2 -Wall -mwindows -s -static -o systemmonitordisplay.exe systemmonitordisplay.c app.res -lgdi32 -lshell32 -ladvapi32 -lcomctl32 -lcomdlg32 -lole32 -luuid -lm
  goto built
)
where cl >nul 2>nul
if not errorlevel 1 (
  rc /nologo app.rc || exit /b 1
  cl /nologo /O2 /W3 /utf-8 systemmonitordisplay.c app.res user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib comdlg32.lib ole32.lib uuid.lib /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:NO
  goto built
)
echo No C compiler found. Install MinGW-w64 (gcc) or the Visual Studio Build Tools.
exit /b 1
:built
if errorlevel 1 exit /b 1
systemmonitordisplay.exe --selftest
