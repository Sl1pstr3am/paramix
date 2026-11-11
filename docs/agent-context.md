# Paramix — Working Context (agent handoff)

**Purpose:** this is the living handoff document. If a session runs out of context, read this
file first, then `docs/paramix-stem-beatgrid-ui-plan.md` (the program plan) and
`docs/old-context.md` (raw transcript of the original planning session).

**Last updated:** 2026-11-11, during Phase −1 (baseline).
**Update rule:** every agent that changes repo state must append to §9 (Work log) and correct
§3 (Current state) before finishing.

---

## 1. What this project is

`paramix` is a fork of **Mixxx 2.7.0-alpha** (GPL-2.0, C++20, Qt 6, CMake ≥ 3.22) being turned
into a commercial-grade DJ application with four headline features:

1. A new beatgrid engine (downbeats, confidence, tempo maps, non-destructive editing).
2. Offline 4-stem separation comparable to Traktor (HTDemucs via ONNX Runtime).
3. A 4-stem waveform (stacked lanes **and** tinted composite).
4. A general UI overhaul, rebranded as **Paramix** (new QML skin as default).

- Local path: `/Users/christianstuart/Documents/paramix`
- Remote: `https://github.com/Sl1pstr3am/paramix` (branch `main`) — **public** as of 2026-11-11
- No `upstream` remote by explicit user request: **do not sync with mixxxdj/mixxx.**
  The imported tree is the source of truth.
- Upstream basis recorded as "Mixxx 2.7.0-alpha"; keep GPL notices and attribution.

**AI policy (AGENTS.md):** this fork is aided by AI agents; disclosure of AI involvement is not
required. Architecture conventions still apply: `ControlObject`/`ControlProxy` with
`[Group], key_name`; the engine thread is real-time (no allocation/locks, may emit but not
receive Qt signals); `parented_ptr`/`make_parented` for Qt ownership.

---

## 2. Locked decisions (do not re-litigate)

| # | Decision |
|---|---|
| 1 | Default separator `htdemucs_ft` (~630 MB fp16); single `htdemucs` only as low-end fallback |
| 2 | Separation on demand + optional batch; **no** auto-separate on import |
| 3 | Downbeats/bars are **required in v1** |
| 4 | `.stem.mp4` **export** (Traktor interop) in v1 |
| 5 | New **Paramix QML skin** is default and highly customisable |
| 6 | Windows x64, Linux x64 (+arm64 where feasible), macOS arm64 **and** x86_64 |
| 7 | Models are **download-only via GitHub Releases**; nothing in git or installers (MIT licence prompt, SHA-256, resume, install-from-file) |
| 8 | Rebrand as **Paramix**; brand-only in v1 (keep `~/.mixxx` data dir) |

Program option chosen: **B "Traktor-class"** (~20–24 weeks, 6–8 agents), with Option A as the
first shippable milestone. Option C (real-time separation) is a deferred spike.

Still-open defaults (plan §11): dedicated `paramix-models` release repo; `0.1` versioning;
auto GPU with opt-out (macOS 13+, Win10 21H2+, Ubuntu 22.04+/Fedora 39+); ALAC default for
`.stem.mp4`; RoFormer "Max" tier in v1.1; full runtime QML overrides (experimental); no telemetry.

---

## 3. Current state (truth as of the last update)

### 3.1 Git / repo
- Commits: `96f541b` Initial commit → `8c67ba8` Update repository (import, 6035 files) →
  `0901e4b` Update repository (docs) → `2ef4670` ci: fix baseline workflow flags and hygiene.
- History is linear on `main`; the user chose **commit directly to main** (no baseline branch).
- `.DS_Store` was tracked and is now untracked + gitignored.
- Tracked size ≈ 180 MB; `.git` ≈ 82 MB. Largest blobs are legitimate stem test fixtures
  (`src/test/stems/stem02/trance_ALAC_24bit.stem.mp4`, 12 MB).

### 3.2 Build environment on this machine
- macOS x86_64 (Darwin 25.6), Apple clang 21, Xcode CLT only.
- **No** CMake, Ninja, Homebrew, clang-format, or Qt. **MacPorts 2.12.6 IS installed** (`port`).
  147 GB free. A local Qt install (`sudo port install qt6 …`) is possible but needs user
  approval and 20–40 min + a 30–60 min compile.
- `python3` is 3.9.6 (system). `gh` CLI authenticated as `Sl1pstr3am`.

### 3.3 Phase −1 status (baseline gate: "CI green, formatting enforced")
| Item | Status |
|---|---|
| Commit the Mixxx import | **done** |
| Dotfiles present (`.clang-format`, `.clang-tidy`, `.gitignore`, `.gitattributes`, `.pre-commit-config.yaml`) | **done** |
| `.github/workflows/ci.yml` functional | **fixed** (phantom `-DQT6=ON` removed, `WARNINGS_FATAL` dropped, timeouts + ccache added, PEP 668 tolerated) |
| Repo hygiene (`.DS_Store`, ignore rules) | **done** |
| Pre-commit suite usable | **done** — missing root configs added (`.codespellignore`, `.codespellignorelines`, `.cmakelintrc`, `.gersemirc`, `.prettierrc.yaml`, `.markdownlint-cli2.yaml`); `no-commit-to-branch` no longer blocks `main`. `black`/`flake8` hooks need Python ≥3.10 (local 3.9 can't build them; CI 3.12 can) |
| Local verification harness | **done** — `tools/paramix_verify.sh` (6 checks, PASS/SKIP/FAIL), `tools/paramix_bootstrap.sh` (dry-run by default, `--yes` to install), `docs/baseline-local-verification.md` |
| CI actually green on 4 targets | **in progress** — runs now execute; `checks` whitespace bug fixed (shallow-clone `HEAD` has no parent, so `git show --check HEAD` flagged the whole import; now `git diff --check HEAD^ HEAD` with `fetch-depth: 2`) |

### 3.4 Feature work
**Not started.** Verified absent: `src/stems/`, `SoundSourceStemCache`, `BeatGrid-3.0`,
`src/analyzer/plugins/beatml/`, `res/skins/Paramix/`, `docs/adr/`. Schema is at revision 40
(plan needs 41). Present from upstream: `src/track/beat*`, `analyzerqueenmarybeats`,
`analyzersoundtouchbeats`, `waveformrendererstem`, `qmlstemsmodel`, `soundsourcestem`.

---

## 4. Where the code already supports the plan (don't rebuild these)

- **Stems playback exists:** `SoundSourceSTEM` (`src/sources/soundsourcestem.cpp`, 8 channels,
  NI 5-stream layout), `EngineDeck::processStem()` (`src/engine/channels/enginedeck.cpp`),
  per-stem control groups `[ChannelN_Stem1..4]` (`volume`, `mute`, `color`, VU),
  `kMaxSupportedStems = 4` (`src/engine/engine.h`), `WaveformData::stems[4]`,
  `allshader::WaveformRendererStem`, `src/qml/qmlstemsmodel.*`, `WTrackStemMenu`,
  fixtures in `src/test/stems/`.
- **Beatgrid model is tempo-map capable:** `mixxx::Beats` (`src/track/beats.h`) = markers +
  terminal tempo; serialised as `BeatGrid-2.0` / `BeatMap-1.0`; `BeatGrid-1.0` readable.
  Detection: `AnalyzerBeats` → Queen Mary or SoundTouch only (string `if/else`, no dynamic
  loading). `BeatUtils` irons beats and snaps BPM heuristically. No downbeats/confidence.
- **Integration point for generated stems:** hook `SoundSourceProxy::openAudioSource()` (it
  already holds the `TrackPointer`) to return a stem source when a cache exists; the existing
  8-channel engine path then works unchanged. `CachingReaderWorker` already requests 8 channels.
- **Waveform constraint:** scene-graph backend uses `kPixelPerStrip = 2` (precision compromise);
  stem renderer is 6 verts × 4 stems × 2 layers = 48 verts/pixel column, CPU-built per frame.
- **Skin reality:** legacy XML skins are parsed by `legacyskinparser.cpp`; `LateNightQML` is the
  only QML skin; library and preferences are still Qt Widgets (`Library.qml` embeds
  `Mixxx.LegacyLibraryItem`). QML skin has 0 `qsTr()` calls — localisation debt to fix.

---

## 5. Known blockers and gotchas

1. **GitHub Actions runner acquisition is very slow (first-run cold start).** Runs sat in
   `queued` with jobs created but no runner for ~8–10 minutes after the repo was made public;
   then runners appeared and jobs executed. This is no longer a blocker — but expect several
   minutes of latency before a run starts. Runs from 20:22/20:26 that predate the visibility
   change may remain permanently queued; ignore them.
2. **Whitespace gate was wrong and is fixed.** `git show --check HEAD` flagged the imported
   Mixxx tree's pre-existing trailing whitespace on every run. The workflow now runs
   `git diff --check HEAD^ HEAD`, so only the pushed commit is gated.
3. **`-DQT6=ON` is not a CMake option** in this tree (Qt 6 is mandatory). The real switch is
   `QML` (`cmake_dependent_option`, CMakeLists.txt:2357). `WARNINGS_FATAL` (CMakeLists.txt:2210)
   defaults OFF — do not turn it on for the baseline.
4. **macOS/Windows buildenv env propagation** is handled by the buildenv scripts writing to
   `$GITHUB_ENV` themselves; no cross-step `source` needed.
5. macOS x86_64 CI builds with `-DQML=OFF` (carried over). This is a **gap** against decision 6
   (QML skin is default); revisit once CI runs.
6. No local compiler toolchain beyond Apple clang: **any "build is green" claim must come from
   CI**, not from this machine, until Qt is installed.
7. `docs/old-context.md` is a raw chat transcript (≈1,267 lines, mostly UI padding). Use it only
   for historical intent; the plan document is authoritative.
8. `concurrency: cancel-in-progress: true` works: a new push cancels the previous run's jobs,
   which can look like a spurious "cancelled" status.

---

## 6. Phase −1 remaining work (in priority order)

1. **Watch CI to green on all four targets.** `checks` + `Linux x64` first (Linux also runs
   `ctest`), then macOS arm64, macOS x86_64, Windows x64. Diagnose failures from job logs:
   `gh run view --log --job <job-id> -R Sl1pstr3am/paramix`.
2. Revisit the macOS x86_64 `-DQML=OFF` gap once the first build is green.
3. Write `docs/baseline-report.md`: baseline commit, toolchain versions, CI evidence (or the
   documented gap), and the exact gate status per target.
4. Then Phase 0: ADRs 001–005, plus the five spikes in plan §7.3
   (ONNX htdemucs_ft benchmark, Beat This! F-measure, 8-ch FLAC round-trip, 4-lane 60 fps
   prototype, GitHub Releases model-download dry run).

---

## 7. Useful commands

```bash
cd /Users/christianstuart/Documents/paramix

# CI state
gh run list -R Sl1pstr3am/paramix -L 5
gh api repos/Sl1pstr3am/paramix/actions/runs/<id>/jobs \
  --jq '.jobs[] | "\(.name): \(.status) \(.conclusion // "")"'
gh workflow run ci.yml -R Sl1pstr3am/paramix

# Local checks (no Qt needed)
python3 -c "import yaml;yaml.safe_load(open('.github/workflows/ci.yml'))"
git grep -nE '^(<<<<<<< |======= |>>>>>>> )' -- ':!*.md'
git show --check --format= HEAD

# Toolchain (only with user approval)
tools/paramix_bootstrap.sh --yes          # cmake, ninja, clang-format, optional Qt via aqtinstall
tools/paramix_verify.sh                   # repo checks + clang-format + CMake flag sanity
```

---

## 8. Agent ownership map (from plan §7.1)

A0 architect/integrator (docs/adr, CMakeLists, contracts) · A1 beatgrid core (`src/track/beat*`,
`src/proto/beats.proto`) · A2 beat ML (`src/analyzer/plugins/beatml/`) · A3 stem runtime
(`src/stems/` separator/model) · A4 stem pipeline/DB (`stemcache`, DAO, `res/schema.xml`) ·
A5 audio engine (`src/engine/`, `src/mixer/`) · A6 waveform (`src/waveform/`,
`src/rendergraph/`, `res/shaders/`) · A7 QML/UX (`res/skins/Paramix/`, `res/qml/`, `src/qml/`) ·
A8 QA (tests, benchmarks) · A9 compliance/packaging (`.github/`, `packaging/`, model releases).

Shared single-owner files (route through A0): `CMakeLists.txt`, `res/schema.xml`,
`src/track/track.h`, `src/engine/engine.h`.

---

## 9. Work log

- **2026-11-11** — Repo import committed; dotfiles added; CI workflow authored (hand-written,
  contained bugs). Repo found untracked with only `README.md` tracked before that.
- **2026-11-11** — This session: audited baseline; found Actions stuck; repo made **public**
  (user-approved) to unlock runners; `upstream` remote was briefly added then removed per user
  instruction not to sync upstream.
- **2026-11-11** — `2ef4670`: removed phantom `-DQT6=ON`, dropped `WARNINGS_FATAL`, PEP 668
  fallback for pyyaml, per-job timeouts, ccache on Linux + both macOS jobs, untracked
  `.DS_Store`, ignored `.DS_Store`.
- **2026-11-11** — Audits completed: dotfiles parse and are coherent (clang-format 19.1.3
  compatible, `.gitattributes` does not normalise binary fixtures, `/models/` and `/stems/`
  root-anchored and effective). Pre-commit suite identified as broken (missing 6 root configs,
  `no-commit-to-branch` blocks `main`).
- **2026-11-11** — `55c4b11`: whitespace gate scoped to the tip commit; `actions/checkout@v5`.
- **2026-11-11** — `c41bb96`: `fetch-depth: 2` on the checks job (a shallow `HEAD` has no
  parent, which is why `git show --check HEAD` flagged the entire import).
- **2026-11-11** — Pre-commit repaired: six root configs added, codespell scoped away from
  vendored `res/controllers` and `res/translations`, `no-commit-to-branch` retargeted to
  semver release branches. All fixer hooks verified idempotent (two passes each, zero diffs).
  One real first-party typo remains unsuppressed: "Windos" in `docs/old-context.md` (kept as an
  exact-line exclusion with a FIXME; fix the doc, then delete the entry).
- **2026-11-11** — Local harness landed: `tools/paramix_verify.sh` reports PASS with one SKIP
  (clang-format) on a clean tree; its whitespace check was aligned with CI semantics.
