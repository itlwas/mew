# Contributing

mew is a pocket language, and keeping it small is a feature, not a bug.

## Ground rules

- **Size matters.** Every new feature must be justified against its cost in source lines and binary bytes. The six files in src/ cover the whole interpreter; keep them small.
- **Modular split.** core.c is allocators/gc/values/collections. parse.c is lexer/parser. eval.c is the tree walker. builtins.c is the standard library. main.c is the CLI. Do not cross wire between files unless required by the public API in mew.h.
- **No magic.** If an edit introduces hidden behaviour, implicit coercions, or non-obvious scoping, it will not be accepted. Code must read the way it runs.
- **ANSI C99 only.** No compiler extensions, no C11 features, no platform specific calls outside of what hosted libc guarantees.
- **Tests before code.** Every bug fix needs a failing test first. Every feature needs a test that documents the behaviour.
- **One page docs.** If a feature cannot be described in one line of POCKET.md, it is probably too big.

## Workflow

```bash
git clone ...
cd pl
make strict       # build with -Werror

# add your test
vi tests/cases/42_myfeature.mew
vi tests/cases/42_myfeature.out

# edit source (pick the right module)
vi src/core.c src/parse.c src/eval.c src/builtins.c

# verify all three layers: unit, repl, smoke
make strict
python tests/run.py
```

## Style

- 4 space indent, no tabs
- lowercase function names with underscores
- structs in PascalCase, macros in UPPERCASE
- braces on same line for functions and control flow
- error messages are lowercase sentences without trailing period

## Commit messages

Subject line imperative and under 70 chars. First sentence states what, body states why when not obvious.

```
fix map iteration to be deterministic

qsort keys by bytes before yielding; required for stable test output.
```

## Reporting bugs

Minimal reproducer first. State platform, compiler, compiler flags, and expected vs actual behaviour. Include the shortest mew program that reproduces the issue.
