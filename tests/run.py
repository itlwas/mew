#!/usr/bin/env python3
"""
mew test runner. three layers:

  unit    each .mew in tests/cases/  compared against matching .out/.err
  repl    feed a snippet to stdin, verify substrings in stdout
  smoke   run each example once; should exit 0

usage:
  python run.py           (all layers)
  python run.py unit      (only unit)
  python run.py repl      (only repl)
  python run.py smoke     (only smoke)
"""
import os, pathlib, subprocess, sys, glob

ROOT  = pathlib.Path(__file__).resolve().parent.parent
MEW   = ROOT / ("mew.exe" if os.name == "nt" else "mew")
CASES = ROOT / "tests" / "cases"
EX    = ROOT / "examples"

GREEN = "\033[32m" if sys.stdout.isatty() and os.name != "nt" else ""
RED   = "\033[31m" if sys.stdout.isatty() and os.name != "nt" else ""
R     = "\033[0m"  if GREEN else ""

def norm(s): return s.replace("\r\n", "\n").rstrip("\n")

# ---------------- unit ----------------
def run_unit():
    files = sorted(CASES.glob("*.mew"))
    ok = fail = 0
    for f in files:
        name   = f.stem
        is_err = name.startswith("err_")
        try:
            p = subprocess.run([str(MEW), str(f)], capture_output=True, timeout=30, text=True)
        except subprocess.TimeoutExpired:
            print(f"{RED}FAIL{R} {name}: TIMEOUT"); fail += 1; continue
        if is_err:
            expect = f.with_suffix(".err").read_text(encoding="utf-8").strip() if f.with_suffix(".err").exists() else ""
            if p.returncode == 0:
                print(f"{RED}FAIL{R} {name}: expected nonzero exit"); fail += 1; continue
            if expect and expect not in p.stderr:
                print(f"{RED}FAIL{R} {name}: stderr missing {expect!r}")
                print("  got:", p.stderr); fail += 1; continue
        else:
            expect = f.with_suffix(".out").read_text(encoding="utf-8") if f.with_suffix(".out").exists() else ""
            if p.returncode != 0:
                print(f"{RED}FAIL{R} {name}: exit={p.returncode}\n  stderr: {p.stderr}"); fail += 1; continue
            if norm(p.stdout) != norm(expect):
                print(f"{RED}FAIL{R} {name}: stdout mismatch")
                print("  got:     ", repr(p.stdout)[:200])
                print("  expected:", repr(expect)[:200])
                fail += 1; continue
        ok += 1
    print(f"unit : {ok} passed, {fail} failed of {ok+fail}", file=sys.stderr)
    return fail

# ---------------- repl ----------------
REPL_CASES = [
    ("single-line",    b"print(1+2)\n",                                  [b"3"]),
    ("two lines",      b"x=10\nprint(x*2)\n",                            [b"20"]),
    ("multi-line fn",  b"fn sq(n)\nreturn n*n\nend\nprint(sq(9))\n",     [b"81"]),
    ("for loop",       b"xs=[1,2,3]\nfor v in xs do print(v) end\n",     [b"1", b"2", b"3"]),
    ("error recovery", b"print(undef)\nprint(\"still alive\")\n",        [b"still alive"]),
    # regression: many consecutive errors should not drain the vstack or
    # leak enough to crash the interpreter. 256 errors is far above any
    # realistic scenario and would previously push 512+ roots that never
    # popped.
    ("repeat errors",  (b"print(undef)\n" * 256) + b"print(\"alive\")\n", [b"alive"]),
]

def run_repl():
    ok = fail = 0
    for title, inp, expected in REPL_CASES:
        try:
            p = subprocess.run([str(MEW)], input=inp, capture_output=True, timeout=10)
        except subprocess.TimeoutExpired:
            print(f"{RED}FAIL{R} repl {title}: TIMEOUT"); fail += 1; continue
        pos = 0; good = True
        for e in expected:
            i = p.stdout.find(e, pos)
            if i < 0: good = False; break
            pos = i + len(e)
        if good: ok += 1
        else:
            print(f"{RED}FAIL{R} repl {title}")
            print("  input   :", inp)
            print("  expected:", expected)
            print("  stdout  :", p.stdout)
            fail += 1
    print(f"repl : {ok} passed, {fail} failed of {ok+fail}", file=sys.stderr)
    return fail

# ---------------- smoke ----------------
SMOKE_SPECIAL = {
    "hexdump.mew": ["LICENSE"],
    "grep.mew":    ["mew", "src/main.c"],
    "stats.mew":   [],
    "todo.mew":    ["list"],
    "calc.mew":    [],
}
SMOKE_STDIN = {"stats.mew": b"1 2 3 4 5 6 7 8 9 10"}

def run_smoke():
    files = sorted(EX.glob("*.mew"))
    ok = fail = 0
    for f in files:
        extra = SMOKE_SPECIAL.get(f.name, None)
        stdin = SMOKE_STDIN.get(f.name, None)
        cmd = [str(MEW), str(f)] + (extra if extra is not None else [])
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=60,
                               input=stdin, cwd=str(ROOT))
        except subprocess.TimeoutExpired:
            print(f"{RED}FAIL{R} smoke {f.name}: TIMEOUT"); fail += 1; continue
        if p.returncode != 0:
            print(f"{RED}FAIL{R} smoke {f.name}: exit={p.returncode}")
            print("  stderr:", p.stderr[:200])
            fail += 1
        else:
            ok += 1
    print(f"smoke: {ok} passed, {fail} failed of {ok+fail}", file=sys.stderr)
    return fail

def cleanup():
    for pat in ("todo.txt", "tmp.csv", "tests_tmp_*", "tests/tmp_*"):
        for p in glob.glob(str(ROOT / pat)):
            try: os.remove(p)
            except OSError: pass

def main():
    if not MEW.exists():
        print(f"{RED}mew binary not found at {MEW}{R}"); sys.exit(2)
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    total = 0
    if which in ("all", "unit"):  total += run_unit()
    if which in ("all", "repl"):  total += run_repl()
    if which in ("all", "smoke"): total += run_smoke()
    cleanup()
    if total == 0: print(f"\n{GREEN}ALL OK{R}", file=sys.stderr)
    else:          print(f"\n{RED}{total} failures{R}", file=sys.stderr)
    sys.exit(0 if total == 0 else 1)

if __name__ == "__main__":
    main()
