@echo off
echo [=============================]
echo [ Building Throttle.exe ]
echo [=============================]

:: Attempt to locate vcvars64.bat in common VS 2019/2022 paths
set VCVARS_2019="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCVARS_2022="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if exist %VCVARS_2019% (
    echo [*] Found VS 2019 Developer Tools...
    call %VCVARS_2019% >nul 2>&1
) else if exist %VCVARS_2022% (
    echo [*] Found VS 2022 Developer Tools...
    call %VCVARS_2022% >nul 2>&1
) else (
    echo [!] Warning: Could not find vcvars64.bat. The build might fail if cl.exe is not in PATH.
)

:: Compile the code
echo [*] Compiling source files...
cl /I. /Isrc /O2 src\main.c src\utils.c src\bucket.c src\conntable.c src\engine.c src\gui.c WinDivert.lib /link /subsystem:windows /ENTRY:mainCRTStartup /out:throttle.exe User32.lib Gdi32.lib Comctl32.lib Dwmapi.lib Uxtheme.lib
if %ERRORLEVEL% neq 0 (
    echo [!] Build failed!
    exit /b %ERRORLEVEL%
)

:: Ensure WinDivert.dll exists (the linker hardcodes WinDivert.dll)
if exist WinDivert64.dll (
    if not exist WinDivert.dll (
        copy /Y WinDivert64.dll WinDivert.dll >nul
        echo [+] Copied WinDivert64.dll to WinDivert.dll for runtime.
    )
)

echo [+] Build successful! throttle.exe is ready.
