#!/usr/bin/env bash
#
# paramix_bootstrap.sh -- install the local Phase -1 verification/build tools.
#
# Dry run by default: it prints the plan and changes nothing. Pass --yes to
# install the Python tools (cmake, ninja, clang-format) into the user site and
# then attempt the CMake configure step.
#
# Qt6 is deliberately NOT installed by this script. It is a ~1.5 GB download
# and needs the user's approval. The exact aqtinstall commands are printed
# below so the step can be run manually.
#
# Usage:
#   tools/paramix_bootstrap.sh          # print the plan (no changes)
#   tools/paramix_bootstrap.sh --yes    # install tools, then configure CMake
#
# Environment:
#   CLANG_FORMAT_VERSION   clang-format version to install
#                          (default: 19.1.3, the .pre-commit-config.yaml pin)

set -euo pipefail

SCRIPT_SOURCE="${BASH_SOURCE[0]:-$0}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$SCRIPT_SOURCE")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

CLANG_FORMAT_VERSION="${CLANG_FORMAT_VERSION:-19.1.3}"

YES=0
case "${1:-}" in
  "") ;;
  --yes|-y) YES=1 ;;
  *)
    echo "error: unknown argument: $1" >&2
    echo "usage: $0 [--yes]" >&2
    exit 2
    ;;
esac

# Directory pip installs console scripts into for the current user.
user_site_bin() {
  printf '%s/bin' "$(python3 -m site --user-base 2>/dev/null)"
}

print_plan() {
  local user_bin
  user_bin="$(user_site_bin)"

  echo "paramix bootstrap"
  echo "  repo:  $REPO_ROOT"
  if [[ "$YES" -eq 1 ]]; then
    echo "  mode:  install"
  else
    echo "  mode:  dry run (no changes)"
  fi
  echo
  echo "Step 1: Python tools (user site)"
  echo "  python3 -m pip install --user cmake ninja clang-format==$CLANG_FORMAT_VERSION"
  echo "  installs into: $user_bin"
  echo "  add to PATH:   export PATH=\"$user_bin:\$PATH\""
  echo
  echo "Step 2: Qt6 (optional, NOT run by this script)"
  echo "  The canonical macOS dependency bundle (Qt6 + audio libraries) is:"
  echo "    source tools/macos_buildenv.sh setup"
  echo "    # ~1.5 GB download, ~3 GB disk; exports CMAKE_PREFIX_PATH/toolchain"
  echo "  Or install Qt6 alone with aqtinstall:"
  echo "    python3 -m pip install --user aqtinstall"
  echo "    aqt install-qt mac desktop 6.7.3 clang_64 -O \"\$HOME/Qt\""
  echo "    export CMAKE_PREFIX_PATH=\"\$HOME/Qt/6.7.3/clang_64\""
  echo "    # ~1.5 GB download, ~3 GB disk, ~10-20 min"
  echo "  Mixxx requires Qt 6.4 or newer for QML (enforced in CMakeLists.txt)."
  echo
  echo "Step 3: CMake configure (only with --yes)"
  echo "  cmake -S . -B build -DQML=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo"
  echo
  echo "Expected dependencies (see CONTRIBUTING.md 'Getting Started'):"
  echo "  macOS:   source tools/macos_buildenv.sh setup   (~1.5 GB download, ~3 GB disk)"
  echo "  Linux:   tools/debian_buildenv.sh setup         (~200 MB download, ~1 GB disk)"
  echo "  Windows: tools\\windows_buildenv.bat setup       (~2.5 GB download, ~9 GB disk)"
  echo "  Core libraries: Qt6 (>= 6.4), libsndfile, libsoundtouch, libebur128,"
  echo "  librubberband, libfftw3, libopus, libmad, libmodplug, libwavpack,"
  echo "  libchromaprint, PortAudio, HIDAPI, QtKeychain, LV2/Lilv, ffmpeg."
  echo "  The full package list is in tools/debian_buildenv.sh."
}

install_tools() {
  local user_bin
  user_bin="$(user_site_bin)"

  local have_cmake=0
  local have_ninja=0
  local have_clang_format=0
  if command -v cmake >/dev/null 2>&1; then
    have_cmake=1
  fi
  if command -v ninja >/dev/null 2>&1; then
    have_ninja=1
  fi
  if [[ -x "$user_bin/clang-format" ]] || command -v clang-format >/dev/null 2>&1; then
    have_clang_format=1
  fi

  if [[ "$have_cmake" -eq 1 && "$have_ninja" -eq 1 && "$have_clang_format" -eq 1 ]]; then
    echo "All tools already installed; skipping pip install."
  else
    echo "Installing Python tools into $user_bin ..."
    # PEP 668 (externally managed environment) can refuse --user installs;
    # retry the way the CI workflow does.
    if ! python3 -m pip install --user --quiet cmake ninja "clang-format==$CLANG_FORMAT_VERSION"; then
      echo "retrying with --break-system-packages..."
      python3 -m pip install --user --quiet --break-system-packages \
        cmake ninja "clang-format==$CLANG_FORMAT_VERSION"
    fi
  fi

  # Make the tools available for the rest of this script. This only affects
  # the script's own shell, not the caller.
  export PATH="$user_bin:$PATH"

  echo "Installed versions:"
  cmake --version | head -n 1
  ninja --version | head -n 1
  clang-format --version | head -n 1
}

configure_cmake() {
  echo
  echo "Configuring CMake (Qt6 must be available or this will fail)..."
  if cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DQML=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo; then
    echo "CMake configure succeeded."
    echo "Next: cmake --build build --parallel \$(sysctl -n hw.ncpu)"
  else
    echo
    echo "CMake configure failed. That is expected without Qt6:"
    echo "  * source tools/macos_buildenv.sh setup  (prebuilt dependency bundle), or"
    echo "  * install Qt6 with aqtinstall and set CMAKE_PREFIX_PATH (see Step 2)."
    echo "This is a documented gap, not a bootstrap failure."
  fi
}

print_plan

if [[ "$YES" -ne 1 ]]; then
  echo
  echo "Dry run: nothing was installed. Re-run with --yes to apply."
  exit 0
fi

echo
install_tools
configure_cmake
