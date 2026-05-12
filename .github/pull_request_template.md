## What

<!-- one sentence summary of the change -->

## Why

<!-- motivation: bug number, user report, design goal -->

## How

<!-- which files changed and why; reference mew.h invariants if relevant -->

## Tests

- [ ] added or updated a test in `tests/cases/`
- [ ] `make strict` passes (`-Werror`)
- [ ] `python tests/run.py` passes all three layers (unit, repl, smoke)

## Checklist

- [ ] no new files above what the change strictly requires
- [ ] POCKET.md updated if user-visible syntax or builtin changed
- [ ] CHANGELOG.md entry under Unreleased
- [ ] no new compiler warnings under `-Wall -Wextra -Wpedantic`
