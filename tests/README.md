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
│   ├── 01_print.zt         # 打印输出 (人工检查)
│   ├── 02_array.zt
│   ├── 03_map.zt
│   ├── 04_iter_next.zt
│   ├── 05_error_check.zt
│   ├── 06_intern.zt
│   ├── 07_input.zt         # 交互测试,需手动运行
│   ├── 08_type.zt          # type() 类型名
│   └── 09_conversions.zt   # int()/float()/str() 转换
├── module/                 # 模块系统测试
│   ├── mod_a.zt            # 被导入模块 A
│   ├── mod_b.zt            # 被导入模块 B (依赖 A)
│   ├── mod_c.zt            # 被导入模块 C (类导出)
│   └── mod_main.zt         # 主模块 (导入 A, B, C)
└── integration/            # 集成测试
    ├── 01_fibonacci.zt     # 斐波那契 (迭代+递归)
    ├── 02_sort.zt          # 冒泡排序
    ├── 03_linked_list.zt   # 链表 (类实现)
    ├── 04_count_words.zt   # 字符串和映射
    ├── 05_calculator.zt    # 计算器 (方法链)
    ├── 06_hanoi.zt         # 汉诺塔 (递归)
    └── 07_prime.zt         # 埃氏筛 (数组)
```

## 运行测试

```bash
# 单个测试
./build/bin/zeta tests/lex/01_integer_literals.zt

# 批量运行 (bash)
bash tests/test.sh

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
- `builtin/01_print.zt` 是人工检查输出的测试 (打印期望值对比); `builtin/07_input.zt` 读取标准输入, 需手动运行。
- 已知限制 (TODO): `class/06_super.zt` 只覆盖 2 层继承链, 3 层及以上链的 `super.method()` 存在 VM bug (见 `src/vm/vm.cpp` 中 `Opcode::SuperCall` 处的 TODO)。
