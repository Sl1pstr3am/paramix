# Baseline local verification

Phase -1 gate: **CI green, formatting enforced**. This harness makes a red CI
run never the first signal by running the same repository checks locally, plus
three checks that catch the common "flag is silently ignored" and "junk got
committed" failures before they reach GitHub.

Machine state for the run below: macOS x86_64 (Darwin 25.6), Apple clang 21,
bash 3.2.57, git 2.55.0, `/usr/bin/python3` 3.9.6. Installed for this run:
`pyyaml 6.0.3`, `clang-format 19.1.3` (pip user site). Not installed: `cmake`,
`ninja`, Qt6.

## What the harness checks

`tools/paramix_verify.sh` is read-only and safe to run daily. It exits `0` when
every check passes or is skipped, `1` when at least one check fails.

| # | Check | Why |
|---|-------|-----|
| a | Merge-conflict markers (`git grep -nE '^(<<<<<<< |======= |>>>>>>> )' -- ':!*.md'`) | Same logic as the CI `checks` job. |
| b | Whitespace errors on the tip commit (`git show --check --format= HEAD`, equivalent to CI's `git diff --check HEAD^ HEAD`) | Same as CI; git exits non-zero on errors. |
| c | YAML + JSON validation with `python3`/`pyyaml` | Same as CI. `lib/` and top-level `build*` dirs are pruned for speed; counts are reported. `pyyaml` is installed into the user site only if missing. |
| d | `clang-format --dry-run --Werror` on changed C++ files vs `origin/main` | Catches formatting drift before CI. Skips with a warning when no `.cpp/.h/.hpp/.cc` files changed or the tool cannot be installed. |
| e | Every `-D...` flag in `.github/workflows/ci.yml` must be declared in `CMakeLists.txt` | CMake silently ignores unknown cache variables. This is the `-DQT6=ON` class of bug. |
| f | Tracked junk: `.DS_Store`, `build/`, `models/`, `stems/` fail; other editor/OS junk warns | Keeps generated/OS files out of the tree. |

The summary table and one-line verdict are printed at the end.

## How to run

```sh
tools/paramix_verify.sh              # run all checks
tools/paramix_verify.sh --no-color   # plain output for logs
```

Install the build/verification tools (dry run by default; nothing is installed
without `--yes`):

```sh
tools/paramix_bootstrap.sh          # print the plan
tools/paramix_bootstrap.sh --yes    # pip install cmake/ninja/clang-format, then configure
```

`paramix_bootstrap.sh` never installs Qt. With `--yes` it installs the small
Python tools and then runs the CMake configure; without Qt that configure fails,
which is expected (see below).

## Result on this machine (2026-11-11)

```
paramix_verify repo=/Users/christianstuart/Documents/paramix

== a. merge-conflict markers ==
no conflict markers

== b. whitespace errors on HEAD ==
no whitespace errors

== c. YAML + JSON validation ==
scanned 36 YAML and 5 JSON files (2 directories skipped)

== d. clang-format on changed C++ files ==
no changed C++ files vs origin/main

== e. CMake -D flag sanity ==
CI -D flags: CMAKE_BUILD_TYPE, QML
declared CMake options: 55

== f. tracked junk ==

== summary ==
CHECK                    RESULT DETAIL
------------------------------------------------------------------
merge-conflict-markers   PASS   clean
whitespace               PASS   clean
yaml-json                PASS   all scanned files valid
clang-format             SKIP   no changed .cpp/.h/.hpp/.cc files vs origin/main
cmake-option-sanity      PASS   all CI -D flags are declared options
tracked-junk             PASS   clean
------------------------------------------------------------------

VERDICT: PASS (1 check(s) skipped)
```

### Defects the harness found (both fixed in `2ef4670`)

1. **`.DS_Store` was tracked.** Check (f) failed on it. It is now untracked and
   ignored via `.gitignore`.
2. **`-DQT6=ON` was a phantom CI flag.** Check (e) reported it as unknown:
   there is no `QT6` option in `CMakeLists.txt` (Qt 6 is mandatory and always
   used), so CMake silently ignored the flag. The flag is now removed from
   `.github/workflows/ci.yml`. `-DWARNINGS_FATAL=ON` was removed at the same
   time (it was a valid option, but off by default upstream).

Re-running the check against the pre-fix `ci.yml` confirms the detection:

```
old ci.yml -D flags: CMAKE_BUILD_TYPE, QML, QT6, WARNINGS_FATAL
unknown: QT6
```

### Known limitation of check (d)

Check (d) formats **whole changed files**. This tree (like upstream Mixxx)
contains legacy lines that plain `clang-format --dry-run --Werror` would
reformat; for example `src/main.cpp:211` and 66 lines in
`src/engine/enginebuffer.cpp`. The repository's own pre-commit hook
(`tools/clang_format.py`) only reformats added/changed lines, so a whole-file
check can report violations on lines you did not touch. If that happens, format
only the changed lines:

```sh
git clang-format origin/main -- <files>
# or, using the repo hook:
python tools/clang_format.py --from-ref origin/main -- <files>
```

The harness pins `clang-format==19.1.3` to match `.pre-commit-config.yaml` and
warns when a different version is found.

## What still requires CI or a Qt install

- **Full CMake configure/build/test** cannot run on this machine yet: `cmake`,
  `ninja` and Qt6 are absent. `tools/paramix_bootstrap.sh --yes` installs
  `cmake`, `ninja` and `clang-format` via pip, but Qt6 is a ~1.5 GB download and
  needs the user's approval. Without it the configure step fails; that is a
  documented gap, not a harness failure.
- **Platform coverage** (Linux, macOS arm64, macOS x86_64, Windows) and the
  compiled test suite only exist in CI.
- **Check (d)** is exercised only when C++ files change vs `origin/main`; on a
  clean `main` it skips.

## Exact CI target commands

Repository checks job (all platforms):

```sh
git grep -nE '^(<<<<<<< |======= |>>>>>>> )' -- ':!*.md'   # must find nothing
git diff --check HEAD^ HEAD                                 # tip commit only
python3 -m pip install --quiet pyyaml || python3 -m pip install --quiet --break-system-packages pyyaml
# + the YAML/JSON validation snippet from .github/workflows/ci.yml
```

The local harness uses `git show --check --format= HEAD` for check (b), which
checks the same tip-commit diff as CI's `git diff --check HEAD^ HEAD`.

Linux x64 (`ubuntu-24.04`):

```sh
tools/debian_buildenv.sh setup
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQML=ON
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

macOS arm64 (`macos-15`):

```sh
source tools/macos_buildenv.sh setup
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQML=ON
cmake --build build --config RelWithDebInfo
```

macOS x86_64 (`macos-15-intel`):

```sh
source tools/macos_buildenv.sh setup
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQML=OFF
cmake --build build --config RelWithDebInfo
```

Windows x64 (`windows-2022`):

```bat
tools\windows_buildenv.bat setup
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQML=ON
cmake --build build --config RelWithDebInfo
```

## Dependency reference

`CONTRIBUTING.md` "Getting Started" lists the platform dependency commands and
costs:

| Platform | Command | Cost |
|----------|---------|------|
| macOS | `source tools/macos_buildenv.sh setup` | ~1.5 GB download, ~3 GB disk |
| Linux (Debian/Ubuntu) | `tools/debian_buildenv.sh setup` | ~200 MB download, ~1 GB disk |
| Windows | `tools\windows_buildenv.bat setup` | ~2.5 GB download, ~9 GB disk |

Core libraries: Qt6 (>= 6.4 for QML), libsndfile, libsoundtouch, libebur128,
librubberband, libfftw3, libopus, libmad, libmodplug, libwavpack,
libchromaprint, PortAudio, HIDAPI, QtKeychain, LV2/Lilv, ffmpeg. The full Linux
package list is in `tools/debian_buildenv.sh`.

Qt6 alone, if you prefer aqtinstall over the prebuilt bundle (not run by the
bootstrap):

```sh
python3 -m pip install --user aqtinstall
aqt install-qt mac desktop 6.7.3 clang_64 -O "$HOME/Qt"
export CMAKE_PREFIX_PATH="$HOME/Qt/6.7.3/clang_64"
# ~1.5 GB download, ~3 GB disk, ~10-20 min
```
