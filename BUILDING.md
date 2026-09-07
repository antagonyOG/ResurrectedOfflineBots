# Building ResurrectedOfflineBots

These instructions reproduce the two compiled components used by the mod: the C++ backend DLL and the Go launcher EXE.

## Requirements

### Backend DLL

- Windows 10 or Windows 11
- Visual Studio 2022 or Visual Studio 2022 Build Tools
- **Desktop development with C++** workload
- Windows 10/11 SDK
- MSVC v143 x64 toolset

### Launcher EXE

- Go installed for Windows
- No third-party Go packages are required; the launcher uses the standard library only.

## Build the backend DLL

From the repository root:

1. Run `build-backend.bat`.
2. The script locates MSBuild through `vswhere.exe`.
3. It builds `ResurrectedOfflineBots.sln` as **Release | x64**.
4. The compiled DLL is produced under `bin\ResurrectedOfflineBots.dll` and copied to the repository root as `ResurrectedOfflineBots.dll`.

Equivalent Visual Studio steps:

1. Open `ResurrectedOfflineBots.sln` in Visual Studio 2022.
2. Select **Release** and **x64**.
3. Build the solution.

## Build the launcher EXE

From the repository root:

1. Run `build-launcher.bat`.
2. The script sets `GOOS=windows`, `GOARCH=amd64`, and `CGO_ENABLED=0`.
3. It builds `launcher\main.go` into `ResurrectedOfflineBots.exe`.

Equivalent command from the repository root:

    cd launcher
    set GOOS=windows
    set GOARCH=amd64
    set CGO_ENABLED=0
    go build -trimpath -ldflags="-s -w" -o ..\ResurrectedOfflineBots.exe main.go

## Runtime layout

After both builds, keep these two files together:

    ResurrectedOfflineBots.exe
    ResurrectedOfflineBots.dll

Then fully load **Offline Play -> Sandbox** in Friday the 13th: Resurrected before starting the EXE.
