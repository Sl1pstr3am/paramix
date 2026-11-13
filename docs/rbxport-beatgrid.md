# rbxport beat-grid analyzer

Paramix includes a C++ port of the beat-grid pipeline from
[rbxport](https://github.com/chrisle/rbxport)'s `rbl-analysis` crate
(`crates/rbl-analysis`). It is available in **Preferences → Beat Detection →
Beat detector** alongside Queen Mary and SoundTouch.

## What was ported

The port covers the stages that produce a beat grid:

| C++ file | rbxport source | Stage |
| --- | --- | --- |
| `src/analyzer/plugins/rbxport/onset.cpp` | `onset.rs` | Spectral-flux onset envelopes (full band and low/kick band) |
| `src/analyzer/plugins/rbxport/attack.cpp` | `attack.rs` | Kick-attack map in the 900–9000 Hz band |
| `src/analyzer/plugins/rbxport/tempo.cpp` | `tempo.rs` | Tempo candidates, fitting, tempo-change segments, gap/ramp handling |
| `src/analyzer/plugins/rbxport/downbeat.cpp` | `downbeat.rs` | Arrangement-based beat 1 and phrase starts |
| `src/analyzer/plugins/rbxport/grid.cpp` | `lib.rs` (`finish_grid`, `anchor_file_start`) | Final numbering, half-beat correction, file-start anchor |
| `src/analyzer/plugins/analyzerrbxportbeats.cpp` | plugin wrapper | Downmix, buffering, `AnalyzerBeatsPlugin` |

Key detection, waveform generation, and rekordbox file I/O are **not**
ported; Paramix already has its own analyzers for those.

## Behavior in Paramix

- Audio is downmixed to mono and buffered until the analysis thread calls
  `finalize()`, because the pipeline is offline and reads the whole track.
  A five-minute track holds about 53 MB of mono float samples while queued.
- The returned beats start at the first downbeat. Paramix treats the first
  beat of a grid as the downbeat, so leading beats numbered 2–4 are dropped.
- With **Assume constant tempo** enabled (the default), `BeatFactory`
  flattens the result to a constant grid. Disable it to keep rbxport's tempo
  map, including tempo changes and ramps.
- Fast analysis (first 60 s only) applies to this engine like any other; the
  result is less reliable on a truncated track.

## Tuning

Defaults match rbxport's **RBXport** preset:

- BPM range 70–180
- kick-attack placement
- follow tempo changes, ramps, and re-phased returns
- arrangement-based downbeat selection

There is no separate preferences UI for these; the existing constant-tempo
and fast-analysis controls are the only knobs exposed today.

## Tests

`src/test/rbxportgridtest.cpp` ports rbxport's tempo unit tests (slowdowns,
ramps, kick-following tempo changes, kickless intros) and adds end-to-end
synthetic click-track tests for BPM, downbeats, file-start anchoring, and the
plugin's streaming interface.

## License

rbxport is GPL-2.0-or-later, which is compatible with this project. The ported
files retain a header pointing at the upstream source.
