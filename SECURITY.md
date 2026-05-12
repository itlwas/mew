# Security Policy

## Supported versions

Only the latest release on `main` is supported.

## Reporting a vulnerability

If you find a security issue, do not open a public issue. Email the maintainer
at the address listed in the commit log, or use GitHub's
["Report a vulnerability"](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
private advisory flow on this repository.

Include:

- a minimal `.mew` reproducer or CLI invocation,
- the platform, compiler and flags,
- the observed behaviour (crash, memory corruption, unexpected output),
- the expected behaviour.

You will get an acknowledgement within a few days. A fix will land on `main`
along with a regression test in `tests/cases/`, and the advisory will be
published after the fix is available.

## Threat model (short version)

`mew` treats `.mew` source files as untrusted input:

- parser recursion is bounded (`PARSE_DEPTH_MAX`, `CHAIN_MAX`),
- runtime recursion is bounded (`CALL_DEPTH_MAX`),
- numeric casts to integer reject NaN, infinity and out-of-range doubles,
- GC roots cover every value reachable from globals, the value stack,
  the AST chain and the in-flight return register.

`mew` does not sandbox the filesystem or network. A script has the full
authority of the process it runs in: `read_file`, `write_file`, `load` and
`getenv` are not restricted. Do not run untrusted scripts with elevated
privileges.
