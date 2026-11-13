#!/usr/bin/env python3
"""qml_qt_api_guard.py -- fail when QML uses API newer than the minimum Qt.

Part of the Paramix project (GPL-2.0, based on Mixxx 2.7.0-alpha).

Linux CI builds against Qt 6.4.2 (Ubuntu 24.04, see tools/debian_buildenv.sh) and
CMakeLists.txt gates QML on Qt >= 6.4. QML that uses a property added in a later
Qt parses fine in the editor and on newer platforms, then fails on the Linux job
with

    Cannot assign to non-existent property "..."

which costs a full configure and build to discover. Workstream W2 lost a CI cycle
to exactly that (docs/qml-i18n-a11y-notes.md section 1.2). This tool is the fast
static guard: standard library only, no Qt installation, no build.

Rule classes:

  accessible-member   The `Accessible` attached type is checked against an
                      allowlist of the Qt 6.4 surface. Any member that is not in
                      the Qt 6.4 documentation fails, so the check fails closed:
                      members added in 6.5, 6.8, 6.12, ... are all rejected
                      without the table having to know about them first.
  qt-import-version   `import QtQuick 6.5` (or any versioned `import Qt* x.y`)
                      above the minimum fails. Qt 5 style imports such as
                      `import QtQuick 2.15` are accepted by Qt 6 and are fine.

Additional regex rules can be supplied through the rules JSON (`extra_rules`);
see docs/qml-qt-version-guard.md.

Comments and string literals are masked before matching, so a documented example
inside a comment does not trip the guard.

Usage:
    py tools/qml_qt_api_guard.py
    py tools/qml_qt_api_guard.py --format github
    py tools/qml_qt_api_guard.py res/qml/Deck.qml res/skins
    py tools/qml_qt_api_guard.py --list-rules

Suppression (use sparingly, and say why on the same line):
    Accessible.value: 0.5  // qml-qt-guard: allow accessible-member
    // qml-qt-guard: allow-file qt-import-version

Exit codes:
    0   no violations
    1   at least one violation (all are printed)
    2   usage or configuration error, or no QML files were found
"""

import argparse
import json
import os
import re
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(TOOLS_DIR)
DEFAULT_RULES = os.path.join(TOOLS_DIR, "qml_qt_api_guard_rules.json")

EXIT_CLEAN = 0
EXIT_VIOLATIONS = 1
EXIT_USAGE = 2

# `Accessible.<name>` followed by one of these is a member usage (assignment,
# binding, method call or a further member access). Anything else, such as
# `Accessible.role: Accessible.Slider`, is an enum value and out of scope.
MEMBER_FOLLOWERS = frozenset(":=.(")

ACCESSIBLE_RE = re.compile(r"\bAccessible\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)")
IMPORT_RE = re.compile(
    r"^[ \t]*import[ \t]+([A-Za-z_][A-Za-z0-9_.]*)[ \t]+(\d+)\.(\d+)"
    r"(?:[ \t]+as[ \t]+[A-Za-z_][A-Za-z0-9_]*)?[ \t]*$",
    re.MULTILINE,
)
SUPPRESS_RE = re.compile(r"//\s*qml-qt-guard:\s*(allow-file|allow)\b([^\n]*)")
SUPPRESS_SPLIT_RE = re.compile(r"[,\s]+")


class ConfigError(Exception):
    """Raised for a malformed or incomplete rules file."""


class Diagnostic(object):
    def __init__(self, path, line, column, rule_id, message, severity="error"):
        self.path = path
        self.line = line
        self.column = column
        self.rule_id = rule_id
        self.message = message
        self.severity = severity


def parse_version(value, what):
    if not isinstance(value, str) or not re.match(r"^\d+\.\d+$", value):
        raise ConfigError("%s must be a 'major.minor' string, got %r" % (what, value))
    major, minor = value.split(".")
    return (int(major), int(minor))


def version_text(version):
    return "%d.%d" % version


def mask_source(text):
    """Blank out comments and string literals, keeping length and newlines.

    Returns a string of exactly the same length as `text`. Because newlines are
    preserved, offsets and line numbers found in the masked text can be used to
    report locations in the original file.
    """
    out = list(text)
    length = len(text)
    state = "code"
    quote = ""
    i = 0
    while i < length:
        char = text[i]
        following = text[i + 1] if i + 1 < length else ""
        if state == "code":
            if char == "/" and following == "/":
                out[i] = out[i + 1] = " "
                state = "line_comment"
                i += 2
                continue
            if char == "/" and following == "*":
                out[i] = out[i + 1] = " "
                state = "block_comment"
                i += 2
                continue
            if char in "\"'`":
                out[i] = " "
                state = "string"
                quote = char
                i += 1
                continue
            i += 1
            continue
        if state == "line_comment":
            if char == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
            continue
        if state == "block_comment":
            if char == "*" and following == "/":
                out[i] = out[i + 1] = " "
                state = "code"
                i += 2
                continue
            if char != "\n":
                out[i] = " "
            i += 1
            continue
        # Inside a string literal.
        if char == "\\" and i + 1 < length:
            out[i] = " "
            if text[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
            continue
        if char == quote:
            out[i] = " "
            state = "code"
            i += 1
            continue
        if char == "\n":
            # Unterminated string: recover at the end of the line.
            state = "code"
            i += 1
            continue
        out[i] = " "
        i += 1
    return "".join(out)


def line_column(text, offset):
    line = text.count("\n", 0, offset) + 1
    line_start = text.rfind("\n", 0, offset) + 1
    return line, offset - line_start + 1


def read_suppressions(original_lines):
    """Return (file_wide rule ids, per-line rule ids) from guard comments."""
    file_wide = set()
    per_line = {}
    for index, line in enumerate(original_lines, start=1):
        match = SUPPRESS_RE.search(line)
        if not match:
            continue
        ids = [token for token in SUPPRESS_SPLIT_RE.split(match.group(2).strip()) if token]
        ids = set(ids) if ids else {"*"}
        if match.group(1) == "allow-file":
            file_wide.update(ids)
            continue
        per_line.setdefault(index, set()).update(ids)
        if line.split("//", 1)[0].strip() == "":
            # The comment is on its own line: it also covers the next code line.
            following = index + 1
            while following <= len(original_lines) and not original_lines[following - 1].strip():
                following += 1
            if following <= len(original_lines):
                per_line.setdefault(following, set()).update(ids)
    return file_wide, per_line


class Guard(object):
    def __init__(self, rules):
        if not isinstance(rules, dict):
            raise ConfigError("rules file must contain a JSON object")
        self.minimum = parse_version(rules.get("minimum_qt"), "minimum_qt")
        accessible = rules.get("accessible")
        if not isinstance(accessible, dict):
            raise ConfigError("rules file needs an 'accessible' object")
        properties = accessible.get("properties")
        signals = accessible.get("signals")
        if not isinstance(properties, list) or not all(isinstance(p, str) for p in properties):
            raise ConfigError("accessible.properties must be a list of strings")
        if not isinstance(signals, list) or not all(isinstance(s, str) for s in signals):
            raise ConfigError("accessible.signals must be a list of strings")
        self.accessible_members = set(properties) | set(signals)
        self.signals = set(signals)
        since = accessible.get("since") or {}
        if not isinstance(since, dict):
            raise ConfigError("accessible.since must be an object")
        for name, version in since.items():
            parse_version(version, "accessible.since.%s" % name)
        self.since = since
        imports = rules.get("imports") or {}
        if not isinstance(imports, dict):
            raise ConfigError("imports must be an object")
        self.check_imports = bool(imports.get("enabled", True))
        extra = rules.get("extra_rules") or []
        if not isinstance(extra, list):
            raise ConfigError("extra_rules must be a list")
        self.extra_rules = []
        for entry in extra:
            if not isinstance(entry, dict):
                raise ConfigError("each extra rule must be an object")
            for key in ("id", "pattern", "message"):
                if not isinstance(entry.get(key), str) or not entry[key]:
                    raise ConfigError("extra rule needs a non-empty '%s'" % key)
            try:
                compiled = re.compile(entry["pattern"], re.MULTILINE)
            except re.error as error:
                raise ConfigError("extra rule %s has a bad pattern: %s" % (entry["id"], error))
            self.extra_rules.append((entry, compiled))

    def minimum_text(self):
        return version_text(self.minimum)

    def check_file(self, path):
        try:
            # utf-8-sig strips a leading BOM, which would otherwise hide the
            # first line (and with it, a first-line import) from the matchers.
            with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
                text = handle.read()
        except OSError as error:
            raise ConfigError("cannot read %s: %s" % (path, error))
        masked = mask_source(text)
        diagnostics = []
        diagnostics.extend(self._check_accessible(path, masked))
        if self.check_imports:
            diagnostics.extend(self._check_imports(path, masked))
        diagnostics.extend(self._check_extra(path, masked))
        file_wide, per_line = read_suppressions(text.splitlines())
        kept = []
        suppressed = 0
        for diagnostic in diagnostics:
            allowed = per_line.get(diagnostic.line, set())
            if (
                "*" in file_wide
                or diagnostic.rule_id in file_wide
                or "*" in allowed
                or diagnostic.rule_id in allowed
            ):
                suppressed += 1
                continue
            kept.append(diagnostic)
        return kept, suppressed

    def _check_accessible(self, path, masked):
        diagnostics = []
        for match in ACCESSIBLE_RE.finditer(masked):
            name = match.group(1)
            end = match.end()
            while end < len(masked) and masked[end] in " \t":
                end += 1
            if end >= len(masked) or masked[end] not in MEMBER_FOLLOWERS:
                continue  # An enum value such as Accessible.Slider.
            member = name
            if name.startswith("on") and len(name) > 2:
                candidate = name[2:]
                candidate = candidate[:1].lower() + candidate[1:]
                if candidate in self.signals:
                    member = candidate
            if member in self.accessible_members:
                continue
            line, column = line_column(masked, match.start())
            since = self.since.get(member)
            if since:
                message = (
                    "Accessible.%s requires Qt >= %s, but the minimum supported Qt is %s"
                    % (member, since, self.minimum_text())
                )
            else:
                message = (
                    "Accessible.%s is not part of the Qt %s API; see "
                    "docs/qml-qt-version-guard.md for the rule table"
                    % (member, self.minimum_text())
                )
            diagnostics.append(
                Diagnostic(path, line, column, "accessible-member", message)
            )
        return diagnostics

    def _check_imports(self, path, masked):
        diagnostics = []
        for match in IMPORT_RE.finditer(masked):
            module = match.group(1)
            if not module.startswith("Qt"):
                continue
            version = (int(match.group(2)), int(match.group(3)))
            if version[0] < 6:
                continue  # Qt 5 style imports are accepted by Qt 6.
            if version <= self.minimum:
                continue
            line, column = line_column(masked, match.start())
            diagnostics.append(
                Diagnostic(
                    path,
                    line,
                    column,
                    "qt-import-version",
                    "import %s %s requires Qt >= %s, but the minimum supported Qt is %s"
                    % (module, version_text(version), version_text(version), self.minimum_text()),
                )
            )
        return diagnostics

    def _check_extra(self, path, masked):
        diagnostics = []
        for entry, pattern in self.extra_rules:
            for match in pattern.finditer(masked):
                line, column = line_column(masked, match.start())
                message = entry["message"]
                if "{match}" in message:
                    message = message.replace("{match}", match.group(0))
                diagnostics.append(
                    Diagnostic(path, line, column, entry["id"], message)
                )
        return diagnostics


def load_rules(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise ConfigError("cannot read rules file %s: %s" % (path, error))
    except ValueError as error:
        raise ConfigError("rules file %s is not valid JSON: %s" % (path, error))


def discover_qml_files(root):
    try:
        result = subprocess.run(
            ["git", "-C", root, "ls-files", "-z", "*.qml"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        names = [name for name in result.stdout.decode("utf-8", "replace").split("\0") if name]
        if names:
            return sorted(os.path.join(root, name.replace("/", os.sep)) for name in names)
    except (OSError, subprocess.CalledProcessError):
        pass
    found = []
    for top in ("res", "src"):
        base = os.path.join(root, top)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(name for name in dirnames if name not in (".git", "build"))
            for filename in sorted(filenames):
                if filename.endswith(".qml"):
                    found.append(os.path.join(dirpath, filename))
    return sorted(found)


def relative(path, root):
    try:
        return os.path.relpath(path, root).replace(os.sep, "/")
    except ValueError:
        return path.replace(os.sep, "/")


def format_text(diagnostic, root):
    return "%s:%d:%d: %s: [%s] %s" % (
        relative(diagnostic.path, root),
        diagnostic.line,
        diagnostic.column,
        diagnostic.severity,
        diagnostic.rule_id,
        diagnostic.message,
    )


def escape_annotation(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def format_github(diagnostic, root):
    level = "error" if diagnostic.severity == "error" else "warning"
    return "::%s file=%s,line=%d,col=%d,title=%s::%s" % (
        level,
        escape_annotation(relative(diagnostic.path, root)),
        diagnostic.line,
        diagnostic.column,
        escape_annotation(diagnostic.rule_id),
        escape_annotation(diagnostic.message),
    )


def print_rules(guard):
    print("minimum Qt: %s" % guard.minimum_text())
    print("")
    print("Accessible members allowed in Qt %s:" % guard.minimum_text())
    for name in sorted(guard.accessible_members):
        suffix = " (signal)" if name in guard.signals else ""
        since = guard.since.get(name)
        if since:
            suffix += " [added in Qt %s]" % since
        print("  %s%s" % (name, suffix))
    print("")
    if guard.extra_rules:
        print("Extra rules:")
        for entry, _pattern in guard.extra_rules:
            print("  %s: %s" % (entry["id"], entry["message"]))
    else:
        print("Extra rules: none")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Fail when QML uses API newer than the minimum supported Qt."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="QML files or directories to check (default: every tracked *.qml file)",
    )
    parser.add_argument("--root", default=DEFAULT_ROOT, help="repository root")
    parser.add_argument("--rules", default=DEFAULT_RULES, help="rules JSON file")
    parser.add_argument(
        "--format",
        choices=("text", "github"),
        default="text",
        help="output format; 'github' emits workflow annotations",
    )
    parser.add_argument(
        "--list-rules", action="store_true", help="print the rule table and exit"
    )
    args = parser.parse_args(argv)

    try:
        guard = Guard(load_rules(args.rules))
        if args.list_rules:
            print_rules(guard)
            return EXIT_CLEAN
        if args.paths:
            files = []
            for entry in args.paths:
                if os.path.isdir(entry):
                    files.extend(discover_tree(entry))
                else:
                    files.append(entry)
        else:
            files = discover_qml_files(args.root)
        if not files:
            print(
                "qml-qt-api-guard: no QML files found; check --root and the tracked tree",
                file=sys.stderr,
            )
            return EXIT_USAGE
        formatter = format_github if args.format == "github" else format_text
        violations = 0
        suppressed = 0
        for path in files:
            diagnostics, hidden = guard.check_file(path)
            suppressed += hidden
            for diagnostic in diagnostics:
                print(formatter(diagnostic, args.root))
                violations += 1
        print(
            "%d file(s) scanned against Qt %s: %d violation(s), %d suppressed"
            % (len(files), guard.minimum_text(), violations, suppressed)
        )
        return EXIT_VIOLATIONS if violations else EXIT_CLEAN
    except ConfigError as error:
        print("qml-qt-api-guard: %s" % error, file=sys.stderr)
        return EXIT_USAGE


def discover_tree(directory):
    found = []
    for dirpath, dirnames, filenames in os.walk(directory):
        dirnames[:] = sorted(name for name in dirnames if name not in (".git", "build"))
        for filename in sorted(filenames):
            if filename.endswith(".qml"):
                found.append(os.path.join(dirpath, filename))
    return found


if __name__ == "__main__":
    sys.exit(main())
