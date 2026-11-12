# Parallel work: coordination guide for additional contributors

This document tells a second team of agents and developers (starting with Bradley's) what they can
work on **concurrently** with the main Paramix workstream without colliding with it.

Read §1 and §8 before starting anything. Claim a workstream in §3 before writing code.

---

## 1. Ground rules

These are not style preferences; breaking them costs a cancelled CI run or a broken build.

1. **Never sync with upstream Mixxx.** No `git remote add upstream`, no merges or cherry-picks from
   `mixxxdj/mixxx`. The imported tree in this repository is the source of truth. The `upstream`
   remote was deliberately removed.
2. **Do not push to `main` — it is enforced, not just convention.** `main` requires a pull request
   and all five CI jobs (`Repository checks`, `Linux x64`, `Windows x64`, `macOS arm64`,
   `macOS x86_64`). Force pushes and branch deletion are blocked, and unresolved review
   conversations block merging. The workflow uses `concurrency: cancel-in-progress: true`, so a
   push to `main` cancels every build that is already running — including other people's.

   The intended loop for every contributor, including the owner:

   ```bash
   git switch -c bradley/<topic>
   # ... work, commit ...
   git push -u origin bradley/<topic>
   gh pr create --fill --base main
   gh pr merge --squash --auto   # merges once all five checks pass
   ```

3. **Branch naming:** `bradley/<topic>`, for example `bradley/controller-mappings` or
   `bradley/a11y-qml`.
4. **Do not edit these files** without asking the integrator first:
   - `docs/agent-context.md` — the main workstream's handoff log; single owner.
   - `.github/workflows/ci.yml` — CI definitions; single owner. Add *new* workflow files instead.
   - `CMakeLists.txt`, `src/CMakeLists.txt`, `res/schema.xml`, `src/track/track.h`,
     `src/engine/engine.h` — shared structural files where concurrent edits conflict badly.
5. **American English** in all prose: `color`, `behavior`, `license`, `customize`, `analyze`,
   `canceled`, `labeled`, `localized`, `normalize`, `artifact`, `center`, `optimize`, `modeling`,
   `gray`. Prose only — do not rename existing code identifiers.
6. **Use `christianstuart`** in every new link, badge, remote and `gh` command. The old owner name
   redirects but must not be written again.
7. **GPL-2.0 compatibility.** Do not add a dependency without checking. Apache-2.0, GPL-3.0 and
   AGPL-3.0 code cannot be linked into this GPL-2.0-only project. Ask before adding anything.
8. **README.md is deliberately aspirational.** The owner asked for it to describe the target
   feature set as if it already ships. Do not "fix" it by deleting features, and do not treat it as
   a specification. `docs/paramix-stem-beatgrid-ui-plan.md` is the specification.
9. **Real-time audio rules still apply.** The engine callback must not allocate, lock, or block.
   See `AGENTS.md`.

---

## 2. What already exists — do not rebuild it

A frequent waste of effort here is building something the imported tree already has.

| Capability | Status | Evidence |
|---|---|---|
| Per-stem waveform rendering (stacked lanes and overlay) | **Exists** | `src/waveform/renderers/allshader/waveformrendererstem.cpp`, QML wrapper `src/qml/qmlwaveformrenderer.h:638` |
| Per-stem waveform data | **Exists** | `WaveformData::stems[mixxx::kMaxSupportedStems]` in `src/waveform/waveform.h:31`, filled by `src/analyzer/analyzerwaveform.cpp` |
| Per-stem gain/mute/solo/FX, 8-channel deck path | **Exists** | `src/engine/channels/enginedeck.cpp`, `[ChannelN_Stem1..4]` control groups |
| Stem control UI | **Exists** | `res/qml/StemKnob.qml`, `res/qml/Deck/HotcueAndStem.qml`, LateNight stem controls |
| Reading NI `.stem.mp4` (ALAC and AAC) | **Exists** | `src/sources/soundsourcestem.cpp`, `src/track/steminfoimporter.cpp`, fixtures in `src/test/stems/` |
| Downbeat / bar / meter in the beatgrid | **Absent** | `src/track/beats.h` has markers plus one tempo; no downbeat field |
| Stem **separation** (any model) | **Absent** | no inference runtime anywhere in the tree |
| `.stem.mp4` **writing/export** | **Absent** | no exporter; the path is read-only |

---

## 3. Claimable workstreams

Each workstream below is chosen because it touches files the main workstream is not editing. Claim it
by adding your name to the table in §4 in your first pull request.

### W1 — Controller mappings for stem mixing

- **Scope:** add or update mappings (144 MIDI, 18 HID today) that expose the existing
  `[ChannelN_Stem1..4]` `volume`, `mute`, `solo` and QuickFX controls, including the four-stem
  views on Traktor S4 MK3-class hardware.
- **Files:** `res/controllers/**` only.
- **Why it is safe:** XML/JS mapping files, no C++.
- **Done when:** mapping loads without XML errors, controls move the documented ControlObjects,
  and `res/controllers` files pass the YAML/XML checks.

### W2 — Accessibility and localization of the QML UI

- **Scope:** the QML skin has effectively no `Accessible.*`/`accessibleName` and **zero** `qsTr()`
  calls, while the widget UI is fully translated. Add accessible names/roles and wrap user-visible
  strings in `qsTr()`.
- **Files:** `res/qml/**`, excluding a small set the main workstream will touch when it builds the
  Paramix skin (announce before you start; see §4).
- **Why it is safe:** additive changes; no engine or library code.
- **Done when:** `lupdate`/translation extraction reports the new strings, no new hardcoded
  user-visible text, and screens pass a keyboard-only navigation check.

### W3 — Packaging and installers

- **Scope:** finish the cross-platform packaging story: Windows installer, macOS universal app
  bundle, Linux AppImage/Flatpak/deb, desktop entry and icons for the Paramix name.
- **Files:** `packaging/**`, `res/linux/**`, `res/images/**` (new assets), `tools/*buildenv*`
  scripts. Do not touch `CMakeLists.txt`; request changes through the integrator.
- **Why it is safe:** no overlap with feature code.
- **Done when:** an installer or bundle can be produced from a clean checkout, and the product name,
  version `0.1` and "based on Mixxx 2.7.0" appear in the About metadata.

### W4 — Test infrastructure and golden audio fixtures

- **Scope:** the beatgrid has **no** end-to-end detection test, no fixture with a golden grid and no
  benchmark. Build that harness, plus a synthetic 4-band mix fixture for separation plumbing.
- **Files:** `src/test/**` (new files), `tools/**` (new scripts).
- **Why it is safe:** additive test code. Do not modify existing tests to make them pass.
- **Done when:** a golden-fixture test runs under `ctest`, fails meaningfully when the grid is
  perturbed, and needs no network access.

### W5 — Model hosting scaffolding (`paramix-models`)

- **Scope:** the model manifest format, SHA-256 + MIT license metadata, and a download/verify tool.
  Models are **download-only** from a dedicated repository; nothing is committed here.
- **Files:** `tools/**` (new script), `docs/**` (new document). Not `src/`.
- **Why it is safe:** no C++ and no CI changes.
- **Done when:** the tool can fetch a local test manifest, verify checksums, resume an interrupted
  download and refuse a mismatched hash.

### W6 — Additional CI workflows

- **Scope:** new, separate workflow files, for example a nightly sanitizer run or a packaging smoke
  test. **Do not edit `.github/workflows/ci.yml`.**
- **Why it is safe:** new files only.
- **Done when:** the workflow is green on a branch and does not run on every push (keep runner cost
  sane).

---

## 4. Ownership map

Update the Owner column in your first PR. `main` means the primary workstream; do not edit those
paths.

| Area | Paths | Owner | Notes |
|---|---|---|---|
| Handoff log | `docs/agent-context.md` | main | Single owner |
| Program plan | `docs/paramix-stem-beatgrid-ui-plan.md` | main | Ask before editing |
| CI definitions | `.github/workflows/ci.yml` | main | Add new files instead |
| Beatgrid / downbeats | `src/track/beats*`, `src/analyzer/**` | main | Planned work, collision-prone |
| Stem separation | `src/engine/**`, new separator code | main | Planned work |
| `.stem.mp4` export | `src/sources/**`, `src/track/steminfo*` | main | Planned work |
| Paramix QML skin | `res/qml/**` (new skin) | main | Coordinate before editing shared QML |
| Controller mappings | `res/controllers/**` | **free** | W1 |
| QML accessibility/localization | `res/qml/**` (existing files) | **free** | W2 — announce first |
| Packaging | `packaging/**`, `res/linux/**` | **free** | W3 |
| Tests and fixtures | `src/test/**`, `tools/**` (new) | **free** | W4 |
| Model hosting tools | `tools/**` (new), `docs/**` (new) | **free** | W5 |
| New workflows | `.github/workflows/*.yml` (new) | **free** | W6 |

---

## 5. Verification before opening a pull request

```bash
# Repository checks: conflict markers, whitespace, YAML/JSON, clang-format
./tools/paramix_verify.sh

# Local formatting fix for changed C++ files
clang-format -i path/to/changed.cpp

# Pre-commit hooks (some need Python 3.10+ and will not run locally on 3.9)
pre-commit run --files <changed files>
```

CI runs five jobs: `checks`, `Linux x64`, `Windows x64`, `macOS arm64`, `macOS x86_64`.

- **The clang-format gate checks changed C++ files only.** The imported tree is not wholly
  formatted, so a whole-tree check cannot pass; touching a legacy file means formatting that file.
- **Linux runs 1 302 tests; 38 are upstream `DISABLED_` cases** — that is expected, not a failure.
- **macOS jobs build but do not run tests**, matching upstream. Green there means "compiles".
- **No `xvfb` is needed:** `src/test/main.cpp` sets `QT_QPA_PLATFORM=offscreen` itself.
- A full clean build takes ~20–40 minutes; a push that touches no file under `src/**`,
  `CMakeLists.txt` or `cmake/**` hits the ccache and finishes in minutes. Keep documentation-only
  changes in their own PR.

---

## 6. Traps worth knowing

1. **The 3.7 GB build environment can truncate.** `file(DOWNLOAD)` in `CMakeLists.txt` has no
   `STATUS` check, so a cut-off transfer appears as `SHA256 checksum mismatch`. The workflow now
   pre-fetches with `curl --retry` and verifies; if you see that error locally, delete
   `<BUILDENV_BASEPATH>/<name>.zip` and retry.
2. **Do not create a local commit without setting an identity.** This repository has no global git
   identity on the owner's machine.
3. **`docs/old-context.md` is a verbatim historical transcript.** It keeps British spelling and the
   pre-rename username. Do not edit it.
4. **Do not commit build outputs, `.DS_Store`, or model files.** Models live in `paramix-models`.
5. **`README.rtf`-style artifacts:** if a Markdown file is ever saved as RTF by an editor, the
   repository check will not catch it. Verify with `file README.md` if a doc suddenly looks wrong.
6. **macOS x86_64 builds with `-DQML=OFF`**, so a QML-only change is not verified on that target.
   Do not assume a green x86_64 job means your QML loads.

---

## 7. Coordination protocol

1. Read this document and §2 before choosing work.
2. Claim a workstream by editing §4 in a small pull request, or open an issue titled
   `workstream: <name>`.
3. Announce anything that touches a **shared** path in §4 before you start, even if the Owner column
   says "free".
4. Keep pull requests small and scoped to one workstream. A PR that mixes packaging, tests and QML
   will conflict with everything.
5. If CI fails for a reason unrelated to your change (runner download, ccache restore), re-run the
   job before investigating your diff.

---

## 8. Scope discipline

Two things are deliberately **not** on this list because they are the main workstream's core
deliverables: **stem separation** and **downbeat-aware beatgrids**. Both require design decisions
that are still open (inference runtime, model choice, grid format) and touch the most
collision-prone files in the tree. If you want to contribute there, start with the ADR or the spike
rather than the implementation.
