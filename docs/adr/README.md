# Architecture decision records

This directory holds the decisions that are expensive to reverse: formats, dependencies,
process boundaries and storage layouts. Each record states the context, the decision, the
alternatives that were rejected, and the consequences a future contributor inherits.

## Conventions

- **File name:** `NNNN-kebab-title.md`, numbered in creation order. The number is permanent;
  do not renumber when a record is superseded.
- **Status:** `Proposed` → `Accepted` → `Superseded by ADR-NNN`. An accepted record is not
  rewritten. Change the decision by writing a new record that supersedes it, and leave the
  old one intact.
- **Scope:** one decision per record. If a record needs two independent decisions, split it.
- **Evidence:** cite file paths and line numbers for claims about the current tree. Link the
  audit or spike that produced the numbers rather than restating them.
- **American English** in all prose, per `docs/agent-context.md` §1.1.

## Index

| ADR | Title | Status |
|---|---|---|
| [0002](0002-stem-cache.md) | Stem cache format and lifecycle | Proposed |
| [0003](0003-grid-format.md) | Beatgrid format for bar and downbeat information | Proposed |

ADR-001 (inference runtime), ADR-004 (UI and customization) and ADR-005 (model
distribution) are planned in
[`docs/paramix-stem-beatgrid-ui-plan.md`](../paramix-stem-beatgrid-ui-plan.md) and are not
written yet.
