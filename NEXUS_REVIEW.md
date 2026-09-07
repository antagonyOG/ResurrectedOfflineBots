# Nexus Mods review notes

This source package is provided specifically to make the compiled **ResurrectedOfflineBots Minimal** upload easy to inspect and reproduce.

## Why the launcher may trigger automated scanners

The mod has two components:

1. `ResurrectedOfflineBots.exe` - launcher/controller
2. `ResurrectedOfflineBots.dll` - in-process AI backend

The launcher must load the companion DLL into the already-running `SummerCamp-Win64-Shipping.exe` process so Unreal Engine gameplay calls can execute safely on the game's own game thread.

Because of that design, `launcher/main.go` uses Windows APIs commonly associated with DLL loading/injection, including:

- `OpenProcess`
- `VirtualAllocEx`
- `WriteProcessMemory`
- `CreateRemoteThread`
- `LoadLibraryW`

These APIs can trigger heuristic detections even when used for a legitimate game mod. The source is intentionally unobfuscated so the behavior can be reviewed directly.

## What the launcher does

- Finds the running Friday the 13th: Resurrected process.
- Loads `ResurrectedOfflineBots.dll` from the same folder as the launcher.
- Sends the F1/F3 requests to exported DLL entry points.
- Reads keyboard state for the documented hotkeys.

## What is not present

The source package contains no:

- network client or download/update code
- persistence or startup registration
- Windows service installation
- scheduled tasks
- kernel driver
- anti-cheat bypass
- credential access
- browser/data collection
- online matchmaking automation

The intended runtime is **Offline Play -> Sandbox** only.

## Backend behavior

The DLL resolves the Resurrected Unreal Engine globals used by this build, installs the game-thread bridge, and runs the Jason/counselor AI logic. Diagnostic logging is written to:

`%TEMP%\ResurrectedOfflineBots.log`

## Reproducing the build

See `BUILDING.md`.

The public GitHub package contains source code and build scripts only. No game binaries or Friday the 13th assets are included.
