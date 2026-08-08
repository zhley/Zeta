# Zeta 测试用例

## 目录结构

```
tests/
├── lex/                    # 词法测试
│   ├── 01_integer_literals.zt
│   ├── 02_float_literals.zt
│   ├── 03_string_literals.zt
│   └── 04_bool_null.zt
├── expr/                   # 表达式测试
│   ├── 01_arithmetic.zt     # 算术运算
│   ├── 02_comparison.zt     # 比较运算
│   ├── 03_logical.zt        # 逻辑运算 & 短路
│   ├── 04_bitwise.zt        # 位运算
│   ├── 05_ternary.zt        # 三元条件
│   ├── 06_string_ops.zt     # 字符串操作
│   ├── 07_is_operator.zt    # is 运算符
│   └── 08_precedence.zt     # 运算符优先级
├── stmt/                   # 语句测试
│   ├── 01_var_let.zt
│   ├── 02_assignment.zt     # 复合赋值
│   ├── 03_if_else.zt
│   ├── 04_while.zt
│   ├── 05_for_in.zt
│   ├── 06_break_continue.zt
│   ├── 07_return.zt
│   └── 08_block_scope.zt
├── func/                   # 函数测试
│   ├── 01_basic.zt
│   ├── 02_lambda.zt        # 函数字面量 & 高阶函数
│   ├── 03_recursion.zt
│   ├── 04_higher_order.zt  # map/filter/reduce
│   └── 05_function_value.zt # 具名函数作为一等值
├── class/                  # 类测试
│   ├── 01_basic.zt
│   ├── 02_constructor.zt
│   ├── 03_inheritance.zt
│   ├── 04_equals.zt        # _equals 自定义比较
│   ├── 05_iter.zt          # 自定义迭代器
│   ├── 06_super.zt         # super.method() 调用 (2 层继承)
│   └── 07_class_value.zt   # 类作为一等值
├── builtin/                # 内置函数 & 方法测试
│   ├── 01_print.zt         # 打印输出 (与 01_print.expected 比对)
│   ├── 01_print.expected   # 01_print.zt 的期望输出
│   ├── 02_array.zt
│   ├── 03_map.zt
│   ├── 04_iter_next.zt
│   ├── 05_error_check.zt
│   ├── 06_intern.zt
│   ├── 07_input.zt         # 标准输入 (runner 自动喂值)
│   ├── 08_type.zt          # type() 类型名
│   └── 09_conversions.zt   # int()/float()/str() 转换
├── module/                 # 模块系统测试
│   ├── mod_a.zt            # 被导入模块 A
│   ├── mod_b.zt            # 被导入模块 B (依赖 A)
│   ├── mod_c.zt            # 被导入模块 C (类导出)
│   └── mod_main.zt         # 主模块 (导入 A, B, C)
├── integration/            # 集成测试
│   ├── 01_fibonacci.zt     # 斐波那契 (迭代+递归)
│   ├── 02_sort.zt          # 冒泡排序
│   ├── 03_linked_list.zt   # 链表 (类实现)
│   ├── 04_count_words.zt   # 字符串和映射
│   ├── 05_calculator.zt    # 计算器 (方法链)
│   ├── 06_hanoi.zt         # 汉诺塔 (递归)
│   └── 07_prime.zt         # 埃氏筛 (数组)
└── gc/                     # GC 测试 (见下方 "GC 测试" 一节)
    ├── 01_young_churn.zt   # 新生代分配压力下基本正确性
    ├── 02_retained_structure.zt # 保留结构 (链表/树) 跨 GC 存活
    ├── 03_old_young.zt     # 老年代对象引用新生代 (remembered set)
    ├── 04_cycles.zt        # 循环引用图跨 GC 存活
    ├── 05_strings.zt       # StrObj 拼接 / intern / 大字符串
    ├── 06_instances.zt     # 继承实例 churn, 字段与方法分派
    ├── 07_recursion.zt     # 深递归: 栈帧局部变量为根
    ├── 08_iterators.zt     # 迭代器跨 GC 存活
    ├── 09_globals_modules.zt # 模块级全局 (含跨模块) 跨 GC 存活
    ├── 10_reclaim.zt       # 有界堆下回收验证 (-i 8 -m 128)
    ├── 11_oom.zt           # 超限堆 → 干净报错退出 (exit 2, -i 8 -m 64)
    ├── 12_heap_growth.zt   # 堆增长 (-i 4 -m 1024)
    ├── 13_determinism.zt   # 不同堆配置下输出一致 (多参数对比)
    ├── gc_mod.zt           # 09 的辅助模块 (不作为测试运行)
    ├── run_gc.sh           # GC 测试运行脚本 (bash)
    └── run_gc.ps1          # GC 测试运行脚本 (powershell)
```

## 运行测试

```bash
# 单个测试
./build/bin/zeta tests/lex/01_integer_literals.zt

# 批量运行 (bash) - 自动检查每个测试的输出 (PASS/FAIL + 汇总)
bash tests/test.sh

# 同时打印每个测试的原始输出
bash tests/test.sh v

# 清理 tests/ 下所有 *.ztc / *.dump 文件
bash tests/test.sh clear

# 批量运行 (powershell)
powershell -File tests/test.ps1

# 模块测试
./build/bin/zeta tests/module/mod_main.zt
```

## 测试约定

- 每个测试包含 `main()` 函数作为入口。
- 使用内置函数 `assert()` 验证, 而不是手动维护 pass/fail 计数。
- **`assert()` 只接受 Bool 类型的条件**: 非 Bool 会报 `[Runtime Error][line N]: Assert: condition must be a boolean`。因此需要把表达式转为比较/`is`/`check()` 等 Bool 形式后再断言 (注意 `&&`/`||` 返回的是操作数值本身, 不一定为 Bool)。
- 断言失败时 VM 打印 `[Runtime Error][line N]: Assertion failed` 并立即终止, 不会打印汇总行。
- 所有断言通过后, 输出类似 `tests_name: all passed` 的汇总行。
- `builtin/01_print.zt` 校验 `print()` 的精确输出: 其输出须与 `tests/builtin/01_print.expected` 逐字节一致 (由 `test.sh` 比对)。
- `builtin/07_input.zt` 读取标准输入: `test.sh` 会自动喂入一行输入, 再检查输出 (输入为空时 `input()` 返回 null, 不会打印 "all passed", 判定失败)。
- `super.method()` 的语义是"跳过当前方法所属的类, 从父类开始向上查找", 支持任意层数的继承链 (见 `class/06_super.zt`); 调用祖父类的方法也可以直接用类名调用, 如 `B.who(instance)`。

## GC 测试 (`tests/gc/`)

GC 测试是内存压力下的黑盒测试: 通过大量分配触发垃圾回收, 再校验保留数据的内容正确性。运行脚本按各文件声明的 `// EXPECT:` 判定成败 (默认: 输出包含 `all passed`。运行期错误如 `assert` 失败后进程仍以 0 退出, 所以输出检查才是关键; OOM 用例则检查退出码 2 与报错信息, 见下)。

### 文件头约定

每个 `tests/gc/[0-9]*.zt` 可在文件开头声明 (均为 Zeta 注释, 故以 `//` 开头):

- `// ARGS: <vm 参数> [;; <vm 参数> ...]`: 声明运行该测试的 VM 堆参数 (对应 `-i`/`-m` 等 CLI 选项)。多组参数以 `;;` 分隔; 此时脚本会每组各跑一次, 并检查**各组输出逐字节一致** (确定性检查: GC 调度不应影响结果)。无该注释时使用默认堆参数。
- `// EXPECT: all_passed | out_of_memory`: 判定方式, 默认 `all_passed` (退出码 0 且输出包含 "all passed")。`out_of_memory` 要求退出码为 2 且输出包含 "Heap limit exceeded": 堆达到 `-m` 上限时 VM 抛出 `VMException`, `main()` 捕获后打印错误信息并以 2 退出 (退出码约定见 `src/main.cpp`)。

`tests/gc/gc_mod.zt` 是 `09_globals_modules.zt` 的辅助模块 (被 import), 不属于测试用例, 通配符 `[0-9]*.zt` 会将其排除。

### 运行方式

```bash
bash tests/gc/run_gc.sh       # 全部 GC 测试 (bash)
# powershell -File tests/gc/run_gc.ps1
```

`bash tests/test.sh` 和 `tests/test.ps1` 已在末尾接入 GC 测试。
