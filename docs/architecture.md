# HoyoFlux architecture

## Layer / dependency graph

Dependencies point strictly downward. A lower layer never references a higher
one; the compiler enforces this because every layer is a separate CMake
target.

```
        domain
        ^   ^
        |   |
platform   scan
   ^        ^
   |        |
 patch  <---+
   ^
   |
 game
   ^
   |
 session
   ^
   |
  app
   ^
   |
  cli
```

| Layer       | Directory       | Responsibility                                        |
| ----------- | --------------- | ----------------------------------------------------- |
| domain      | src/domain      | Pure data model (Profile, LaunchRequest, Error, …). No I/O. |
| platform    | src/platform/win32 | Win32 boundary: process spawn, registry, PE, display, privilege. |
| scan        | src/scan        | Pattern scanner + compiled patterns + module snapshot. |
| patch       | src/patch       | Patch engine, remote memory, remote state.            |
| game        | src/game        | Game knowledge: adapters + per-game signatures.       |
| session     | src/session     | Session engine, journal, display guard, rollback.     |
| app         | src/app         | Command dispatch, validation, wiring.                 |
| frontend    | src/frontend    | CLI / tray.                                            |

## Design decisions

- **Plain Win32, no syscall layer.** The legacy project routed everything
  through a private NTSYSAPI syscall shim for anti-analysis reasons. HoyoFlux
  uses ordinary Win32 (`CreateProcessW`, `ReadProcessMemory`, …) and makes no
  attempt at anti-cheat evasion.
- **Self-contained remote state.** The legacy shellcode read the unlocker's
  `FpsValue` across process boundaries, forcing the launcher to stay resident.
  HoyoFlux allocates a `RemoteState` block inside the game; fixed profiles are
  fully non-resident after patch+resume.
- **Session-scoped display config.** Persistent game settings are snapshotted,
  guarded while the session runs, and restored on exit — the launcher's config
  is never polluted.
- **GameAdapter produces PatchPlan, PatchEngine executes.** Game knowledge is
  decoupled from memory writes.
- **TOML profiles parsed once.** Config never enters the hot path.
- **UTF-8 sources.** The legacy project stored sources as UTF-16LE; everything
  here is UTF-8.

## Session lifecycle

```
Idle -> Preparing -> Launching -> Resolving -> Patching -> Running
                                                              |
                                                              v
                                      Restoring -> Completed
```

Any stage failing routes `Failed -> Rollback -> Completed`. The SessionEngine
is the only module allowed to restore state, kill the game, or exit.
