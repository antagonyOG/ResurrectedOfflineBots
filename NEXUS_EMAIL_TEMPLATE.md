# Nexus review email template

**Subject:** Request for Review - ResurrectedOfflineBots Minimal Quarantined File

Hi Nexus Mods Support,

My mod **ResurrectedOfflineBots Minimal** was automatically quarantined after upload. It contains compiled code: a small Windows x64 launcher EXE and a companion DLL used to add an offline AI-controlled Jason to Friday the 13th: Resurrected Sandbox mode.

I have published the source code and reproducible build instructions here:

**Source:** [PASTE GITHUB REPOSITORY URL]

**Build instructions:** [PASTE LINK TO BUILDING.md]

**Security/review notes:** [PASTE LINK TO NEXUS_REVIEW.md]

The launcher loads the companion DLL into the already-running game process, which requires Windows APIs such as OpenProcess, VirtualAllocEx, WriteProcessMemory, CreateRemoteThread, and LoadLibraryW. I understand those APIs may trigger automated heuristic scanners, so I have left the source unobfuscated and documented the behavior for review.

The project is intended for Offline Play -> Sandbox only. It does not contain a network client, persistence mechanism, kernel driver, service installation, or anti-cheat bypass.

Mod page: [PASTE NEXUS MOD URL]

Please let me know if you need any additional files or information.

Thank you,
Nick
