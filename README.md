# ResurrectedOfflineBots

An offline/Sandbox AI mod for **Friday the 13th: Resurrected** that lets you play as a counselor against an autonomous Jason.

Friday the 13th never shipped with an offline Jason bot for counselor-side play. This project adds that missing mode to Resurrected's Sandbox environment.

## Main features

- Autonomous AI-controlled Jason
- Counselor hunting and pursuit
- Jason ability usage
- Visible throwing knives
- Strategic traps at important objectives
- Door interaction / breaking
- Stuck recovery and navigation handling
- Additional AI counselor spawning

## Using the mod

1. Launch **Friday the 13th: Resurrected** normally.
2. Go to **Offline Play -> Sandbox**.
3. Choose your map and Sandbox settings and start the session.
4. **Wait until Sandbox is fully loaded before running the mod.** Make sure the world and your counselor are completely loaded and the in-game cursor/UI is available.
5. Run `ResurrectedOfflineBots.exe` with `ResurrectedOfflineBots.dll` beside it.
6. Controls:
   - `F1` - spawn AI Jason
   - `F3` - spawn an AI counselor
   - `ESC` - close the launcher only

Do not start the launcher while the Sandbox level is still loading.

## Source / Nexus Mods review

This repository is the source package for the public **ResurrectedOfflineBots Minimal** build. It is intentionally source-only: compiled EXE/DLL files are not included here.

The project consists of:

- `launcher/main.go` - small Windows x64 launcher/controller
- `backend/` - C++ DLL containing the in-process AI backend
- `ResurrectedOfflineBots.sln` - Visual Studio 2022 solution
- `build-backend.bat` - builds the DLL in Release/x64
- `build-launcher.bat` - builds the launcher EXE

See [BUILDING.md](BUILDING.md) for reproducible build steps and [NEXUS_REVIEW.md](NEXUS_REVIEW.md) for security-review notes.

## Scope

This project is intended for **offline / Sandbox play only**. It does not include an anti-cheat bypass, kernel driver, online matchmaking support, network client, updater, persistence mechanism, or background service.

Backend diagnostics are written to:

`%TEMP%\ResurrectedOfflineBots.log`

## Credits

The main inspiration for this project was the anonymous **Jason AI and Offline Bots Mod** created for the original Friday the 13th: The Game. That project demonstrated what an AI-controlled Jason could be and was the major influence behind this Resurrected implementation.

Thank you to its anonymous creator and to the Resurrected/modding community for keeping the game alive.

Third-party source included in this repository: **MinHook**, distributed under its BSD 2-Clause license. See `LICENSES/MinHook-LICENSE.txt` and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Project code license

The project-specific source is published here for transparency, review, and reproducible builds. No additional license is granted for the project-specific code unless explicitly stated elsewhere by the author. Third-party components retain their own licenses.
