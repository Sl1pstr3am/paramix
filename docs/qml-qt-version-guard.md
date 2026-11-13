# QML minimum-Qt guard (W6)

A static check that fails when QML uses an API newer than the project's minimum
supported Qt. It runs in `.github/workflows/qml-qt-api-guard.yml` and takes
seconds, because it does not install Qt and does not build anything.

```bash
python tools/qml_qt_api_guard.py             # check the tracked *.qml tree
python tools/qml_qt_api_guard.py --list-rules
python tools/qml_qt_api_guard_selftest.py     # prove the guard itself works
```

Exit codes: `0` clean, `1` violations, `2` bad rules file, unreadable input, or
no QML files found (a broken scan must not look green).

## 1. Why this exists

Linux CI builds against **Qt 6.4.2** (Ubuntu 24.04, `tools/debian_buildenv.sh`)
and `CMakeLists.txt` gates QML on `Qt6_VERSION >= 6.4`. A property added in a
later Qt is accepted by editors, by Qt Creator's newer kits and by the macOS and
Windows CI images, so it survives review. On the Linux job it fails at component
load with:

```text
Cannot assign to non-existent property "value"
```

That is a full configure plus build before the error appears, and the whole
component (often the startup screen) fails, not just the binding.

This is not hypothetical: workstream W2 added `Accessible.value` and friends,
which are Qt 6.5+, and only found out on the Linux job. The removal is recorded
in [`docs/qml-i18n-a11y-notes.md`](qml-i18n-a11y-notes.md) section 1.2, together
with the Qt 6.4 constraint that now applies to every QML change.

The guard is the fast feedback loop for that class of mistake. It is a
**static approximation**: a green guard is not a substitute for the Linux build,
it only removes the slowest way to discover the most common regression.

## 2. What it checks

### 2.1 `accessible-member` (fail-closed allowlist)

`tools/qml_qt_api_guard_rules.json` lists the properties and signals that the
`Accessible` attached type has in **Qt 6.4**, taken from
<https://doc.qt.io/qt-6.4/qml-qtquick-accessible.html>. Any other member used as
an attached property, handler or method call fails:

- `Accessible.value`, `minimumValue`, `maximumValue`, `stepSize` - Qt 6.5
- `Accessible.announce(...)` - Qt 6.8
- `Accessible.labelFor`, `labelledBy` - Qt 6.10
- `Accessible.expandable`, `expanded` - Qt 6.12
- `Accessible.id`, `showOnScreenAction` and anything else not in the Qt 6.4 list

The check is an allowlist, not a denylist, so **it fails closed**: a member added
in a future Qt is rejected even though this document has never heard of it. That
is deliberate. The alternative - enumerating known-bad members - silently passes
every API the table has not learned yet.

`Accessible.role: Accessible.Slider` is not a member usage: the right-hand side
is an enum value, so it is ignored. Comments and string literals are masked
before matching, so documentation such as the comments in
`res/qml/Mixxx/Controls/Knob.qml` does not trip the guard.

### 2.2 `qt-import-version`

A versioned import above the minimum fails:

```qml
import QtQuick 6.5   // fails: requires Qt >= 6.5
import QtQuick 2.15  // fine: Qt 5 style imports are accepted by Qt 6
import Mixxx 1.0     // not a Qt module: ignored
```

### 2.3 `extra_rules`

`extra_rules` in the rules JSON adds plain regex checks for other APIs that are
not covered by the two classes above. An entry has an `id`, a `pattern` and a
`message`; there is no per-rule version gate, because the global minimum is what
the guard enforces. Put the documented "introduced in" version in the message so
the finding explains itself.

## 3. What it does not check

- **Role enum values.** `Accessible.role: Accessible.Markdown` is a value, not a
  member, and role enums are not versioned in the table. If that becomes a real
  risk, add a rule with a role allowlist; do not guess.
- **QML APIs outside `Accessible`** unless they are covered by `extra_rules`.
  Adding a class of checks is a data change plus, if needed, a new regex rule.
- **Anything C++ or widget based.** The guard reads `.qml` files only.
- **Runtime behavior.** A component can be Qt 6.4-clean and still wrong.
- **Dynamic access.** `Accessible["va" + "lue"]` and similar are not detected.

## 4. Extending the rule table

Rules live in `tools/qml_qt_api_guard_rules.json`. Every entry must cite a
source in `sources`, and the provenance rule is strict: **do not add a member or
a version from memory.** Use the documentation for `minimum_qt` for the
allowlist and the current documentation's "This property was introduced in Qt
X.Y" note for the `since` map. If the documentation is silent, leave the
version out; an unknown member is still rejected, just with a generic message.

Example of an `extra_rules` entry. This one is a project policy rule; a
version-based rule has the same shape but states the documented "introduced in"
version in its message:

```json
{
  "id": "no-console-log",
  "pattern": "\\bconsole\\.log\\s*\\(",
  "message": "use qDebug() or the QML logger instead of console.log()"
}
```

Then run `python tools/qml_qt_api_guard_selftest.py` and
`python tools/qml_qt_api_guard.py`. The self-test exercises the guard's own
logic and the shipped table, so a table edit that breaks an invariant (for
example, accidentally allowing `value`) fails it.

### Raising the minimum Qt

1. Confirm the Linux CI image actually ships the new Qt, and say so in the PR.
2. Update `minimum_qt`, rebuild the allowlist from the new version's docs, and
   prune `since` entries that are no longer relevant.
3. Update the `Qt6_VERSION` gate in `CMakeLists.txt`. That file is
   integrator-owned (`docs/parallel-work.md` section 1) — request the change, do
   not edit it from a contributor branch.

## 5. Suppression

Prefer fixing the QML. When a violation is intentional and temporary, suppress
it on the spot and say why:

```qml
Accessible.value: root.value  // qml-qt-guard: allow accessible-member
```

```qml
// qml-qt-guard: allow accessible-member
Accessible.stepSize: 0.1
```

```qml
// qml-qt-guard: allow-file qt-import-version
```

`allow` with no rule ids suppresses every rule on that line (and, when the
comment is on its own line, on the next line). Suppressed findings are counted in
the summary, so they stay visible. There is no baseline file and no global
disable switch.

## 6. Where it runs

`.github/workflows/qml-qt-api-guard.yml` runs

- on pull requests that touch `res/qml/**`, `res/skins/**`, `res/controllers/**`,
  the guard itself or this document, and
- weekly on a schedule, to catch drift the path filter cannot see.

It is a separate workflow by design: `ci.yml` is single-owner
(`docs/parallel-work.md` section 1), and adding new workflow files is the
sanctioned way for a contributor to add CI. It does not run on every push, and
it never modifies the required checks.
