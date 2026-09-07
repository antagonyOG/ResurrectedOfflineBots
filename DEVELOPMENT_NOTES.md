# AI-only cleanup

This package is extracted from the working Features53 AI implementation. It intentionally removes Supremacy's general gameplay/QoL/debug surface and leaves only the bot backend needed by Jason + counselor AI.

Removed from the backend:
- DirectX / Kiero rendering path, ImGui, menu, ESP and overlay code.
- Human-Jason knife tracer and human trap tracer used during reverse-engineering.
- Obsolete knife flight/visibility diagnostic helpers that no longer participate in the proven world-space knife launch.
- Old unused trap-distance helpers from pre-Features53 iterations.
- Custom counselor perks, startup items, always-health-spray/knife maintenance, ExpandedSmallItems, MaxCounselorStats and NoJasonSense.
- Pocket-knife controller shortcut and XInput dependency.
- Old F6 counselor shortcut and the old all-features `Features::Tick()`.
- Generic movement/FOV/fear/stamina/regen/noclip/interaction/damage/aimbot/host/ESP ticks.
- Base-game OfflineBots executable probing from Engine.cpp; this backend is Resurrected-only.

Retained deliberately:
- AI resource cache / game-thread ProcessEvent bridge.
- F1 Jason request + SCKillerAIController handoff and Tick hook.
- Counselor AI native behavior-tree spawning and target registration.
- Fast counselor registry and retargeting.
- Chase / MoveTo / stuck recovery / door behavior.
- Slash, grab, quick grab-kill and visible throwing-knife behavior.
- Phone + car startup traps, stable phone approach, transit walking/combat.
- Shared 20-second Morph cooldown, including the >100m rule.

The cleaned `Features.cpp` has no single-reference static functions after extraction, which was used as a basic dead-code sanity check.
