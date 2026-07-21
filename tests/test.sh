for f in tests/lex/*.zt tests/expr/*.zt tests/stmt/*.zt tests/func/*.zt tests/class/*.zt tests/builtin/*.zt tests/integration/*.zt; do
  echo "=== $f ==="
  ./build/bin/zeta "$f"
  echo ""
done

./build/bin/zeta tests/module/mod_main.zt
