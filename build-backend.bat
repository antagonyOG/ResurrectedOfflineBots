@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio 2022 or Build Tools with Desktop development with C++ is required.
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
copy /y "%~dp0bin\ResurrectedOfflineBots.dll" "%~dp0ResurrectedOfflineBots.dll" >nul
echo.
echo Built: %~dp0ResurrectedOfflineBots.dll
echo Keep it beside ResurrectedOfflineBots.exe.
pause
