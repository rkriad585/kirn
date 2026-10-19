#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/5_conventions.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

echo "=== gate 5: conventions (kirn driver) ==="
kirn_run() { # dir -> prints "$code|$out"
    local d="$1"; shift
    local so se c
    so="$(mktemp "$TMP/c.XXXX")"; se="$(mktemp "$TMP/e.XXXX")"
    (cd "$d" && timeout "$WATCH" "$KIRN" "$@") >"$so" 2>"$se"; c=$?
    printf '%s|%s%s' "$c" "$(cat "$so")" "$(cat "$se")"
}
P="$TMP/conv"; mkdir -p "$P"
# 1. code/main.kn entry
mkdir -p "$P/a/code"; printf 'fn main() -> none { print("A"); }' > "$P/a/code/main.kn"
r=$(kirn_run "$P/a" run "$P/a"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "A" && report PASS "code/main.kn entry" || report FAIL "code/main.kn entry" "$out"
# 2. manifest main wins over code/main.kn
mkdir -p "$P/b/code"; printf '[package]\nname="b"\nmain="start.kn"\n' > "$P/b/kirn.toml"
printf 'fn main() -> none { print("Bmain"); }' > "$P/b/start.kn"
printf 'fn main() -> none { print("Bcode"); }' > "$P/b/code/main.kn"
r=$(kirn_run "$P/b" run "$P/b"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "Bmain" && report PASS "manifest main preferred" || report FAIL "manifest main preferred" "$out"
# 3. pin.kn as run entry
mkdir -p "$P/c"; printf 'pub fn main() -> none { print("C"); }' > "$P/c/pin.kn"
r=$(kirn_run "$P/c" run "$P/c"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "C" && report PASS "pin.kn as run entry" || report FAIL "pin.kn as run entry" "$out"
# 4. no entry -> fix-it error
mkdir -p "$P/d/code"; printf 'pub fn u() -> Int { return 1; }' > "$P/d/code/util.kn"
r=$(kirn_run "$P/d" run "$P/d"); c=${r%%|*}; out=${r#*|}
[ "$c" != 0 ] && echo "$out" | grep -q "no entry point" && report PASS "no entry -> fix-it error" || report FAIL "no entry -> fix-it error" "$out"
# 5. pin.kn runs once + pub surface
mkdir -p "$P/proj/pets/greet/code"
printf 'var loads = 0;\nloads = loads + 1;\npub fn load_count() -> Int { return loads; }\npub fn hi(who: Str) -> Str { return "hi " + who; }\n' > "$P/proj/pets/greet/code/pin.kn"
printf 'import greet;\nimport greet as g2;\nimport greet as g3;\nfn main() -> none {\n    print("count=", greet.load_count());\n    print(greet.hi("world"));\n}\n' > "$P/proj/main.kn"
r=$(kirn_run "$P/proj" run "$P/proj"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "count= 1" && report PASS "pin.kn runs once (count=1)" || report FAIL "pin.kn runs once (count=1)" "$out"
[ "$c" = 0 ] && echo "$out" | grep -q "hi world" && report PASS "pin.kn pub surface" || report FAIL "pin.kn pub surface" "$out"
# 6. pin.kn discovered without manifest
mkdir -p "$P/proj2/pets/nomanifest/code"
printf 'pub fn poke() -> Str { return "poked"; }\n' > "$P/proj2/pets/nomanifest/code/pin.kn"
printf 'import nomanifest;\nfn main() -> none { print(nomanifest.poke()); }\n' > "$P/proj2/main.kn"
r=$(kirn_run "$P/proj2" run "$P/proj2"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "poked" && report PASS "pin.kn discovered w/o manifest" || report FAIL "pin.kn discovered w/o manifest" "$out"
# 7. kirn new (app scaffold): bare `kirn run` + `kirn test .` — the prelude
#    batteries (bail/ensure/ok/err/Error/Result) must need no import.
(cd "$P" && timeout "$WATCH" "$KIRN" new gapp) >/dev/null 2>&1
r=$(kirn_run "$P/gapp" run); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "hello from gapp, world!" && report PASS "kirn new app: bare run" || report FAIL "kirn new app: bare run" "$out"
r=$(kirn_run "$P/gapp" test .); c=${r%%|*}; out=${r#*|}
echo "$out" | grep -q "1 passed, 0 failed" && report PASS "kirn new app: test" || report FAIL "kirn new app: test" "$out"
# 8. kirn new pet: pin.kn must compile under `kirn test`, and the
#    README-advertised `kirn build` must pack a .pet.
(cd "$P" && timeout "$WATCH" "$KIRN" new pet gpk) >/dev/null 2>&1
r=$(kirn_run "$P/gpk" test .); c=${r%%|*}; out=${r#*|}
echo "$out" | grep -q "1 passed, 0 failed" && report PASS "kirn new pet: test" || report FAIL "kirn new pet: test" "$out"
r=$(kirn_run "$P/gpk" build); c=${r%%|*}; out=${r#*|}
echo "$out" | grep -q "gpk-.*\.pet" && report PASS "kirn new pet: build packs" || report FAIL "kirn new pet: build packs" "$out"

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
