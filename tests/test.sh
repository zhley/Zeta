for f in tests/lex/*.zt tests/expr/*.zt tests/stmt/*.zt tests/func/*.zt tests/class/*.zt tests/builtin/*.zt tests/integration/*.zt; do
  echo "=== $f ==="
  ./build/bin/zeta "$f"
  echo ""
done

./build/bin/zeta tests/module/mod_main.zt

# GC tests (heap-config-aware, see tests/gc/run_gc.sh)
echo "=== tests/gc (run_gc.sh) ==="
bash tests/gc/run_gc.sh
