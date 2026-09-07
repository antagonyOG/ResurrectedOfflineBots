@echo off
where go >nul 2>nul || (echo Go is not installed. Use the included prebuilt ResurrectedOfflineBots.exe. & pause & exit /b 1)
set GOOS=windows
set GOARCH=amd64
set CGO_ENABLED=0
pushd "%~dp0launcher"
go build -trimpath -ldflags="-s -w" -o "..\ResurrectedOfflineBots.exe" main.go
popd
if errorlevel 1 (pause & exit /b 1)
echo Built ResurrectedOfflineBots.exe
pause
