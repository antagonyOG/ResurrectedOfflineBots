# Building the V2 Sandbox Lite Source

## Prerequisites
- Visual Studio 2022 (or Build Tools) with C++ and MSBuild
- Windows 64-bit target

## 1) Build backend DLL (AI core)

From this package root:

```powershell
msbuild backend\ResurrectedOfflineBots.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Output:
- `backend\bin\ResurrectedOfflineBots.dll`

You can also use:

```powershell
.\build-backend.bat
```

## 2) Build Lite launcher

From this package root:

```powershell
msbuild lite-launcher\LiteLauncher.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Output:
- `lite-launcher\bin\ResurrectedOfflineBots.exe`

You can also use:

```powershell
.\build-launcher.bat
```

That script builds the current C++ Lite launcher in `lite-launcher\LiteLauncher.vcxproj` and copies `ResurrectedOfflineBots.exe` to the package root.

## 3) Run-test flow

1. Copy `backend\bin\ResurrectedOfflineBots.dll` next to `ResurrectedOfflineBots.exe`/launcher executable.
2. Start game → Offline Play → Sandbox.
3. Load map, then launch `ResurrectedOfflineBots.exe` from Lite launcher / CLI.
4. In-game hotkeys:
   - **F1**: spawn AI Jason
   - **F3**: spawn one AI counselor (up to 6 total AI counselors in Sandbox)
