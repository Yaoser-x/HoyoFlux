# HoyoFlux

> Session-level launcher, display configuration and runtime controller for
> HoYoverse PC games.

HoyoFlux is an independent reimplementation and architectural redesign of the
classic Genshin / Star Rail FPS unlocker tools. It is **not** a fork or a
"modded" fork of any existing project: the repository has a fresh git history,
a new architecture, a new configuration model and a new user interface.

**Status: `1.0.0-alpha.1` prerelease.** The architecture phase (A1-A10) and
the feature-closure phases (F0-F12) are implemented and unit-tested; the
remaining gates are real-game verification (plan B1) - the feature table
below marks exactly what is verified.

```
hoyoflux launch genshin --profile desktop
hoyoflux launch genshin --profile ipad
hoyoflux launch starrail --profile auto -- -popupwindow   # passthrough after --
hoyoflux doctor        # read-only environment + capability report
hoyoflux state-dump genshin
```

## What it does

- Launches Genshin Impact / Honkai: Star Rail (CN and Global) from the HoYoPlay
  registry install path.
- Applies a **profile** to a single game session. Every feature a profile
  names maps to a capability; a launch with a feature the adapter cannot
  honor stops with a reason *before the game starts* - there are no silent
  no-ops (`hoyoflux doctor` prints the same report).
- Session-scoped resolution: the game's persisted settings are snapshotted
  before launch, watched with an event-driven guard (`RegNotifyChangeKeyValue`,
  no polling) while the session runs, and restored after it exits.
- Crash-safe: a schema-2 journal records the rollback state; the next run
  restores it, verifies the restore, and only then clears the journal. A
  failed restore keeps the journal for a retry.
- Runtime controller while the game runs: power save reacts to foreground
  changes with an event hook (never a polling loop) and writes exactly four
  bytes; hotkeys (END toggle, Ctrl+Up/Down) adjust fps through the session's
  fps channel. With power save disabled **no listener exists at all** -
  HoyoFlux cannot influence fps on Alt-Tab.

## Feature status

Verified here means: exercised by the automated test suite. Real-game
verification (resolution takes effect in-game, mobile UI renders, the
official launcher keeps its settings) is the B1 gate and is **pending**.

| Feature                    | Genshin (CN/Global) | Star Rail (CN/Global) |
| -------------------------- | ------------------- | --------------------- |
| FPS unlock                 | implemented          | implemented           |
| Custom resolution          | implemented¹         | implemented¹          |
| Fullscreen windowed/exclusive | implemented¹      | implemented¹          |
| Fullscreen borderless      | unsupported²         | unsupported²          |
| Monitor selection          | unsupported³         | unsupported³          |
| Mobile UI                  | gated⁴               | gated⁴                |
| Custom DPI                 | implemented¹         | unsupported           |
| Power save (event-driven)  | implemented¹         | implemented¹          |
| Hotkeys (fps control)      | implemented¹         | implemented¹          |
| Session persistent-state guard | implemented¹     | implemented¹          |

¹ implemented and unit-tested; real-machine verification pending (B1).
² not expressible as a Unity launch argument; the launch fails with a reason
  instead of a guess. To be revisited via the persistent-state path.
³ no verified mechanism; requesting it stops the launch with a reason.
⁴ the bootstrap mechanism (code stub + remote invocation) exists and is
  unit-tested, but the payload is refused until validated on the live game.

## Scope

Windows x64 only. CLI first. No GUI main window, no online signature updates,
no driver, no generic DLL injector, no anti-cheat evasion.

## Building

Requirements: CMake ≥ 3.24, Ninja, and a C++23 compiler. On this project the
supported toolchain is **clang targeting x86_64-w64-windows-gnu** (a MinGW-w64
+ UCRT LLVM toolchain); all dependencies are header-only and fetched via
CMake `FetchContent`.

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## License

MIT — see [LICENSE](LICENSE). Third-party notices and upstream attribution in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
