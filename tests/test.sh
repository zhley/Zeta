#!/bin/bash
# Zeta test runner (see tests/README.md).
#
# Usage:
#   bash tests/test.sh           run all tests and check pass/fail
#   bash tests/test.sh v         also print each test's raw output
#   bash tests/test.sh clear     delete every *.ztc / *.dump under tests/
#
# Conventions:
#   - every test prints "<name>: all passed" on success. Runtime errors now
#     exit 3 (VM exceptions exit 2), so each run must also exit 0.
#   - tests/builtin/01_print.zt verifies the exact output of print(); its
#     expected output lives in tests/builtin/01_print.expected.
#   - tests/builtin/07_input.zt reads stdin; the runner feeds it a value.
#   - tests/gc/* are delegated to tests/gc/run_gc.sh (heap-config-aware).
#   - tests/cpp/* are delegated to tests/cpp/run_cpp.sh (C++ interop).

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

VERBOSE=0
[ "${1:-}" = "v" ] && VERBOSE=1

pass=0
fail=0

run_test() {
    local f="$1" out="" code=0
    if [ "$f" = "tests/builtin/01_print.zt" ]; then
        out="$("$BIN" "$f" 2>&1)"
        code=$?
        if [ "$VERBOSE" -eq 1 ]; then
            echo ""
            echo "=== $f ==="
            printf '%s\n' "$out"
        fi
        if [ "$code" -eq 0 ] && printf '%s\n' "$out" | diff - tests/builtin/01_print.expected >/dev/null; then
            echo -e "${GREEN}PASS${NC}  $f"
            pass=$((pass + 1))
        else
            echo -e "${RED}FAIL${NC}  $f (exit=$code, output differs from 01_print.expected)"
            printf '%s\n' "$out" | diff - tests/builtin/01_print.expected | head -8
            fail=$((fail + 1))
        fi
        return
    fi

    if [ "$f" = "tests/builtin/07_input.zt" ]; then
        out="$(printf 'hello\n' | "$BIN" "$f" 2>&1)"
        code=$?
    else
        out="$("$BIN" "$f" 2>&1)"
        code=$?
    fi
    if [ "$VERBOSE" -eq 1 ]; then
        echo ""
        echo "=== $f ==="
        printf '%s\n' "$out"
    fi
    if [ "$code" -eq 0 ] && printf '%s' "$out" | grep -q "all passed"; then
        echo -e "${GREEN}PASS${NC}  $f"
        pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${NC}  $f (exit=$code)"
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

echo ""
echo "=== tests/cpp (run_cpp.sh) ==="
bash tests/cpp/run_cpp.sh
cpp_ok=$?

[ "$fail" -eq 0 ] && [ "$gc_ok" -eq 0 ] && [ "$cpp_ok" -eq 0 ]
