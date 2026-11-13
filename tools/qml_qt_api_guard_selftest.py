#!/usr/bin/env python3
"""qml_qt_api_guard_selftest.py -- offline self-test for qml_qt_api_guard.py.

Builds throwaway QML fixtures in a temporary directory and checks the guard's
exit codes, rule ids, locations, suppression handling and the data-driven
`extra_rules` path. Standard library only; no network and no Qt installation.

Usage:
    py tools/qml_qt_api_guard_selftest.py

Exit codes:
    0   every check passed
    1   at least one check failed
"""

import json
import os
import re
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.join(TOOLS_DIR, "qml_qt_api_guard.py")
RULES = os.path.join(TOOLS_DIR, "qml_qt_api_guard_rules.json")

DIAGNOSTIC_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): error: "
    r"\[(?P<rule>[^\]]+)\] (?P<message>.*)$"
)

FIXTURES = {
    "clean.qml": """import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
import QtQml 6.4
import Mixxx 1.0 as Mixxx

Item {
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Play")
    Accessible.checkable: false
    Accessible.onPressAction: {}
    property string note: "Accessible.value is text, not a binding"
}
""",
    "commented.qml": """import QtQuick 2.15

Item {
    // Accessible.value: 0.5
    /* Accessible.maximumValue: 1
       Accessible.announce("hi") */
    property string path: "C:/music/Accessible.value"
}
""",
    "bad_property.qml": """import QtQuick 2.15

Item {
    Accessible.value: 0.5
}
""",
    "bad_unknown.qml": """import QtQuick 2.15

Item {
    Accessible.id: "playButton"
}
""",
    "bad_method.qml": """import QtQuick 2.15

Item {
    Component.onCompleted: Accessible.announce("ready")
}
""",
    "bad_import.qml": """import QtQuick 6.5

Item {
}
""",
    "suppressed.qml": """import QtQuick 2.15

Item {
    Accessible.value: 0.5 // qml-qt-guard: allow accessible-member
    // qml-qt-guard: allow accessible-member
    Accessible.stepSize: 0.1
}
""",
    "suppressed_file.qml": """// qml-qt-guard: allow-file qt-import-version
import QtQuick 6.5

Item {
}
""",
    "extra.qml": """import QtQuick 2.15

Item {
    Component.onCompleted: console.log("debug")
}
""",
}


class Checker(object):
    def __init__(self):
        self.failures = []
        self.checks = 0

    def check(self, name, condition, detail=""):
        self.checks += 1
        if condition:
            print("PASS  %s" % name)
            return
        print("FAIL  %s%s" % (name, (": " + detail) if detail else ""))
        self.failures.append(name)

    def run(self, args):
        result = subprocess.run(
            [sys.executable, GUARD] + args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return (
            result.returncode,
            result.stdout.decode("utf-8", "replace"),
            result.stderr.decode("utf-8", "replace"),
        )

    def diagnostics(self, output):
        found = []
        for line in output.splitlines():
            match = DIAGNOSTIC_RE.match(line)
            if match:
                found.append(
                    {
                        "file": os.path.basename(match.group("file")),
                        "line": int(match.group("line")),
                        "rule": match.group("rule"),
                        "message": match.group("message"),
                    }
                )
        return found


def write_fixtures(directory):
    for name, content in FIXTURES.items():
        with open(os.path.join(directory, name), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)


def line_of(name, needle):
    for index, line in enumerate(FIXTURES[name].splitlines(), start=1):
        if needle in line:
            return index
    raise AssertionError("fixture %s does not contain %r" % (name, needle))


def main():
    checker = Checker()
    temporary = tempfile.mkdtemp(prefix="qml-qt-guard-selftest-")
    write_fixtures(temporary)

    def guard(*args):
        return checker.run(list(args) + ["--root", temporary])

    # 1. Allowed API, Qt 5 style imports, comments and strings.
    code, out, err = guard(
        os.path.join(temporary, "clean.qml"),
        os.path.join(temporary, "commented.qml"),
    )
    checker.check("clean fixtures exit 0", code == 0, "exit %d, stderr=%r" % (code, err))
    checker.check("clean fixtures report two files", "2 file(s) scanned" in out, out.strip())
    checker.check("clean fixtures report no violations", "0 violation(s)" in out, out.strip())

    # 2. Qt 6.5 property, precise location and message.
    code, out, err = guard(os.path.join(temporary, "bad_property.qml"))
    findings = checker.diagnostics(out)
    checker.check("Qt 6.5 property exits 1", code == 1, "exit %d" % code)
    checker.check("Qt 6.5 property is one finding", len(findings) == 1, str(findings))
    if findings:
        finding = findings[0]
        checker.check(
            "Qt 6.5 property reports accessible-member",
            finding["rule"] == "accessible-member",
            finding["rule"],
        )
        checker.check(
            "Qt 6.5 property reports the right line",
            finding["line"] == line_of("bad_property.qml", "Accessible.value"),
            "line %d" % finding["line"],
        )
        checker.check(
            "Qt 6.5 property names the required version",
            "Qt >= 6.5" in finding["message"] and "6.4" in finding["message"],
            finding["message"],
        )

    # 3. Fail-closed: a member the table has never heard of is rejected.
    code, out, err = guard(os.path.join(temporary, "bad_unknown.qml"))
    findings = checker.diagnostics(out)
    checker.check("unknown member exits 1", code == 1, "exit %d" % code)
    checker.check(
        "unknown member is rejected without a version entry",
        len(findings) == 1 and "not part of the Qt 6.4 API" in findings[0]["message"],
        str(findings),
    )

    # 4. Methods carry their own introduction version.
    code, out, err = guard(os.path.join(temporary, "bad_method.qml"))
    findings = checker.diagnostics(out)
    checker.check(
        "Qt 6.8 method is rejected with its version",
        code == 1 and findings and "Qt >= 6.8" in findings[0]["message"],
        str(findings),
    )

    # 5. Versioned Qt imports above the minimum.
    code, out, err = guard(os.path.join(temporary, "bad_import.qml"))
    findings = checker.diagnostics(out)
    checker.check("Qt 6.5 import exits 1", code == 1, "exit %d" % code)
    checker.check(
        "Qt 6.5 import reports qt-import-version",
        len(findings) == 1 and findings[0]["rule"] == "qt-import-version",
        str(findings),
    )

    # 6. Suppression: trailing comment and standalone comment for the next line.
    code, out, err = guard(os.path.join(temporary, "suppressed.qml"))
    checker.check("suppressed findings exit 0", code == 0, "exit %d, out=%s" % (code, out.strip()))
    checker.check("suppressed findings are counted", "2 suppressed" in out, out.strip())

    # 7. File-wide suppression.
    code, out, err = guard(os.path.join(temporary, "suppressed_file.qml"))
    checker.check("allow-file exit 0", code == 0, "exit %d, out=%s" % (code, out.strip()))

    # 8. Data-driven extra rules.
    extra_rules = {
        "minimum_qt": "6.4",
        "accessible": {"properties": ["role"], "signals": [], "since": {}},
        "imports": {"enabled": False},
        "extra_rules": [
            {
                "id": "no-console-log",
                "pattern": r"\bconsole\.log\s*\(",
                "message": "use qDebug() instead of console.log()",
            }
        ],
    }
    rules_path = os.path.join(temporary, "extra-rules.json")
    with open(rules_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(extra_rules, handle)
    code, out, err = guard(os.path.join(temporary, "extra.qml"), "--rules", rules_path)
    findings = checker.diagnostics(out)
    checker.check(
        "extra_rules are applied",
        code == 1 and findings and findings[0]["rule"] == "no-console-log",
        "exit %d, findings=%s, stderr=%r" % (code, findings, err),
    )

    # 9. A UTF-8 BOM must not hide the first line from the matchers.
    bom_path = os.path.join(temporary, "bom.qml")
    with open(bom_path, "w", encoding="utf-8-sig", newline="\n") as handle:
        handle.write('import QtQuick 6.5\n\nItem {\n}\n')
    code, out, err = guard(bom_path)
    findings = checker.diagnostics(out)
    checker.check(
        "UTF-8 BOM does not hide the first line",
        code == 1 and findings and findings[0]["rule"] == "qt-import-version",
        "exit %d, findings=%s" % (code, findings),
    )

    # 10. Rule table is printable, and the shipped table is valid.
    code, out, err = checker.run(["--rules", RULES, "--list-rules"])
    checker.check("--list-rules exits 0", code == 0, "exit %d, stderr=%r" % (code, err))
    checker.check(
        "--list-rules shows the Qt 6.4 surface",
        "minimum Qt: 6.4" in out and "pressAction (signal)" in out,
        out.strip()[:200],
    )
    with open(RULES, "r", encoding="utf-8") as handle:
        shipped = json.load(handle)
    properties = shipped["accessible"]["properties"]
    since = shipped["accessible"]["since"]
    checker.check(
        "shipped table excludes the W2 regression members",
        "value" not in properties and since.get("value") == "6.5",
        "properties=%s since.value=%r" % (properties, since.get("value")),
    )
    checker.check(
        "shipped table keeps Qt 6.4 role assignment valid",
        "role" in properties,
        "properties=%s" % (properties,),
    )

    # 11. A tree with no QML must not pass silently.
    empty = tempfile.mkdtemp(prefix="qml-qt-guard-empty-")
    code, out, err = checker.run(["--root", empty])
    checker.check("empty tree exits 2", code == 2, "exit %d, out=%s" % (code, out.strip()))
    checker.check(
        "empty tree explains itself",
        "no QML files found" in err,
        err.strip(),
    )

    print("")
    print("%d check(s), %d failure(s)" % (checker.checks, len(checker.failures)))
    return 1 if checker.failures else 0


if __name__ == "__main__":
    sys.exit(main())
