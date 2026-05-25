# mew

[![CI](https://github.com/itlwas/mew/actions/workflows/ci.yml/badge.svg)](https://github.com/itlwas/mew/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-99-blue.svg)](https://en.wikipedia.org/wiki/C99)

A tiny scripting language in ANSI C99. Small, readable, no magic.

```
# the canonical example in one line
if lower(a) == lower(b) then print("same name") end
```

## Contents

- [Why another language](#why)
- [At a glance](#at-a-glance)
- [Build](#build)
- [Getting started](#getting-started)
- [Language tour](#language-tour)
- [Standard library](#standard-library)
- [Comparison](#comparison)
- [Documentation](#documentation)
- [Tests](#tests)
- [Design decisions](#design-decisions)
- [Non goals](#non-goals)
- [A personal note](#a-personal-note)
- [License](#license)

## Why

Sometimes you want to compare two filenames, parse a CSV, or count words in a log, and you do not want to pull in Python or write C. You want a small, obvious tool that reads the way it runs. That is mew.

## At a glance

| Property | Value |
|----------|-------|
| Implementation | 6 ANSI C99 files in `src/` (`mew.h` + `core.c` + `parse.c` + `eval.c` + `builtins.c` + `main.c`) |
| Source size | ~3644 lines, ~125 KB |
| Binary size | 147 KB with `-Os -s`, 163 KB with `-O2` (x64 mingw) |
| Types | 7: `nil`, `bool`, `number`, `string`, `list`, `map`, `fn` |
| Keywords | 17 |
| Builtins | 54 |
| Documentation | one A6 page or a 10 KB JSON spec |
| Dependencies | hosted libc only |
| GC | mark-and-sweep with temporary-value protection |
| Closures | yes, with environment capture |
| Recursion limit | 512 frames |

## Build

```bash
# any C99 compiler, all files in one command
cc -O2 -std=c99 -o mew src/core.c src/parse.c src/eval.c src/builtins.c src/main.c

# Windows through mingw
gcc -O2 -std=c99 -o mew.exe src\core.c src\parse.c src\eval.c src\builtins.c src\main.c

# smallest binary
cc -Os -std=c99 -s -o mew src/core.c src/parse.c src/eval.c src/builtins.c src/main.c

# through the Makefile
make           # default build
make tiny      # smallest binary
make strict    # -Werror with extra warnings
make test      # run the full test suite
```

The source compiles clean with `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror`.

### Source layout

| file | role | lines |
|------|------|-------|
| `src/mew.h` | public types and prototypes | 285 |
| `src/core.c` | globals, errors, gc, strings, lists, maps, env, printing | 990 |
| `src/parse.c` | lexer and recursive-descent parser | 723 |
| `src/eval.c` | tree-walking interpreter and REPL helper | 438 |
| `src/builtins.c` | 54 built-in functions | 1056 |
| `src/main.c` | entry point, REPL, CLI | 152 |

## Getting started

```bash
./mew hello.mew         # run a script
./mew -e "print(2+2)"   # run an expression
./mew                   # interactive REPL (multi-line input)
./mew script.mew a b c  # arguments reachable via args()
./mew -v  /  -h         # version and help
```

## Language tour

### Variables and types
```
x = 42                          # number (always float64)
name = "mew"                    # string
flags = [true, false]           # list
user = {"name": "a", "age": 1}  # map
empty = nil

print(type(x))                  # number
```

### Control flow
```
if x > 10 then print("big") else print("small") end

for i = 1 to 5 do print(i) end       # ascending, inclusive
for x in [10, 20, 30] do print(x) end

i = 0
while i < 10 do
    i = i + 1
    if i == 5 then break end
end
```

### Functions and closures
```
fn square(x) return x * x end

double = fn(x) return x * 2 end      # anonymous fn

fn counter()                         # closure captures n
    n = 0
    return fn() n = n + 1 return n end
end
```

### Strings
```
s = "  Hello, World!  "
print(trim(s))                       # "Hello, World!"
print(lower(s))
print(replace(s, "World", "mew"))
print(split("a,b,c", ","))           # ["a", "b", "c"]
print(starts_with("mew.exe", "mew")) # true
print(format("%s is %d", "mew", 3))  # "mew is 3"
```

### Lists and maps
```
xs = [3, 1, 4, 1, 5, 9, 2, 6]
print(sort(xs))                              # [1,1,2,3,4,5,6,9]
print(sort(xs, fn(a,b) return b-a end))      # custom comparator

m = {"b": 2, "a": 1, "c": 3}
print(keys(m))                               # always ["a","b","c"]
print(values(m))                             # [1, 2, 3]
```

### Bytes
```
print(ord("A"))          # 65
print(chr(65))           # "A"
```

### Files
```
for line in lines("log.txt") do
    if contains(line, "ERROR") then print(line) end
end
write_file("out.txt", "done\n")
```

### Modules
```
# util.mew
fn greet(name) return "hi " .. name end

# main.mew
load("util.mew")
print(greet("world"))
```

### Errors
```
fn divide(a, b)
    if b == 0 then error("div by zero") end
    return a / b
end
assert(divide(10, 2) == 5, "math is broken")
```

## Standard library

54 builtins, no external modules:

- **io**: `print`, `write`, `repr`, `read`
- **types**: `len`, `type`, `str`, `num`
- **strings**: `upper`, `lower`, `trim`, `repeat`, `starts_with`, `ends_with`, `contains`, `split`, `join`, `find`, `slice`, `replace`
- **lists**: `push`, `pop`, `append`, `reverse`, `sort`
- **maps**: `keys`, `values`, `has`, `del`
- **files**: `read_file`, `write_file`, `lines`
- **env**: `args`, `exit`, `getenv`, `clock`, `time`, `sleep`
- **math**: `abs`, `min`, `max`, `floor`, `ceil`, `round`, `sqrt`, `pow`, `rand`, `seed`
- **bytes**: `chr`, `ord`
- **formatting**: `format` (supports `%s %d %x %f`, width and precision)
- **errors**: `error`, `assert`
- **modules**: `load`

Full reference in [docs/POCKET.md](docs/POCKET.md) and [docs/SPEC.json](docs/SPEC.json).

## Comparison

| | mew | Lua 5.4 | Python 3 |
|-|-----|---------|----------|
| binary (KB) | ~150 | ~230 | ~5000 |
| syntax | Lua-like | Lua | indent |
| types | 7 | 8 | 10+ |
| keywords | 17 | 22 | 35 |
| GC | yes | yes | yes |
| closures | yes | yes | yes |
| exceptions | no | pcall | yes |
| classes | no | metatables | yes |
| numbers | float64 | double+int | many |
| zero-deps build | yes | yes | no |

mew is deliberately smaller than Lua in exchange for transparency and a pocket-sized reference.

## Documentation

The full reference fits on one page:

- **[docs/POCKET.md](docs/POCKET.md)** - human-readable cheat sheet, ~4 KB, A6 format
- **[docs/SPEC.json](docs/SPEC.json)** - machine-readable spec with grammar and translation tables for Python, C and shell, ~10 KB. Designed for LLM prompts

## Tests

```bash
python tests/run.py           # all three layers: unit, repl, smoke
python tests/run.py unit      # 66 unit tests: arithmetic, loops, closures, GC, errors, audit regressions
python tests/run.py repl      # 6 REPL scenarios: multi-line input, error recovery
python tests/run.py smoke     # 9 examples end-to-end
```

## Examples

The [examples/](examples/) directory holds realistic scripts:

| file | what it does |
|------|--------------|
| `compare_names.mew` | case-insensitive filename comparison |
| `csv.mew` | CSV parser with header-row into map |
| `wordcount.mew` | word frequency in text |
| `grep.mew` | substring grep utility |
| `hexdump.mew` | classic hex dump of a file |
| `calc.mew` | expression calculator with precedence |
| `todo.mew` | CLI todo list |
| `stats.mew` | min/max/mean/median/stddev from stdin or file |
| `fib.mew` | micro-benchmark |

## Design decisions

**Tree-walking interpreter, not bytecode.** For short scripts, compiling to bytecode costs more than it saves. A tree walker is simpler, smaller, and easier to port.

**Mark-and-sweep GC, not reference counting.** RC cannot handle closure-environment cycles, and dropping closures would mean dropping functional composition.

**String keys in map.** Generic keys need `hash` and `equal` for every type, plus 200 lines of code. String keys cover 99% of scripting tasks.

**`..` for concatenation, not `+`.** No surprises like `"5" + 3`. Here it is explicit: `+` is numbers only, `..` is strings or lists only.

**Assignment scopes by default.** No `local`/`global`. Reads walk up the scope chain; writes go to the binding found, or define a new one in the current scope.

**Deterministic map iteration.** Keys are always sorted lexicographically. Output is reproducible, tests stay stable.

**for-to only ascends.** If `from > to`, the loop does not execute. For descending, use `while`. Fewer surprises.

**AST lives for the whole process.** Functions keep pointers directly into the AST, so freeing it while the interpreter runs is impossible. That is the price of simplicity and the absence of use-after-free.

**Stack overflow protection.** Recursion depth is capped at 512 frames with a readable error instead of a segfault.

**Errors abort the program.** No exceptions. To recover, return `nil`. The REPL recovers from errors via `setjmp`/`longjmp`.

**Parser nesting limit.** Deep structural nesting (`if`/`while`/`for`/`fn`) and long prefix chains (`not not ...`, `- - -`) are capped at 256 levels with a clear error, so an adversarial `.mew` cannot overflow the C stack.

## Non goals

Deliberate omissions, not oversights:

- classes and inheritance (use map)
- exceptions and try/catch (return `nil`, call `error`)
- threads and coroutines (single threaded)
- regular expressions (`find`, `split`, `contains`, `replace`)
- integer arithmetic (everything is float64, precise up to 2^53)
- sockets and HTTP (out of scope for the base interpreter)

Lua and Python exist for all of these. mew stays small on purpose.

## A personal note

I made mew because I wanted it. Half the time I need a script, all I am doing is comparing two filenames or counting words in a log, and pulling in Python just for that always felt heavy. Writing the same thing in C is worse. So this is the tool I wanted to have on hand.

Two rules from the start. It should run on whatever machine you happen to be at, even the kind of hardware that makes you want to cry. And it should read exactly the way it runs. If you see a line, that is what happens. No magic, no clever desugaring, no surprise behaviour.

The whole reference fits on one page on purpose. A small LLM, even a 1B model, can read it once and translate code from a bigger language into mew with no extra context. The smaller the spec, the higher the accuracy floor.

Before you commit to anything, a fair warning. mew is not Python. There is no roadmap. I fix critical bugs when I feel like fixing them. The language grows by personal taste, not by feature request. I make no promises about backwards compatibility, release cadence, or anything else. If you want to build something serious on top of it, the license is MIT. Clone it, fork it, do what you want with it.

If the language fits how you think, good. If not, Lua and Python are right there and they are good at what they do.

## License

MIT. See [LICENSE](LICENSE).
