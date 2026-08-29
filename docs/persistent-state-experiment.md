# Persistent-state experiment (plan §7.2)

Goal: prove **which registry values Genshin / Star Rail actually rewrite**
when they run, so HoyoFlux protects exactly those - nothing more, nothing
less. Do not extend the code's field list from this document; this document
only records the *procedure* and its results. The code captures whatever
`Screenmanager*` values exist under the watched roots and restores them
verbatim, so the experiment's job is to validate **root coverage**, i.e.
that the watched roots see every value the game changes.

## Status: PENDING REAL-MACHINE VERIFICATION

The 1.0.0 release gate (plan Tests A-D) requires this experiment to be run
on a machine with the real game. Until then the session guard ships behind
the capability report's honest status and the compatibility matrix records
this as pending.

## Tool

```text
hoyoflux state-dump genshin     # read-only dump of watched roots
hoyoflux state-dump starrail
```

The dump prints, per candidate root: whether the key exists, and every
`Screenmanager*` value with its DWORD value decoded. It writes nothing.

## Procedure (per game, per install: CN / Global)

1. **Dump A (desktop baseline).** Launch the game the official way
   (HoYoPlay), set a known desktop resolution in-game, exit, then run
   `hoyoflux state-dump <game> > dump-A.txt`.
2. **Pollution run.** Launch through HoyoFlux with an iPad/mobile profile,
   play or reach the main menu, exit.
3. **Dump B.** `hoyoflux state-dump <game> > dump-B.txt`.
4. **Diff.** `git diff --no-index dump-A.txt dump-B.txt`.
   Every changed line is a value the game rewrote. For the F2/F3 gate,
   every changed value must be a `Screenmanager*` value under one of the
   dumped roots - otherwise the root list in the adapter is incomplete.
5. **Restore check.** Run `hoyoflux recover` (or let any later session
   finish) and re-dump: the changed values must be back to dump-A.

## Current watched roots (candidates, to be validated by this experiment)

| Game     | Region | Root (under HKCU)                  |
| -------- | ------ | ---------------------------------- |
| Genshin  | CN     | `Software\miHoYo\原神`             |
| Genshin  | Global | `Software\miHoYo\Genshin Impact`   |
| StarRail | CN     | `Software\miHoYo\崩坏：星穹铁道`   |
| StarRail | Global | `Software\Cognosphere\Star Rail`   |

If a real-machine dump shows the game writing display state elsewhere
(file-based config, another key), add that storage location to the
adapter's `persistent_state_roots()` - and re-run this experiment.
