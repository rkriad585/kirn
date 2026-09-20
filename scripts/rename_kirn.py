#!/usr/bin/env python3
"""scripts/rename_kirn.py - idempotent, dry-run-first, examples-first Coco->Kirn PROSE renamer.

Plain Python 3, stdlib only (argparse, pathlib, re). Zero deps. This is the
single tool that executes the PROSE layer of the rename per
COCO_PLANS/COCO_TO_KIRN_PLAN.md x4.5 ("one name, four case-forms").

GATE - never break the corpus. Dry-run against examples/ FIRST (the whole demo
corpus - small, self-contained, rebuild-free). Only when that passes G-VERIFY
(all harnesses green) may --apply run against the main tree (src/ tools/ stdlib/
tests/ docs/).

PROSE ONLY. Deliberately does NOT touch later-phase layers (own gates):
  - .co -> .kn source extension (Phase 5)
  - coco_* identifiers, coco::/lib. namespaces, .cob/COCOB magic (frozen)
  - .cocolib -> .pet bundle, coco_libs/ -> pets/ deps dir, coco.toml/lock
Usage:
    python scripts/rename_kirn.py --scan examples     # dry-run inventory, no writes
    python scripts/rename_kirn.py --apply examples    # examples FIRST, after scan green
    python scripts/rename_kirn.py --apply src tools stdlib tests docs   # main after green
"""

import argparse
import re
import sys
from pathlib import Path

CASE_FORMS = [
    (re.compile(r"\bCoco\b"), "Kirn"),  # prose / capitalized
    (re.compile(r"\bcoco\b"), "kirn"),  # identifiers / strings / commands
    (re.compile(r"\bCOCO\b"), "KIRN"),  # env / cmake / magic
]

# Blocklist: tokens the PROSE layer must never touch - they belong to later
# phases (extensions -> Phase 5, identifiers -> Phase 6, bundle/registry ->
# Phase 7, dirs -> Phase 7). Short-circuits a whole line.
BLOCKLIST = re.compile(
    r"coco_|coco/|/coco|\.co\b|\.cob\b|\.cocolib\b|COCOB\b|COCO_|"
    r"cocolib|lib\.|github\.com/coco|coco_libs|coco\.toml|coco\.lock|"
    r"coco-lib|coco-libs|coco-ffi|coco-pkg|coco-registry|\.coco-"
)

SKIP_SUFFIX = {".py", ".ps1", ".exe", ".dll", ".obj", ".lib", ".cob", ".co", ".kn"}
SKIP_PART = {"build", ".git", "third_party", "coco_libs"}


def _iter_target_files(dirs):
    for d in dirs:
        root = Path(d)
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if not p.is_file():
                continue
            if any(x in p.parts for x in SKIP_PART):
                continue
            if p.suffix.lower() in SKIP_SUFFIX:
                continue
            yield p


def _protected_spans(text):
    """Byte spans of BLOCKLIST matches over the ORIGINAL text (identity layer,
    frozen before any prose write). A later replacement is rejected if its span
    overlaps any protected span - so grammar-filename/dir/extension/magic/
    registry references survive every case-form regardless of pass order."""
    return {m.span() for m in BLOCKLIST.finditer(text)}


def _overlaps(span, protected):
    a, b = span
    return any(not (b <= x or y <= a) for x, y in protected)


def _rename_one(text, protected, form):
    pat, repl = form
    if not protected:
        out = pat.sub(repl, text)
        return out, int(out != text)
    hits = []

    def _cb(m):
        if _overlaps(m.span(), protected):
            return m.group(0)
        hits.append(m.span())
        return repl

    out = pat.sub(_cb, text)
    return out, bool(hits)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", metavar="DIR", nargs="+", help="dry-run inventory")
    ap.add_argument("--apply", metavar="DIR", nargs="+", help="apply after scan green")
    args = ap.parse_args()
    apply_mode = bool(args.apply)
    targets = args.apply if args.apply else args.scan
    if not targets:
        sys.exit("error: pass --scan DIR first, then --apply DIR")
    total = 0
    files = 0
    for p in _iter_target_files(targets):
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        out = text
        n = 0
        protected = _protected_spans(text)
        for form in CASE_FORMS:
            out, c = _rename_one(out, protected, form)
            n += c
        if not n:
            continue
        total += n
        files += 1
        if apply_mode:
            p.write_text(out, encoding="utf-8", newline="")
        else:
            print(f"{n:5d}  {p.as_posix()}")
    verb = "applied" if apply_mode else "scanned"
    print(f"[{verb}] {total} replacement(s) across {files} file(s) (prose layer only)")
    if not apply_mode:
        print(
            "dry-run only - re-run with --apply examples, confirm G-VERIFY, then the main tree."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
