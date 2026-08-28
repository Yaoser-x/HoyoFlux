# HoyoFlux

> Session-level launcher, display configuration and runtime controller for
> HoYoverse PC games.

HoyoFlux is an independent reimplementation and architectural redesign of the
classic Genshin / Star Rail FPS unlocker tools. It is **not** a fork or a
"modded" fork of any existing project: the repository has a fresh git history,
a new architecture, a new configuration model and a new user interface.

```
hoyoflux launch genshin --profile desktop
hoyoflux launch genshin --profile ipad
hoyoflux launch starrail --profile auto
```

## What it does

- Launches Genshin Impact / Honkai: Star Rail (CN and Global) from the HoYoPlay
  registry install path.
- Applies a **profile** (resolution, FPS, mobile UI, DPI, power saving,
  priority) to a single game session.
- Session-scoped display configuration: persistent game settings are never
  polluted — on exit the previous state is restored.
- Crash-safe: an on-disk journal lets the next run recover a stale session
  (e.g. after HoyoFlux itself was killed).
- `hoyoflux doctor` reports signature freshness so a game update is obvious
  instead of silently failing.

## Scope

Windows x64 only. CLI + optional tray later. No GUI main window, no online
signature updates, no driver, no anti-cheat evasion.

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
