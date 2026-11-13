#!/usr/bin/env bash
#
# paramix_verify.sh -- local Phase -1 verification gate for the Paramix fork.
#
# This mirrors the repository checks in .github/workflows/ci.yml and adds
# local-only checks that catch problems before a CI run goes red:
#
#   a. merge-conflict markers (same logic as CI)
#   b. whitespace errors on HEAD (git show --check)
#   c. YAML + JSON validation (pyyaml; lib/ and build dirs skipped for speed)
#   d. clang-format on changed C++ files (vs origin/main)
#   e. CMake -D flag sanity: every CI flag must be a declared CMake option
#   f. tracked-junk check (.DS_Store, build/, models/, stems/)
#
# The script is read-only and safe to run daily. It exits 0 when every check
# passes or is skipped, and 1 when at least one check fails. Skips and warnings
# are not failures.
#
# Usage:
#   tools/paramix_verify.sh
#   tools/paramix_verify.sh --no-color
#
# Environment:
#   CLANG_FORMAT_VERSION   clang-format version to install if none is found
#                          (default: 19.1.3, the .pre-commit-config.yaml pin)

set -euo pipefail

# ---------------------------------------------------------------------------
# Locate the repository root. The script may be invoked from any directory and
# never changes the caller's working directory (all git commands use -C).
# ---------------------------------------------------------------------------
SCRIPT_SOURCE="${BASH_SOURCE[0]:-$0}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$SCRIPT_SOURCE")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

CLANG_FORMAT_VERSION="${CLANG_FORMAT_VERSION:-19.1.3}"

# ---------------------------------------------------------------------------
# Output helpers. Colors are disabled when stdout is not a TTY or when
# --no-color is passed, so the output is safe to paste into logs.
# ---------------------------------------------------------------------------
USE_COLOR=0
if [[ "${1:-}" != "--no-color" ]] && [[ -t 1 ]]; then
  USE_COLOR=1
fi

if [[ "$USE_COLOR" -eq 1 ]]; then
  C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_BLUE=$'\033[34m'
  C_BOLD=$'\033[1m'
  C_RESET=$'\033[0m'
else
  C_RED=""
  C_GREEN=""
  C_YELLOW=""
  C_BLUE=""
  C_BOLD=""
  C_RESET=""
fi

section() {
  printf '\n%s== %s ==%s\n' "$C_BOLD$C_BLUE" "$1" "$C_RESET"
}

# ---------------------------------------------------------------------------
# Check bookkeeping. Results are PASS, FAIL or SKIP. Only FAIL is fatal.
# ---------------------------------------------------------------------------
CHECK_NAMES=()
CHECK_RESULTS=()
CHECK_DETAILS=()

record() {
  CHECK_NAMES+=("$1")
  CHECK_RESULTS+=("$2")
  CHECK_DETAILS+=("${3:-}")
}

result_color() {
  case "$1" in
    PASS) printf '%s' "$C_GREEN" ;;
    FAIL) printf '%s' "$C_RED" ;;
    SKIP) printf '%s' "$C_YELLOW" ;;
    *) printf '%s' "" ;;
  esac
}

# ---------------------------------------------------------------------------
# a. Merge-conflict markers. Mirrors the CI step exactly; git grep exits 0 when
#    it finds matches and 1 when it finds none.
# ---------------------------------------------------------------------------
check_conflict_markers() {
  section "a. merge-conflict markers"
  local out status
  set +e
  out="$(git -C "$REPO_ROOT" grep -nE '^(<<<<<<< |======= |>>>>>>> )' -- ':!*.md' 2>&1)"
  status=$?
  set -e
  if [[ "$status" -eq 0 ]]; then
    printf '%s\n' "$out"
    record "merge-conflict-markers" FAIL "conflict markers found (see above)"
  elif [[ "$status" -eq 1 ]]; then
    echo "no conflict markers"
    record "merge-conflict-markers" PASS "clean"
  else
    printf '%s\n' "$out"
    record "merge-conflict-markers" FAIL "git grep failed (status $status)"
  fi
}

# ---------------------------------------------------------------------------
# b. Whitespace errors in the tip commit. Mirrors the CI gate exactly: only the
#    pushed change is checked, never the whole imported tree (which has
#    pre-existing trailing whitespace). `git show --check HEAD` must NOT be used
#    here: in a shallow clone HEAD has no parent, so git treats the entire
#    snapshot as an addition and flags every file.
# ---------------------------------------------------------------------------
check_whitespace() {
  section "b. whitespace errors in the tip commit"
  local out status
  if ! git -C "$REPO_ROOT" rev-parse --verify HEAD^ >/dev/null 2>&1; then
    echo "root commit or shallow history: skipping"
    record "whitespace" SKIP "no parent commit to diff"
    return
  fi
  set +e
  out="$(git -C "$REPO_ROOT" diff --check HEAD^ HEAD 2>&1)"
  status=$?
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "no whitespace errors"
    record "whitespace" PASS "clean"
  else
    printf '%s\n' "$out"
    record "whitespace" FAIL "git diff --check HEAD^ HEAD reported errors"
  fi
}

# ---------------------------------------------------------------------------
# c. YAML + JSON validation of tracked/working-tree files. lib/ is vendored and
#    build dirs are generated, so both are pruned from the walk for speed.
# ---------------------------------------------------------------------------
check_yaml_json() {
  section "c. YAML + JSON validation"
  if ! python3 -c 'import yaml' >/dev/null 2>&1; then
    echo "pyyaml not importable; installing into the user site..."
    # PEP 668 (externally managed environment) can refuse --user installs;
    # retry the way the CI workflow does.
    if ! python3 -m pip install --user --quiet pyyaml &&
      ! python3 -m pip install --user --quiet --break-system-packages pyyaml; then
      record "yaml-json" SKIP "pyyaml unavailable (pip install failed)"
      return
    fi
  fi

  local out status
  set +e
  out="$(python3 - "$REPO_ROOT" <<'PY'
import json
import os
import sys

import yaml

root = sys.argv[1]
errors = []
counts = {"yaml": 0, "json": 0, "skipped": 0}

# Walk the working tree, pruning .git, __pycache__, the top-level lib/ tree
# and any top-level build directory. lib/ is large and vendored; CI validates
# it, the local harness skips it for speed.
for dirpath, dirnames, filenames in os.walk(root):
    rel = os.path.relpath(dirpath, root)
    depth = 0 if rel == "." else rel.count(os.sep) + 1
    kept = []
    for name in dirnames:
        if name in (".git", "__pycache__"):
            counts["skipped"] += 1
            continue
        if depth == 0 and (name == "lib" or name.startswith("build")):
            counts["skipped"] += 1
            continue
        kept.append(name)
    dirnames[:] = kept

    for name in filenames:
        path = os.path.join(dirpath, name)
        suffix = os.path.splitext(name)[1].lower()
        if suffix in (".yml", ".yaml"):
            counts["yaml"] += 1
            try:
                with open(path, "rb") as handle:
                    yaml.safe_load(handle)
            except Exception as exc:  # report every parse failure
                errors.append(f"{path}: {exc}")
        elif suffix == ".json":
            counts["json"] += 1
            try:
                with open(path, "rb") as handle:
                    json.loads(handle.read().decode("utf-8"))
            except Exception as exc:  # report every parse failure
                errors.append(f"{path}: {exc}")

print(
    f"scanned {counts['yaml']} YAML and {counts['json']} JSON files "
    f"({counts['skipped']} directories skipped)"
)
if errors:
    print()
    print("\n".join(errors))
    sys.exit(1)
PY
)"
  status=$?
  set -e
  printf '%s\n' "$out"
  if [[ "$status" -eq 0 ]]; then
    record "yaml-json" PASS "all scanned files valid"
  else
    record "yaml-json" FAIL "invalid YAML/JSON (see above)"
  fi
}

# ---------------------------------------------------------------------------
# d. clang-format on changed C++ files. Changed files are computed against
#    origin/main (falling back to HEAD when origin/main is unavailable).
# ---------------------------------------------------------------------------
check_clang_format() {
  section "d. clang-format on changed C++ files"

  local ref="origin/main"
  if ! git -C "$REPO_ROOT" rev-parse --verify --quiet "$ref" >/dev/null; then
    echo "warning: $ref not found; comparing against HEAD instead"
    ref="HEAD"
  fi

  local changed
  changed="$(git -C "$REPO_ROOT" diff --name-only "$ref" 2>/dev/null)" || changed=""

  local files=()
  local name
  while IFS= read -r name; do
    [[ -n "$name" ]] || continue
    case "$name" in
      lib/*) continue ;;
      *.cpp|*.h|*.hpp|*.cc) files+=("$name") ;;
    esac
  done <<< "$changed"

  if [[ "${#files[@]}" -eq 0 ]]; then
    echo "no changed C++ files vs $ref"
    record "clang-format" SKIP "no changed .cpp/.h/.hpp/.cc files vs $ref"
    return
  fi
  echo "changed C++ files: ${#files[@]}"

  # Discover clang-format: PATH first, then the pip user-site bin directory.
  local clang_format=""
  if command -v clang-format >/dev/null 2>&1; then
    clang_format="$(command -v clang-format)"
  else
    local user_bin
    user_bin="$(python3 -m site --user-base 2>/dev/null)/bin/clang-format"
    if [[ -x "$user_bin" ]]; then
      clang_format="$user_bin"
    fi
  fi

  if [[ -z "$clang_format" ]]; then
    echo "clang-format not found; installing clang-format==$CLANG_FORMAT_VERSION ..."
    if ! python3 -m pip install --user --quiet "clang-format==$CLANG_FORMAT_VERSION" &&
      ! python3 -m pip install --user --quiet --break-system-packages "clang-format==$CLANG_FORMAT_VERSION"; then
      record "clang-format" SKIP "clang-format not installed and pip install failed"
      return
    fi
    local user_bin
    user_bin="$(python3 -m site --user-base 2>/dev/null)/bin/clang-format"
    if [[ -x "$user_bin" ]]; then
      clang_format="$user_bin"
    fi
  fi

  if [[ -z "$clang_format" ]]; then
    record "clang-format" SKIP "clang-format not installed and pip install failed"
    return
  fi

  local cf_version
  cf_version="$("$clang_format" --version 2>/dev/null | head -n 1 || true)"
  echo "using $clang_format ($cf_version)"
  if [[ "$cf_version" != *"$CLANG_FORMAT_VERSION"* ]]; then
    echo "warning: .pre-commit-config.yaml pins clang-format==$CLANG_FORMAT_VERSION;"
    echo "         a different version may report different violations."
  fi

  # Run from the repo root in a subshell so clang-format finds .clang-format;
  # the caller's working directory is unaffected.
  local out status
  set +e
  out="$(cd "$REPO_ROOT" && "$clang_format" --dry-run --Werror --style=file "${files[@]}" 2>&1)"
  status=$?
  set -e

  if [[ "$status" -eq 0 ]]; then
    record "clang-format" PASS "${#files[@]} changed C++ file(s) formatted"
  else
    printf '%s\n' "$out"
    echo
    echo "auto-fix with:"
    printf '  cd %s && %s -i --style=file %s\n' "$REPO_ROOT" "$clang_format" "${files[*]}"
    echo "or, to format only the lines changed vs $ref:"
    printf '  git clang-format %s -- %s\n' "$ref" "${files[*]}"
    record "clang-format" FAIL "formatting violations in changed C++ files"
  fi
}

# ---------------------------------------------------------------------------
# e. CMake -D flag sanity. Every -D flag used by CI must be declared as an
#    option in CMakeLists.txt. CMake silently ignores unknown cache variables,
#    so a typo or a stale flag (the -DQT6=ON class of bug) is otherwise only
#    discovered when the build behaves unexpectedly.
# ---------------------------------------------------------------------------
check_cmake_options() {
  section "e. CMake -D flag sanity"
  local ci="$REPO_ROOT/.github/workflows/ci.yml"
  local cml="$REPO_ROOT/CMakeLists.txt"
  if [[ ! -f "$ci" || ! -f "$cml" ]]; then
    record "cmake-option-sanity" SKIP "ci.yml or CMakeLists.txt not found"
    return
  fi

  local out status
  set +e
  out="$(python3 - "$ci" "$cml" <<'PY'
import re
import sys

ci_path, cmake_path = sys.argv[1], sys.argv[2]
with open(ci_path, encoding="utf-8") as handle:
    ci_text = handle.read()
with open(cmake_path, encoding="utf-8") as handle:
    cmake_text = handle.read()

# Every -DNAME=value used by the CI workflow.
used = sorted(set(re.findall(r"-D([A-Za-z0-9_]+)=", ci_text)))

# Options declared by CMakeLists.txt. option()/cmake_dependent_option() may
# put the name on the line after the opening parenthesis; default_option() is
# the macro wrapper used for most optional features.
declared = set(
    re.findall(
        r"^\s*(?:option|cmake_dependent_option|default_option)\s*\(\s*([A-Za-z0-9_]+)",
        cmake_text,
        re.MULTILINE,
    )
)

# CMAKE_* variables are built in to CMake (CMAKE_BUILD_TYPE, CMAKE_PREFIX_PATH,
# ...) and are not expected to be declared as options.
unknown = [name for name in used if name not in declared and not name.startswith("CMAKE_")]

print("CI -D flags: " + ", ".join(used))
print(f"declared CMake options: {len(declared)}")
if unknown:
    print("UNKNOWN (not declared in CMakeLists.txt): " + ", ".join(unknown))
    sys.exit(1)
PY
)"
  status=$?
  set -e
  printf '%s\n' "$out"
  if [[ "$status" -eq 0 ]]; then
    record "cmake-option-sanity" PASS "all CI -D flags are declared options"
  else
    echo
    echo "Remove the unknown -D flags from .github/workflows/ci.yml or declare"
    echo "them in CMakeLists.txt. Unknown flags are silently ignored by CMake."
    record "cmake-option-sanity" FAIL "unknown -D flags (see above)"
  fi
}

# ---------------------------------------------------------------------------
# f. Tracked-junk check. .DS_Store and build/models/stems contents are hard
#    failures; other editor/OS junk is a warning.
# ---------------------------------------------------------------------------
check_tracked_junk() {
  section "f. tracked junk"
  local listing
  listing="$(git -C "$REPO_ROOT" ls-files)"

  local hard=()
  local soft=()
  local name
  while IFS= read -r name; do
    [[ -n "$name" ]] || continue
    case "$name" in
      .DS_Store|*/.DS_Store|build/*|models/*|stems/*)
        hard+=("$name")
        ;;
      *.pyc|__pycache__/*|*.log|*/.idea/*|.idea/*|*/.vscode/*|.vscode/*|*.suo|*.user|*.swp|*~|CMakeCache.txt|*/CMakeCache.txt)
        soft+=("$name")
        ;;
    esac
  done <<< "$listing"

  if [[ "${#hard[@]}" -gt 0 ]]; then
    echo "tracked junk (fail):"
    printf '  %s\n' "${hard[@]}"
  fi
  if [[ "${#soft[@]}" -gt 0 ]]; then
    echo "tracked junk (warn):"
    printf '  %s\n' "${soft[@]}"
  fi

  if [[ "${#hard[@]}" -gt 0 ]]; then
    record "tracked-junk" FAIL "${#hard[@]} tracked junk path(s) must be removed"
  elif [[ "${#soft[@]}" -gt 0 ]]; then
    record "tracked-junk" PASS "${#soft[@]} tracked junk warning(s)"
  else
    record "tracked-junk" PASS "clean"
  fi
}

# ---------------------------------------------------------------------------
# Summary table and verdict.
# ---------------------------------------------------------------------------
print_summary() {
  section "summary"
  printf '%-24s %-6s %s\n' "CHECK" "RESULT" "DETAIL"
  printf '%s\n' "------------------------------------------------------------------"
  local i color
  for i in "${!CHECK_NAMES[@]}"; do
    color="$(result_color "${CHECK_RESULTS[$i]}")"
    printf '%-24s %s%-6s%s %s\n' \
      "${CHECK_NAMES[$i]}" "$color" "${CHECK_RESULTS[$i]}" "$C_RESET" "${CHECK_DETAILS[$i]}"
  done
  printf '%s\n' "------------------------------------------------------------------"
}

verdict() {
  local failures=0
  local skips=0
  local result
  for result in "${CHECK_RESULTS[@]}"; do
    case "$result" in
      FAIL) failures=$((failures + 1)) ;;
      SKIP) skips=$((skips + 1)) ;;
    esac
  done
  echo
  if [[ "$failures" -gt 0 ]]; then
    printf '%sVERDICT: FAIL%s (%d check(s) failed, %d skipped)\n' \
      "$C_RED$C_BOLD" "$C_RESET" "$failures" "$skips"
    return 1
  fi
  printf '%sVERDICT: PASS%s (%d check(s) skipped)\n' \
    "$C_GREEN$C_BOLD" "$C_RESET" "$skips"
  return 0
}

main() {
  printf '%sparamix_verify%s repo=%s\n' "$C_BOLD" "$C_RESET" "$REPO_ROOT"
  check_conflict_markers
  check_whitespace
  check_yaml_json
  check_clang_format
  check_cmake_options
  check_tracked_junk
  print_summary
  if verdict; then
    exit 0
  else
    exit 1
  fi
}

main "$@"
