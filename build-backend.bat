@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio with Desktop development with C++ is required.
  pause
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
  echo MSBuild not found.
  pause
  exit /b 1
)
"%MSBUILD%" "%~dp0ResurrectedOfflineBots.sln" /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)

echo.
echo Built: %~dp0bin\ResurrectedOfflineBots.dll
pause
