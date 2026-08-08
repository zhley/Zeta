#!/bin/bash
# GC black-box test runner (see tests/README.md, section "GC 测试").
#
# Each tests/gc/[0-9]*.zt may declare options in its first lines:
#   // ARGS: <vm args> [;; <vm args> ...]      default: no args
#       Declares the heap configuration(s) the test is run with. When
#       multiple sets are given (separated by ";;"), every run's output
#       must be byte-identical (determinism check).
#   // EXPECT: all_passed | out_of_memory       default: all_passed
#       all_passed    - exit code 0 and output must contain "all passed".
#                       (Assert failures and runtime errors still exit 0,
#                       so the output check is what actually matters.)
#       out_of_memory - exit code 2 and output must contain "Heap limit
#                       exceeded": when the heap hits the -m limit the VM
#                       throws VMException, main() prints the message and
#                       returns 2 (see tests/README.md).

set -u
cd "$(dirname "$0")/../.." || exit 1
BIN=${BIN:-./build/bin/zeta}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

pass=0
fail=0
out=""
code=0
ok=0

run_once() {
    local f="$1" args="$2"
    out="$("$BIN" $args "$f" 2>&1)"
    code=$?
    local expect
    expect=$(grep -m1 '^// EXPECT:' "$f" | sed 's/^\/\/ EXPECT:[[:space:]]*//')
    expect=${expect:-all_passed}
    case "$expect" in
        all_passed)
            if [ "$code" -eq 0 ] && printf '%s' "$out" | grep -q "all passed"; then
                ok=1
            else
                ok=0
            fi
            ;;
        out_of_memory)
            if [ "$code" -eq 2 ] && printf '%s' "$out" | grep -q "Heap limit exceeded"; then
                ok=1
            else
                ok=0
            fi
            ;;
        *)
            echo -e "${RED}FAIL${NC}  $f (unknown EXPECT: $expect)"
            ok=0
            ;;
    esac
}

for f in tests/gc/[0-9]*.zt; do
    argsets=$(grep -m1 '^// ARGS:' "$f" | sed 's/^\/\/ ARGS:[[:space:]]*//')

    if [ -z "$argsets" ]; then
        run_once "$f" ""
        if [ "$ok" -eq 1 ]; then
            echo -e "${GREEN}PASS${NC}  $f"
            pass=$((pass + 1))
        else
            echo -e "${RED}FAIL${NC}  $f (exit=$code)"
            printf '%s\n' "$out" | tail -5
            fail=$((fail + 1))
        fi
        continue
    fi

    # one or more argument sets: run each, all outputs must be identical
    IFS=$'\n' read -ra sets <<< "$(printf '%s' "$argsets" | sed 's/;;/\n/g')"
    all_ok=1
    first_out=""
    n=0
    for s in "${sets[@]}"; do
        s=$(echo "$s" | xargs)
        run_once "$f" "$s"
        n=$((n + 1))
        if [ "$ok" -ne 1 ]; then
            echo -e "${RED}FAIL${NC}  $f (args: [$s], exit=$code)"
            printf '%s\n' "$out" | tail -5
            all_ok=0
        fi
        if [ -z "$first_out" ]; then
            first_out="$out"
        elif [ "$first_out" != "$out" ]; then
            echo -e "${RED}FAIL${NC}  $f (output differs between configurations, args: [$s])"
            all_ok=0
        fi
    done
    if [ "$all_ok" -eq 1 ]; then
        echo -e "${GREEN}PASS${NC}  $f ($n runs)"
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
done

echo ""
echo -e "gc tests: $pass ${GREEN}passed${NC}, $fail ${RED}failed${NC}"
[ "$fail" -eq 0 ]
