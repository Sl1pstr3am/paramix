## What this changes

<!-- One or two sentences. Link the workstream or issue if there is one. -->

## Workstream

<!-- See docs/parallel-work.md. Example: W3 packaging, or "main workstream". -->

## Verification

<!-- Paste what you actually ran. Do not claim tests you did not run. -->

- [ ] `./tools/paramix_verify.sh` passes
- [ ] Changed C++ files are clang-format clean (`clang-format -i <files>`)
- [ ] `ctest` run locally, or "no test impact" with a reason

## Checklist

- [ ] American English in all prose (`color`, `behavior`, `license`, `customize`, `canceled`)
- [ ] No new dependency, or its license is GPL-2.0 compatible
- [ ] No upstream Mixxx sync, no `upstream` remote added
- [ ] Docs updated if behavior or format changed
