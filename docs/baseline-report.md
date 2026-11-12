# Phase −1 Baseline Report

**Date:** 2026-11-11
**Scope:** repository baseline only. No feature code (beatgrid, stems, waveform, UI) is part of
this phase.
**Plan reference:** `docs/paramix-stem-beatgrid-ui-plan.md` §7.3, Phase −1.
**Exit gate:** "CI green, formatting enforced" on Windows x64, Linux x64, macOS arm64 and
macOS x86_64.

---

## 1. Repository state

| Item | Value |
|---|---|
| Branch | `main` (linear history; this fork commits directly to `main`) |
| Visibility | Public (changed from private to obtain hosted runners; approved by the owner) |
| Upstream sync | None. `upstream` remote deliberately absent — the imported tree is the source of truth |
| Tracked files | ~6,035 |
| Tracked size | ~180 MB (largest blobs are legitimate stem test fixtures under `src/test/stems/`) |

Baseline commits in order:

| Commit | Purpose |
|---|---|
| `96f541b` | Initial commit |
| `8c67ba8` | Mixxx import |
| `0901e4b` | Planning documents |
| `2ef4670` | CI flags, timeouts, ccache, `.DS_Store` hygiene |
| `55c4b11` | Whitespace gate scoped to the pushed commit; checkout v5 |
| `c41bb96` | Shallow-clone fix for the whitespace gate (`fetch-depth: 2`) |
| `18c817b` | Pre-commit repair, local verification harness, agent context |

---

## 2. What was broken and what fixed it

### 2.1 CI workflow

| Defect | Effect | Fix |
|---|---|---|
| `-DQT6=ON` passed to CMake | Phantom flag: there is no `QT6` option in this tree (Qt 6 is mandatory) | Removed |
| `-DWARNINGS_FATAL=ON` | Would gate the baseline on warnings that default to OFF (`CMakeLists.txt:2210`) | Removed |
| `git show --check HEAD` | Flagged the imported tree's pre-existing trailing whitespace on every run | Replaced with `git diff --check HEAD^ HEAD` |
| `fetch-depth: 1` | Shallow `HEAD` has no parent, so `git show` treated the whole snapshot as an addition | `fetch-depth: 2` on the checks job |
| `pip install pyyaml` | Fails on externally managed Python (PEP 668) | Fallback to `--break-system-packages` |
| No job timeouts | A hung build could burn runner time indefinitely | 10/75/90 min per job, later `macos-x64` → 120 (see §4) |
| No compiler cache | Every run rebuilt from scratch | ccache restore/save on Linux and both macOS jobs |
| Node 20 deprecation | Warning noise | `actions/checkout@v5` |

Environment propagation for the macOS/Windows build environments needed no fix: the buildenv
scripts write `MIXXX_VCPKG_ROOT`, `VCPKG_TARGET_TRIPLET`, `BUILDENV_*` and `CMAKE_GENERATOR` to
`$GITHUB_ENV` themselves, so isolated workflow steps see them.

### 2.2 Pre-commit

`.pre-commit-config.yaml` referenced six configuration files that were never imported into the
tree, so five hooks could not run, and `no-commit-to-branch` blocked `main` — meaning every
commit was blocked if hooks were installed.

| File | Purpose |
|---|---|
| `.gersemirc` | `indent: 2`, matching the existing `cmake/*.cmake` style |
| `.cmakelintrc` | Filters only the rules that fire on vendored `Find*.cmake` modules |
| `.prettierrc.yaml` | Tree style for YAML plus the `*.d.ts` overrides used by controller typings |
| `.markdownlint-cli2.yaml` | Disables only rules the existing documents already violate |
| `.codespellignore` | 23 verified false positives (identifiers, vendor/person names, DSP vocabulary) |
| `.codespellignorelines` | One exact-line exclusion, with a FIXME, for a misspelled "Windows" in `docs/old-context.md` |

All fixer hooks were run twice with zero resulting changes (idempotent). `pre-commit
validate-config` passes.

Known local-only gap: `black` and `flake8` require Python ≥ 3.10; this workstation has 3.9.6.
Their environments build correctly on the CI runner's Python 3.12.

### 2.3 Hygiene

- `.DS_Store` was tracked; now untracked and ignored.
- `/models/` and `/stems/` are root-anchored ignores so downloaded weights and generated caches
  can never be committed, while `src/test/stems/` fixtures stay tracked.
- `.gitattributes` verified not to normalize binary fixtures (`*.stem.mp4`, `*.wav`, `*.flac`).
- Heuristic credential scan (tokens, private keys) across the tree, excluding vendored `lib/`
  and test data: no findings.

---

## 3. Local verification path

This workstation has no CMake, Ninja, Qt or Homebrew (MacPorts 2.12.6 is present), so build
evidence must come from CI. Two scripts close the gap for everything that does not need Qt:

- `tools/paramix_verify.sh` — merge-conflict markers, tip-commit whitespace, YAML/JSON
  validation, clang-format on changed C++ files, CMake `-D` flag sanity against `CMakeLists.txt`,
  tracked-junk detection. Prints PASS/SKIP/FAIL per check and exits non-zero on failure.
- `tools/paramix_bootstrap.sh` — dry-run by default; `--yes` installs cmake, ninja and
  clang-format via pip and attempts a configure. Qt is never installed automatically.

Last local run on a clean tree: **PASS**, one SKIP (no changed C++ files vs `origin/main`).

---

## 4. CI results

Run [`43100769404`](https://github.com/christianstuart/paramix/actions/runs/43100769404) on
`18c817b`, all jobs green.

| Target | Job | Result | Wall time |
|---|---|---|---|
| Repository checks | conflict markers, whitespace, YAML/JSON | green | 41 s |
| Linux x64 | build + `ctest` | green — **1 302/1 302 tests passed** | 22 m 35 s |
| macOS arm64 | build (QML on) | green | 39 m 49 s |
| macOS x86_64 | build (QML off) | green | 1 h 15 m 46 s |
| Windows x64 | build (QML on) | green | — |

Notes from the run:

- **Build time:** arm64 26 m 16 s (1 154 Ninja edges) versus x86_64 60 m 44 s (981 edges). The Intel
  runner is ~2.3× slower per edge, which is why its timeout was raised to 120 minutes — at 90 it
  had only 14 minutes of headroom.
- **Configure:** 12 m 52 s (arm64) and 13 m 54 s (x86_64), dominated by downloading a ~3.7 GB
  prebuilt dependency bundle from `downloads.mixxx.org`. SHA-256 pins verified correct on both.
- **ccache:** both macOS jobs and the Linux job missed on this run (no cache existed) and saved
  ~0.4–0.5 GB each. Caches are keyed on `src/**`, `CMakeLists.txt` and `cmake/**`, so the next
  docs-only push should hit them.
- **Skipped tests:** 1 340 tests are registered, 1 302 ran, 38 are upstream `DISABLED_` cases
  (SoftTakeover, SoundTouch/Rubberband toggles, 24 stem AAC/ALAC parameters). None were excluded
  by this repo.
- **Headless:** no `xvfb` is needed — `src/test/main.cpp` sets `QT_QPA_PLATFORM=offscreen` itself.
- **macOS runs no tests.** Both macOS jobs build only, matching upstream. "Green" there means
  "compiles", not "passes".

---

## 5. Known gaps carried into Phase 0

1. **macOS x86_64 builds with `-DQML=OFF`.** This contradicts locked decision 6 (QML skin is the
   default UI) and is inherited from upstream, which also sets QML off for Intel. The x64 prebuilt
   environment does ship `qtdeclarative`, so enabling it is likely a one-line change — but it adds
   ~11 minutes to the slowest job, so it should land as its own scoped commit together with the
   120-minute timeout, not bundled with a fix.
2. **Formatting enforcement.** Added to the `checks` job in the closing commit: clang-format 19.1.3
   on changed C++ files only (`--diff-filter=ACMR`, vendored and generated paths excluded), because
   the imported tree is not wholly formatted and a whole-tree check cannot pass.
3. **No local Qt toolchain.** Reproducing a desktop build locally requires
   `sudo port install qt6` (or the aqtinstall path in the bootstrap script) plus 30–60 min of
   compile time.
4. **Misspelled "Windows"** in `docs/old-context.md` is excluded from codespell by exact line;
   fix the document and delete the exclusion.
5. **`black`/`flake8` hooks** cannot run under the local Python 3.9; they work on CI.

---

## 6. Outcome and next phase

Phase −1 is complete. All four build targets are green on one commit, the repository checks are
enforced, formatting is gated in CI, and a local verification harness exists that mirrors the CI
checks without requiring a full build.

Phase 0 (discovery and spikes), per plan §7.3:

1. ADR-001 inference runtime and model, ADR-002 cache format, ADR-003 grid format,
   ADR-004 UI/customization, ADR-005 model distribution.
2. Spikes: `htdemucs_ft` ONNX benchmark on available hardware; Beat This! F-measure against
   Queen Mary on a labeled fixture set; 8-channel FLAC write/read through the existing
   `EngineDeck` 8-channel path; 4-lane waveform geometry at 60 fps; a GitHub Releases
   model-download dry run.
3. Audit what the imported tree already provides before writing new code. Preliminary evidence
   suggests a stem playback layer already exists (`src/track/steminfo*`, `src/sources/soundsourcestem*`,
   `src/widget/wstemlabel*`, `src/qml/qmlstemsmodel*`, and ALAC/AAC `.stem.mp4` test fixtures), so
   Phase 1 may be separation and export rather than a stem mixer built from scratch. Confirm before
   planning that work.
