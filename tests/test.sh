#!/bin/bash
# Zeta test runner (see tests/README.md).
#
# Usage:
#   bash tests/test.sh           run all tests and check pass/fail
#   bash tests/test.sh clear     delete every *.ztc / *.dump under tests/
#
# Conventions:
#   - every test prints "<name>: all passed" on success. Runtime errors and
#     assert failures still exit 0, so the output check is what matters.
#   - tests/builtin/01_print.zt verifies the exact output of print(); its
#     expected output lives in tests/builtin/01_print.expected.
#   - tests/builtin/07_input.zt reads stdin; the runner feeds it a value.
#   - tests/gc/* are delegated to tests/gc/run_gc.sh (heap-config-aware).

set -u
cd "$(dirname "$0")/.." || exit 1
BIN=${BIN:-./build/bin/zeta}

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

if [ "${1:-}" = "clear" ]; then
    find tests \( -name '*.ztc' -o -name '*.dump' \) -delete
    echo "cleared all *.ztc and *.dump files under tests/"
    exit 0
fi

pass=0
fail=0

run_test() {
    local f="$1" out=""
    if [ "$f" = "tests/builtin/01_print.zt" ]; then
        out="$("$BIN" "$f" 2>&1)"
        if printf '%s\n' "$out" | diff - tests/builtin/01_print.expected >/dev/null; then
            echo -e "${GREEN}PASS${NC}  $f"
            pass=$((pass + 1))
        else
            echo -e "${RED}FAIL${NC}  $f (output differs from 01_print.expected)"
            printf '%s\n' "$out" | diff - tests/builtin/01_print.expected | head -8
            fail=$((fail + 1))
        fi
        return
    fi

    if [ "$f" = "tests/builtin/07_input.zt" ]; then
        out="$(printf 'hello\n' | "$BIN" "$f" 2>&1)"
    else
        out="$("$BIN" "$f" 2>&1)"
    fi
    if printf '%s' "$out" | grep -q "all passed"; then
        echo -e "${GREEN}PASS${NC}  $f"
        pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${NC}  $f"
        printf '%s\n' "$out" | tail -5
        fail=$((fail + 1))
    fi
}

for f in tests/lex/*.zt tests/expr/*.zt tests/stmt/*.zt tests/func/*.zt tests/class/*.zt tests/builtin/*.zt tests/integration/*.zt; do
    run_test "$f"
done

run_test tests/module/mod_main.zt

echo ""
echo -e "zeta tests: $pass ${GREEN}passed${NC}, $fail ${RED}failed${NC}"

echo ""
echo "=== tests/gc (run_gc.sh) ==="
bash tests/gc/run_gc.sh
gc_ok=$?
[ "$fail" -eq 0 ] && [ "$gc_ok" -eq 0 ]
