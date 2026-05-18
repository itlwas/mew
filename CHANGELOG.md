# Changelog

## 1.0.11 - eleventh audit pass: control-flow rigour, hash flooding

Eleventh audit. Two correctness defects in control-flow plus a
small batch of low-severity hardening.

### Correctness fixes
- F-205: `break` outside any while/for used to silently set
  g_break = 1 and abandon the rest of the enclosing block. it is
  now a parse-time error matching how lua and python handle it.
  caught at parse time via a static g_loop_depth counter that
  parse_while/parse_for increment around the body and parse_fn_body
  resets to 0 inside a function body.
- F-206: `return` outside any function used to set g_ret = 1 and
  silently swallow the rest of the program. it is now a parse-time
  error tracked through g_fn_depth.
- 40_break_isolation rewritten: it used to demonstrate the
  silent-failure behaviour with `fn stray_break() break end` which
  is now a parse error. the new version exercises the same
  invariant (break stays scoped to its function) using a function-
  local loop.

### Hardening
- F-204: str_hash now uses a per-process random seed so a malicious
  script that places attacker-controlled data into map keys cannot
  exploit FNV-1a determinism to engineer hash-flooding collisions
  and turn O(1) map operations into O(n^2). seed is folded from
  time(NULL) and an address-randomized stack pointer.
- F-201: format_num clamps snprintf return values before passing
  them to sb_putn so a libc reporting truncation could not push a
  negative or oversized length into a copy.
- F-213: parse_assign_or_expr now free_node()s both sides of the
  attempted assignment before raising "invalid assignment target",
  closing a 50-byte-per-error leak that accumulated in REPL
  sessions with frequent syntax mistakes.
- F-212: bi_format vpush/vpops the StrObj produced by value_to_str
  for `%s` so a hypothetical future gc trigger inside sb_putn
  cannot reclaim its data buffer mid-copy. unwind path uses
  vrestore via a baseline g_vsp captured at entry.

### Tests
- 71 unit (up from 69), 6 repl, 9 smoke = 86 green.
- New: err_break_outside, err_return_outside.
- Fuzz harness: 10200 mutation runs in 60 sec, 0 crashes.
- gcc 15.2 -fanalyzer on every source file: 0 warnings.

## 1.0.10 - tenth audit pass: AST chain leak and format error leak

Tenth audit. Two real memory-leak findings closed plus several
small robustness wins.

### Memory leaks closed
- F-101: `g_ast_chain` accumulated linearly. Every successful
  `run_source` (each REPL line) and every successful `bi_load`
  appended a tree of Node-objects that nothing released. After
  1000 REPL lines or 1000 `load()` calls the residual heap was
  measured in megabytes. Fix: a new `ast_sweep()` is called from
  `run_source` and `bi_load` between top-level evaluations. It
  walks every live FnObj in the gc chain, marks the body subtree
  of each one, then releases AstChain entries whose roots have
  no marked node. Currently-executing programs are protected by
  an explicit pin stack (`ast_pin_root`/`ast_unpin_root`) that
  the sweeper treats as live regardless of FnObj reachability.
  Regression test 50_load_no_leak runs `load()` 500 times.

- F-103: `bi_format` leaked its StrBuf payload on every error
  raised inside the format-string parser (unknown specifier,
  truncated, precision too large, etc). 9 of the 10 die() sites
  did not call `sb_free(&out)` first. Fix: wrap the function
  body in a local error sandbox; on longjmp release `out` and
  any heap scratch buffer, then re-raise. Regression test
  err_format_unknown anchors the path.

### Robustness
- F-108: linear AST chain limit (`CHAIN_MAX`) lowered from 1024
  to 512 for symmetry with `CALL_DEPTH_MAX`. Eval of 512 stacked
  N_BINOP nodes uses ~425 KB of stack, comfortably below the
  default Windows 1 MB.
- F-106: duplicate parameter names in `fn(a, b, a)` are now a
  parse error (`fn: duplicate parameter name 'a'`) instead of
  silently overwriting at call time. Regression err_dup_param.
- F-110/F-111: `g_alloc_bytes` is incremented by the *delta*
  when env or map containers grow, not by the full new size.
  Avoids stale gc thresholds and over-eager collection.
- F-102: `bi_load` now resets `g_break` and `g_ret` on its
  error re-raise so a partly executed loaded script cannot leak
  break/return state into the caller's surrounding loop.

### Tests
- 69 unit (up from 66), 6 repl, 9 smoke = 84 green.
- Fuzz harness: 14562 mutation runs in 90 sec, 0 crashes.
- gcc 15.2 -fanalyzer on all 5 files: 0 warnings.

## 1.0.9 - ninth audit pass: stack exhaustion via linear AST

Ninth audit. Surfaced a DoS angle that six audits had walked past:
the parser accepts a left-associative operator chain as a single
linear tree, and the evaluator plus the gc marker recurse through
that tree one N_BINOP node at a time. With 20000 plus-signs in
"1+1+1+..." the C stack overflows on Windows.

### Fixes
- parser caps every left-associative chain (arith, comparison,
  logic, postfix) at 2048 terms. Adversarial input is rejected
  as "parse: expression chain too long" in milliseconds instead
  of crashing.
- mark_ast made iterative. Uses a heap-allocated work stack and
  the same drain pattern as the gc object marker. Even if a
  trusted but legitimately deep AST slips past the parser cap,
  gc marking never recurses through C stack.
- free_node made iterative. The partial-AST cleanup on syntax
  error could previously overflow the C stack when the REPL error
  handler called free_node on a huge chain, potentially crashing
  the interpreter on a deliberately bad input.
- bi_seed: fmod(fs * 2654435761.0, ...) can overflow to inf when
  |fs| is near 1e300, and then (uint32_t)nan is UB. Now falls
  back to xoring the bit pattern of the seed if the fold produces
  nan/inf.

### Tests
- 57 unit (up from 56), 6 repl, 9 smoke = 72 green. New
  err_chain_too_long exercises the 2048-term parser cap with a
  3000-term chain.

## 1.0.8 - eighth audit pass: locale + leak-proofing

Eighth audit. Two concrete issues and a few microscopic defensive
cleanups that turned up in a line-by-line reread.

### Locale safety
- main() now pins LC_NUMERIC to the "C" locale at startup. A host
  environment with ru_RU / de_DE / etc. locale would otherwise make
  strtod reject "3.14" (expecting "3,14") and format_num print
  commas inside numbers. The lexer and printer are now robust
  against any hosting environment. Regression test 49_locale.

### Die-path leaks reduced
- format_map, bi_keys, bi_values collect keys into a gc-protected
  ListObj on the value stack instead of a bare xmalloc / free pair.
  A die() deep inside value_format_depth or value equality would
  previously leak the raw pointer array; now gc reclaims the list
  on the next sweep.

### Overflow hardening
- String literal lexer: cap*=2 doubling now saturates at 2 GB so a
  pathologically long literal cannot wrap cap negative and under-
  allocate before the memcpy.
- intern_grow: capacity doubling saturated at 2^29 with explicit
  overflow die, matching what was already in map_set.
- intern load-factor check rewritten overflow-safe: the old
  (len+1)*4 > cap*3 could wrap int at very high counts.
- map_set load-factor check rewritten the same way.
- parse_fn_body caps fn parameters at 1024 so fn->n + 1 cannot
  overflow xrealloc size math.

### Cleanup
- cmp_strobj removed: after the format_map and bi_keys/bi_values
  rewrite no caller uses it. cmp_value_str is now the single
  comparator for StrObj-valued arrays.

### Tests
- 56 unit (up from 55), 6 repl, 9 smoke = 71 green. New
  49_locale anchors the LC_NUMERIC hardening.

## 1.0.7 - seventh audit pass: REPL hygiene

Seventh audit focused on interpreter state hygiene across the
setjmp/longjmp error boundary. Found that a long REPL session with
frequent errors would steadily lose slots on the gc value stack,
eventually tripping VSTACK_MAX and breaking all further input.

### Fixes
- REPL error handler now resets g_vsp to 0. Previously each error
  left whatever roots the current expression had pushed on the value
  stack. After roughly 200 to 300 errors, g_vsp hit VSTACK_MAX and
  the interpreter refused new input with "value stack overflow".
  Regression test "repeat errors" runs 256 bad statements followed
  by a good one and verifies the good one still produces output.
- run_source: explicit g_vsp = 0 on normal return, making the reset
  invariant hold without relying on careful balanced vpush/vpop in
  every branch.

### Tests
- 55 unit, 6 repl (up from 5), 9 smoke = 70 green. New REPL case
  "repeat errors" exercises the state-reset invariant.

## 1.0.6 - sixth audit pass: security review

Sixth audit. Focused specifically on memory-safety and security: heap
overflows from integer wrap, double-free from longjmp paths, and DoS
through pathological input sizes.

### Critical security fixes
- bi_load: double-free of the slurp buffer on the error path. The
  success branch freed src after parse_program, then exec_block could
  longjmp away and the error branch freed src again. Fixed by marking
  src volatile and nilling it out after the success-path free, so the
  error path only frees it if still owned.
- str_concat: int overflow when a->len + b->len exceeded INT_MAX could
  produce a smaller-than-needed buffer and the memcpy would write out
  of bounds. Now guarded with an explicit upper bound before the add.
- bi_join: total length accumulated as int; with enough short strings
  the addition wrapped and xmalloc allocated too little, letting the
  write loop overflow the heap. Now computed in size_t with explicit
  overflow checks against a 2 GB ceiling on each step.
- bi_replace: the doubling loops "while (need > cap) cap *= 2" did not
  check for cap wrapping to a negative int, giving either an infinite
  loop or an under-sized buffer. Both growth sites now use saturated
  arithmetic with explicit error on overflow.
- sb_need: same cap*=2 overflow, rewritten with saturation. This
  affects every string operation that builds output through StrBuf
  (format, split, repr, etc.).
- bi_read and bi_lines: the read-and-grow loops had the same cap*=2
  issue and could heap-overflow on a pathologically long line.
- list_push, env_define, map_set: cap*2 growth is now saturated to
  prevent int wrap on very large containers. list_push also no longer
  over-counts g_alloc_bytes (it reported the full new size each grow
  instead of the delta, causing gc to fire too aggressively).
- format_num: n == (double)(long long)n was evaluated unconditionally
  even when n was outside the long-long range, which is UB in C99.
  Now guarded behind an explicit range check.
- main entry: argument length now checked before casting strlen's
  size_t return to int, preventing a pathologically long argv[i] from
  fabricating a negative length into str_new.
- slurp and bi_read_file: file size is rejected if it exceeds the
  2 GB string ceiling, so a misleading ftell result cannot silently
  truncate.

### Robustness
- parse.c: node_add_kid refuses to exceed ~2G children, preventing int
  wrap on the n->n counter for adversarial input.
- N_INDEX_SET and N_DOT_SET keep the value pushed on the gc value
  stack until the mutation completes, so a future gc trigger inside
  map_set or list_push could not prematurely reclaim the value.

### Tests
- 55 unit, 5 repl, 9 smoke = 69 green. New 48_overflow_guards
  exercises the hardened paths (big repeat, join, replace, concat).

## 1.0.5 - fifth audit pass

Fifth audit. Attention moved to gc scalability under deep object graphs
and to a few remaining pathological paths in the builtin library.

### Scalability
- Garbage collector marker uses an explicit work stack. mark_list,
  mark_map, mark_fn and mark_env push out-edges via mark_push, which
  adds to a heap-allocated Object* stack. mark_drain then pulls
  items one at a time. Deeply nested lists or maps (thousands of
  levels) no longer overflow the C stack during gc marking. The
  work stack grows on demand via xrealloc, so the upper bound is
  only the available heap. Regression 47_gc_deep_list chains 5000
  lists and 5000 maps inside one another.

### Builtins
- bi_format heap-spills the scratch buffer for %f with precision so
  large that it overflows the 64-byte stack buffer, e.g. %.100f of
  1e300. Previously such specifiers produced "format: %f output too
  large"; they now encode successfully.
- bi_format caps width and precision at 100000 each so a format
  string like "%99999999d" cannot integer-overflow the parser's
  width accumulator.
- bi_format padding uses bulk sb_putn writes (64 bytes at a time)
  instead of one sb_putc per padding byte, eliminating the O(width)
  function-call overhead.
- bi_read strips CR on input so Windows-style CRLF stdin behaves
  like LF, matching the behaviour of lines() and write_file.
- bi_repeat handles empty input string up front, removing the
  possibility of dividing by s->len == 0 in the overflow guard.

### Tests
- 54 unit, 5 repl, 9 smoke = 68 green. New 47_gc_deep_list.

## 1.0.4 - fourth audit pass

Fourth audit. Focus on termination guarantees against pathological or
adversarial user code: cyclic containers, deeply nested source, and
buffered output lost around errors.

### Correctness
- value_format now caps nesting at 64 levels. Cyclic lists and maps
  used to recurse infinitely through format_list / format_map and
  overflow the C stack (STATUS_STACK_OVERFLOW on Windows). They now
  print as "[...]" / "{...}" at the cap and terminate cleanly.
  Regression 45_cyclic_print covers direct and indirect self-refs
  plus a very deep acyclic chain.
- die() flushes stdout before raising. Previously a buffered print
  just before an error could vanish, leaving the user with an error
  message detached from the output that produced it.
- Parser enforces a 256-level expression nesting limit. Deeply
  adversarial input like 300 nested parens now raises a clean parse
  error rather than overflowing the C stack. Counter is reset at
  the start of every parse_program, so normal nested code is
  unaffected. Regression 46_parse_depth_ok exercises the normal
  path with 10 nested parens, a chain of 5 function calls and a
  mixed list-of-maps-of-list structure.

### Tests
- 53 unit, 5 repl, 9 smoke = 67 green. New 45_cyclic_print,
  46_parse_depth_ok.

## 1.0.3 - third audit pass

Third audit focused on undefined-behaviour traps in numeric coercion
and on the setjmp/longjmp boundary where the previous pass had already
found a volatile-related miscompile.

### Undefined behaviour fixes
- Casting a non-finite or out-of-range double to an integer is UB in
  C99. All such casts (list/string index, for-to bounds, chr, slice,
  exit code, format %d/%x, sleep, rand bound, seed) now route through
  safe_dtoi / safe_dtoll helpers that raise a clean language-level
  error. New unit test err_nan_index covers the most common case.
- parse_program originally used setjmp with a non-volatile local
  pointer to clean up a partial AST on syntax error. Under gcc -O2
  this miscompiled on normal paths too: a program with three or more
  top-level statements lost the first one. The AST cleanup now goes
  through a global g_parse_current that the outer error handler in
  the REPL, parse_needs_more and bi_load release on longjmp. No
  setjmp is required inside parse_program itself, eliminating the
  fragile interaction with the optimiser.
- parse_needs_more declares its 'more' flag volatile so gcc's
  -Wclobbered cannot warn and the value survives longjmp.
- bi_load wraps both parse and execute inside a local error handler,
  freeing the slurp buffer and resetting g_src_name / g_err_line on
  the error path. Previously a die() during exec_block of a loaded
  file leaked the source buffer and left the caller's source context
  dangling.
- bi_seed replaces the unsafe (uint32_t)(uint64_t)double cast with a
  defined fmod-based fold; nan/inf seeds are rejected explicitly.
- bi_sleep rejects nan/inf and caps at 24 hours before the (time_t)
  / (DWORD) cast.
- bi_repeat rejects nan/inf before converting the count to int.

### Build hygiene
- -Wclobbered added to the strict target and to the linux-gcc CI
  job; this is how the parse_needs_more issue was detected.

### Tests
- 51 unit, 5 repl, 9 smoke = 65 green. New err_nan_index.

## 1.0.2 - second audit pass

Second systematic audit. Found and fixed one miscompile-class bug
(the parser returning stale state under -O2), several smaller leaks,
and tightened semantics around loop control flow.

### Correctness
- parse_program uses setjmp to clean up partial AST on syntax error.
  The local 'block' pointer is written after setjmp, which per
  C99 7.13.2.1 requires 'volatile' to survive longjmp. Under gcc -O2
  the optimiser cached the pointer in a register, which on the error
  path read stale data and occasionally dropped the first statement
  of a successful parse. Marking block as volatile restores the
  guarantee.
- call_value now saves and restores g_break alongside g_ret. A bare
  'break' inside a function body could previously leak out and exit
  the caller's surrounding loop. Regression test in 40_break_isolation.
- for-in over a list snapshots the length before the loop starts.
  push()/pop() inside the body can no longer cause infinite iteration
  or late-bound index reads. Semantics match lua ipairs.
- format_num uses a single isinf() probe branching on sign, so the
  expression does not trigger the -Wfloat-conversion pedantic
  warning on macro-based isinf() implementations.

### Leak-free error paths
- parse_program catches its own longjmp and frees the partially
  built AST before re-raising. REPL sessions that produce many
  syntax errors no longer accumulate orphaned Nodes.
- for-in over a map uses a value-stack-protected list for key
  collection. A die() inside the loop body no longer leaks the
  temporary key buffer.
- bi_sort merge buffer is a transient ListObj pushed on the value
  stack. A die() from inside a user-supplied comparator reclaims it
  on the next gc cycle instead of leaking the raw malloc.
- bi_load installs a local error handler that restores g_src_name
  and g_err_line before re-raising, so die() from a loaded script
  no longer corrupts the parent source context.
- slurp() and bi_read_file() check ferror() before committing the
  result; a partial read from a broken I/O path now returns NULL/nil
  instead of silently truncating.

### Input validation
- bi_repeat caps count at 1e9 and checks for int overflow before
  allocating, avoiding a short-buffer write.
- bi_rand rejects bounds above 2^32 that would alias under uint32_t
  modulo.
- bi_format caps %f precision at 100 and uses a 256-byte scratch
  buffer with explicit snprintf length check, avoiding silent
  truncation.
- bi_write_file compares return value in size_t to avoid a false
  positive on 2GB+ payloads.

### Robustness
- parse_needs_more uses an explicit g_err_at_eof flag set by the
  lexer and parser when the error was caused by premature EOF. The
  REPL no longer relies on substring matching inside g_err_msg.
- REPL input buffer moved from 64KB on-stack to heap allocation,
  keeping the platform footprint on tiny-stack targets small.
- Comparator helpers (cmp_strobj, cmp_value_str, string-range
  comparisons in eval.c) use explicit sign-preserving ordering
  instead of int subtraction that could overflow on pathological
  inputs.
- mark_env is now iterative, walking the closure parent chain with
  a while loop instead of recursion, so a deep closure chain cannot
  overflow the C call stack during gc marking on tiny-stack
  platforms.

### Tests
- 50 unit cases now (up from 44 after the first audit). New:
  39_break_scope, 40_break_isolation, 41_for_in_mutate,
  42_volatile_setjmp, 43_sort_custom_cmp, 44_closure_edge.
- Full matrix remains green (50 unit + 5 repl + 9 smoke = 64 green).

### Documentation
- POCKET.md clarifies that -5 is the unary-minus operator applied
  to a number literal, not a literal by itself, and documents the
  exponential notation (1.5e10).
- SPEC.json number regex updated accordingly.

## 1.0.1 - deep audit pass

Internal hardening pass, no user-visible language changes.

### Reliability
- N_CALL no longer uses a separate heap allocation for argv when
  argc is large. Arguments live directly on the value stack,
  removing the last code path that could leak on die() via longjmp
  from inside a callee.
- N_BINOP keeps both operands on the value stack across the binop
  itself, so a future gc-triggering operation inside eval_binop
  cannot invalidate the second operand.
- N_INDEX applies the same discipline: both the collection and the
  index stay pushed until the result is computed, then pop cleanly.

### Correctness
- format_num uses isnan / isinf from <math.h> instead of ad hoc
  probes (n != n, n == 1.0/0.0). Portable to strict C99 toolchains
  and to platforms with signaling NaN.
- String literals are interned at lex time. Two identical "abc"
  tokens share a single StrObj, so map key lookups with literal
  keys cost one pointer compare and never re-hash.
- str_new and str_concat add the payload size (len+1) to
  g_alloc_bytes in addition to the struct itself, so the gc
  threshold reflects real string pressure rather than a stale
  approximation.

### Tests
- Added 36_call_manyargs, 37_intern_literals, 38_special_numbers.

## 1.0 - first release

### Language
- 7 types: nil, bool, number (float64), string, list, map, fn
- 17 keywords, recursive descent parser, tree-walking interpreter
- Closures with lexical scoping
- Mark-and-sweep GC with value stack and AST roots
- Deterministic map iteration (sorted keys)
- Ascending-only `for-to` (use `while` for descending)
- Recursion depth limit of 512 frames
- setjmp/longjmp for REPL error recovery

### Standard library (53 builtins)
- io:         print, write, repr, read
- types:      len, type, str, num
- strings:    upper, lower, trim, repeat, starts_with, ends_with, contains,
              split, join, find, slice, replace
- lists:      push, pop, append, reverse, sort (stable, with optional comparator)
- maps:       keys, values, has, del
- files:      read_file, write_file, lines
- env:        args, exit, getenv, clock, time, sleep
- math:       abs, min, max, floor, ceil, round, sqrt, pow, rand, seed
- bytes:      chr, ord
- formatting: format (%s %d %x %f with width and precision)
- errors:     error, assert
- modules:    load

### Tooling
- Multi-file C99 source (core.c parse.c eval.c builtins.c main.c plus mew.h),
  builds cleanly on gcc/clang/tcc with
  -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
- Makefile with build, tiny, strict, test, clean targets
- Python test runner with three layers (unit, repl, smoke)
- REPL with multi-line input and error recovery
- Command line flags: -e, -v, -h
- GitHub Actions CI matrix: linux-gcc / clang / tcc / asan, macos, windows

### Documentation
- POCKET.md (~4 KB) human reference, fits on A6 paper
- SPEC.json (~10 KB) machine-readable spec with grammar and translation rules
- README.md with full tour, comparisons, design notes
- 9 realistic examples covering file io, parsing, cli utilities
