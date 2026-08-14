#!/bin/bash
# C++ interop test runner (see tests/README.md, section "C++ 互操作测试").
#
# Runs the cpp_interop_test host program (built via `cmake --build build`)
# and checks that it prints "all passed" and exits 0, matching the pass
# convention of tests/test.sh and tests/gc/run_gc.sh.

set -u
cd "$(dirname "$0")/../.." || exit 1
BIN=${BIN:-./build/bin/cpp_interop_test}

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

if [ ! -x "$BIN" ]; then
    echo -e "${RED}FAIL${NC}  $BIN not found. Build it first: cmake --build build"
    exit 1
fi

out="$("$BIN" 2>&1)"
code=$?

if [ "$code" -eq 0 ] && printf '%s' "$out" | grep -q "all passed"; then
    echo -e "${GREEN}PASS${NC}  cpp_interop_test"
    printf '%s\n' "$out" | tail -1
    exit 0
else
    echo -e "${RED}FAIL${NC}  cpp_interop_test (exit=$code)"
    printf '%s\n' "$out" | tail -15
    exit 1
fi
