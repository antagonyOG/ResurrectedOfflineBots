# Resurrected Offline Bots – V2 Sandbox Lite Source

This folder is the source-only Nexus/GitHub-ready release for:

- **V2 Sandbox Lite** AI behavior
- map-aware/offline sandbox spawning
- improved Jason AI and counselor AI convergence + final-kill flow
- dynamic, non-menu launcher control (F1/F3 hotkeys)

Included in this package:
- C++ source under `backend/src` (current V2 implementation)
- current Lite launcher source (`lite-launcher/`)
- Visual Studio project files (`ResurrectedOfflineBots.sln`, `backend/ResurrectedOfflineBots.vcxproj`, `lite-launcher/LiteLauncher.vcxproj`)
- MinHook source (`backend/vendor/minhook`)
- build scripts (`build-backend.bat`, `build-launcher.bat`)
- build/package docs (`BUILDING.md`, `V2-CHANGELOG.md`, `THIRD-PARTY-NOTICES.md`)

Not included: binaries, logs, tests, backups, object files, symbol files, and legacy distribution folders.

For this release, the authoritative implementation is:
- `backend/src/Game/Setup/FrozenJasonBridge.cpp`
- `backend/src/Game/Features/Features.cpp`
- `backend/src/Game/Engine/Engine.cpp`
- `backend/src/Game/Setup/OfflineSetup.cpp`

These files are the ones used by the current V2 Lite DLL build workflow.
