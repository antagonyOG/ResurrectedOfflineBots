# V2 changelog (Sandbox Lite)

## V2.x (Current source drop)

### Added
- Multi-map-aware Sandbox support:
  - Uses map/route state capture and stable world sync to keep AI attachment reliable after map changes.
- Dynamic counselor selection and distributed counselor AI spawn behavior.
- Full counselor convergence logic (`g_CounselorConvergence*`) with team routing and melee follow-up for end-sequence handling.
- Pamela sweater flow with repeatable re-arm and multiple activation opportunities.
- Final-kill route improvements for repeatable kill/revive checks.
- Car-aware / high-priority Jason endgame handling.
- Map-independent trap/teleport objective routing improvements and guarded trap/teleport cadence.
- Throwing-knife helper/pickup behavior kept in AI route and non-blocking scan cadence.

### Fixed
- Sequential Sandbox run/reattach behavior so AI can be requested again after a match ends.
- Death/kill-state cleanup to avoid stale AI interaction states on match restart.
- Reduced stale combat/freeze states by resetting counselor/kill team registries between phases.

### Notes
- This source package intentionally excludes public binaries and legacy/legacy-v1 source snapshots.
- The authoritative V2 source for this release is in `backend/src/*` and `backend/vendor/minhook`.
