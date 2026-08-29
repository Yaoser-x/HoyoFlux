# Compatibility matrix: legacy tool vs HoyoFlux (plan F15)

The legacy tool remains the behavior oracle. This matrix records, per
feature, where HoyoFlux reaches parity and where it deliberately improves on
the legacy behavior. "Verified" requires the real-game gate (B1); the
automated suite alone proves the mechanism, not the in-game result.

## Parity

| Feature                   | Upstream (legacy)        | HoyoFlux                  | Result        | Verified        |
| ------------------------- | ------------------------ | ------------------------- | ------------- | --------------- |
| FPS unlock (Genshin)      | sig redirect + sync thread | rip-relative redirect at RemoteState slot | parity | mechanism tested / B1 pending |
| FPS unlock (Star Rail)    | direct variable write    | direct variable write + mov flip | parity | mechanism tested / B1 pending |
| Custom DPI (Genshin)      | GetDPI prologue replacement | same patch, same prologue bytes | parity | mechanism tested / B1 pending |
| Mobile UI (Genshin)       | in-process il2cpp calls  | code stub + remote invocation | parity in mechanism | payload gated (B1) |
| Mobile UI (Star Rail)     | in-process il2cpp calls  | code stub + remote invocation | parity in mechanism | payload gated (B1) |
| Launch argument rendering | -screen-width/height     | same verified subset only | parity        | B1 pending      |
| Install detection         | launcher registry walk   | launcher registry walk (channel subkeys opened properly) | parity + bugfix | tested (registry) |

## Intentional behavioral improvements

| Behavior                     | Upstream (legacy)         | HoyoFlux                            | Why |
| ---------------------------- | ------------------------- | ----------------------------------- | --- |
| Persistent resolution pollution | session resolution stays in the game's saved settings and leaks into official launches | snapshot → event-driven guard → verified restore; journal survives crashes | the original bug that motivated the rewrite |
| Alt-Tab throttling           | power-save polled foreground with GetAsyncKeyState/Sleep loops; disabling still left hooks | `power_save disabled` registers nothing at all; enabled uses EVENT_SYSTEM_FOREGROUND and writes exactly 4 bytes | no silent hooks; testable regression gate |
| Hotkeys                      | GetAsyncKeyState + Sleep(50) polling | RegisterHotKey + message pump       | no busy-wait CPU burn |
| Unsupported features         | configured feature silently did nothing | launch stops pre-spawn with a reason (capability contract) | no silent no-ops |
| Crash recovery               | none / journal cleared blindly | restore → verify → only then clear; failed restore keeps the journal | state survives launcher crashes |
| Command-line quoting         | manual pre-quoting + second quoting pass | single strict CommandLineToArgvW encoder, round-trip tested | correct argv for paths with spaces/quotes/backslashes |
| Config parsing               | exception-throwing stoi paths | from_chars + explicit range checks; schema key for future migration | malformed config reports instead of crashing |
| Injection surface            | manual DLL loader, general shellcode | fixed-page code stubs with per-plan ownership, no DLL loading | minimal bootstrap only |

## Verification status

- Automated: 10 test executables, all green on the debug and release presets.
- Real-game gates (B1) pending on a machine with the games installed:
  desktop / iPad / Xiaomi resolution profiles, mobile UI payload validation,
  power-save Alt-Tab regression, persistent-state Tests A-D
  (see docs/persistent-state-experiment.md for the A/B procedure).
