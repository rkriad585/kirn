# Security Policy

Coco is a **0.0.x-beta** WIP compiler (see `.version`). It runs no daemons,
makes no network calls, and stores no credentials — but reports are still
appreciated and handled.

## Reporting a Vulnerability

Please **do not open a public issue** for security problems. Report privately
instead:

- **Email:** `rkriad585@users.noreply.github.com`
- **GitHub private vulnerability reporting** (preferred if enabled for the
  repo): use the repository's "Report a vulnerability" / Security Advisory
  flow.

You will receive a confirmation that the report was received. We aim to
acknowledge within **5 business days** and to coordinate a fix before any
public disclosure.

## What we ask you to include

- Affected version(s) from `.version` / git ref.
- Reproduction: the smallest `.co` file + the exact command line and harness
  (`coco run`, `cococheck`, `cocoparse`, ...) that triggers it.
- Expected vs actual behavior.

## Supported versions

Only the current `main` tip is supported; there is no LTS. Pre-`0.1.0` betas
are supported on a best-effort basis.

## Security-relevant areas

Compiler/VM memory safety (parsing, type checking, codegen, the `coco`/`cocorun`
runners) is the highest-priority area — memory corruption, UB, or out-of-bounds
in the toolchain pipelines should be reported as above even though this is a
language project.

## Automated analysis

ASan + harness coverage is run by the repo's `asanall`/`rename_baseline` gates;
a green baseline does **not** guarantee absence of issues. Treat any ASan report
as a bug, and please still report it.
