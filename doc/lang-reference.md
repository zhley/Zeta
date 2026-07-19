# Zeta 语言参考

> **版本**: 0.1.0（开发中，尚未稳定）
> **定位**: 轻量级、面向对象、动态类型脚本语言

---

## 目录

1. [词法结构](#1-词法结构)
2. [类型系统](#2-类型系统)
3. [字面量](#3-字面量)
4. [运算符](#4-运算符)
5. [变量与赋值](#5-变量与赋值)
6. [语句](#6-语句)
7. [函数](#7-函数)
8. [类与对象](#8-类与对象)
9. [模块系统](#9-模块系统)
10. [内置函数](#10-内置函数)
    - [10.1 内置方法](#101-内置方法)
11. [程序入口](#11-程序入口)
12. [运行时](#12-运行时)
13. [字节码参考](#13-字节码参考)

---

## 1. 词法结构

### 1.1 注释

支持两种注释形式：

```zeta
// 单行注释

/*
   多行注释
*/
```

### 1.2 标识符

标识符由字母、下划线和数字组成，不能以数字开头：

```
[a-zA-Z_][a-zA-Z0-9_]*
```

### 1.3 关键字

```
var    let    fn     return  if     else
while  for    break  continue
class  extends  this
import as    is
true   false  null
```

### 1.4 分隔符与符号

```
;  .  ,  :  ?  (  )  [  ]  {  }
```

---

## 2. 类型系统

Zeta 是动态类型语言。运行时类型如下：

| 类型 | 说明 |
|------|------|
| `Null` | 空值，唯一值为 `null` |
| `Int` | 64 位有符号整数（`int64_t`） |
| `Float` | 双精度浮点数（`double`） |
| `Bool` | 布尔值：`true` / `false` |
| `String` | 不可变驻留字符串（VM 管理，相同内容共享内存） |
| `StrObj` | 堆分配的不可变字符串对象（GC 管理，由拼接或 `input()` 产生） |
| `Array` | 动态数组，索引为整数 |
| `Map` | 键值对容器，键为驻留字符串 |
| `Function` | 函数（一等公民） |
| `Class` | 类 |
| `Instance` | 类的实例 |
| `Iterator` | 迭代器（通常由内置函数 `iter()` 产生） |
| `Error` | 错误标记值，携带一个整数错误码 |

> **String vs StrObj**: `String` 是 VM 级别的驻留字符串（字面量、标识符），`StrObj` 是运行时动态创建的堆字符串（拼接结果、`input()` 返回值）。两者都代表不可变字符串，且在大多数场景下行为一致。Map 键只能用驻留 `String`，如需用 `StrObj` 访问 Map，请先调用 `intern()` 驻留。

---

## 3. 字面量

### 3.1 整数字面量

支持十进制、二进制、八进制、十六进制，可包含 `_` 作为分隔符：

```zeta
42          // 十进制
0b1010      // 二进制
0o777       // 八进制
0xFF        // 十六进制
1_000_000   // 十进制，含分隔符
0b10_10     // 二进制，含分隔符
```

### 3.2 浮点数字面量

```zeta
3.14
0.5
1.0e10
2.5e-3
100_000.5   // 含分隔符
```

### 3.3 字符串字面量

支持双引号和单引号，转义序列包括 `\n`、`\t`、`\r`、`\\`、`\'`、`\"`：

```zeta
"Hello, World!\n"
'single-quoted string'
"包含 \"转义\" 引号"
```

### 3.4 布尔与空字面量

```zeta
true
false
null
```

### 3.5 数组字面量

```zeta
[1, 2, 3]
["a", "b", "c"]
[]
```

> 若所有元素为常量，数组将在编译期求值为常量数组。

### 3.6 映射字面量

键可以是标识符（无需引号）或字符串：

```zeta
{ name: "Alice", age: 30 }
{ "complex-key": "value" }
{}
```

> 若所有值为常量，映射将在编译期求值为常量映射。

### 3.7 函数字面量（Lambda）

```zeta
fn(x, y) { return x + y; }
fn() { print("no args"); }
```

---

## 4. 运算符

按优先级由低到高排列：

### 4.1 赋值运算符（右结合）

| 运算符 | 示例 |
|--------|------|
| `=` | `a = b` |
| `+=` `-=` | `a += b`, `a -= b` |
| `*=` `/=` `%=` | `a *= b` |
| `&=` `|=` `^=` | `a &= b` |
| `<<=` `>>=` | `a <<= b` |

### 4.2 三元条件（右结合）

```zeta
cond ? a : b
```

### 4.3 逻辑运算符

| 运算符 | 示例 | 说明 |
|--------|------|------|
| `\|\|` | `a \|\| b` | 逻辑或（短路） |
| `&&` | `a && b` | 逻辑与（短路） |

### 4.4 关系运算符

| 运算符 | 示例 | 说明 |
|--------|------|------|
| `==` `!=` | `a == b` | 值相等比较（见下方规则） |
| `<` `>` `<=` `>=` | `a < b` | 有序比较 |
| `is` | `a is b` | 严格恒等比较（类型 + 值/指针完全相同） |

#### 4.4.1 `==` / `!=` 比较规则

- **Int 与 Float**: 按数值比较（`0 == 0.0` → `true`）。
- **String 与 StrObj**: 按字符串内容比较。
- **实例（Instance）**: 若类定义了 `_equals` 方法（接收 `this` + 一个参数），则委托给该方法；否则按指针相等比较。
- **其他对象类型**（Array, Map, Function 等）: 按指针相等比较。
- **其他基础类型**: 按类型和值比较。

> 示例：`0 == 0.0` 为 `true`，但 `0 is 0.0` 为 `false`（类型不同）。`"a" + "b" == "ab"` 为 `true`（内容相同）。

### 4.5 移位运算符

| 运算符 | 示例 |
|--------|------|
| `<<` | `a << b` |
| `>>` | `a >> b` |

### 4.6 加减运算符

| 运算符 | 示例 |
|--------|------|
| `+` | `a + b` |
| `-` | `a - b` |

### 4.7 乘除模运算符

| 运算符 | 示例 |
|--------|------|
| `*` | `a * b` |
| `/` | `a / b` |
| `%` | `a % b` |

### 4.8 位运算符

| 运算符 | 示例 |
|--------|------|
| `&` | `a & b` |
| `\|` | `a \| b` |
| `^` | `a ^ b` |

### 4.9 一元运算符（右结合）

| 运算符 | 示例 | 说明 |
|--------|------|------|
| `!` | `!a` | 逻辑非 |
| `~` | `~a` | 按位取反 |
| `-` | `-a` | 算术取负 |

### 4.10 后缀运算符

| 运算符 | 示例 |
|--------|------|
| `.` | `obj.field`, `obj.method()` |
| `[ ]` | `arr[i]`, `map["key"]` |
| `( )` | `func(args)` |

### 4.11 数值类型转换规则

算术运算（`+` `-` `*` `/`）和比较运算（`<` `>` `<=` `>=`）支持 Int 和 Float 混合运算。若任一操作数为 Float，另一操作数自动提升为 Float，结果为 Float。

位运算（`&` `|` `^` `<<` `>>` `~`）和取模（`%`）仅支持 Int 类型。

**字符串拼接**: `+` 运算符支持字符串拼接（String 和 StrObj 均可），结果为新的 `StrObj`。

```zeta
var s = "Hello, " + "World!";   // s 是 StrObj, 内容为 "Hello, World!"
```

### 4.12 真值规则

条件判断（`if`、`while`、条件分支 `JumpIfFalse`/`JumpIfTrue`）中自动进行真值转换：

| 值 | 真值 |
|----|------|
| `null` | false |
| `0` (Int), `0.0` (Float) | false |
| `false` (Bool) | false |
| 非零整数/浮点数、`true` | true |
| 字符串（String / StrObj） | true |
| 对象（Array, Map, Function, Instance 等） | true |
| `Error` | true |

> 注意：此规则同时适用于 `if`/`while` 等语句条件，以及 `!` 逻辑非运算符。

---

## 5. 变量与赋值

### 5.1 变量声明

```zeta
var x;          // 可变变量，初始值为 null
var y = 10;     // 可变变量，带初始值
let z = "hi";   // 不可变变量（常量），声明时必须初始化
```

- `var` 声明的是**可变**变量，可以不提供初始值（默认为 `null`）。
- `let` 声明的是**不可变**变量，必须提供初始值，声明后不可再次赋值。
- 全局变量的初始值必须是常量表达式。

### 5.2 赋值

```zeta
a = 42;
a += 1;         // 等价于 a = a + 1
a -= 1;
a *= 2;
a /= 2;
a %= 3;
a &= 0xFF;
a |= 0x10;
a ^= 0x0F;
a <<= 1;
a >>= 1;
```

赋值目标可以是：

- **标识符**: `x = 10`
- **成员访问**: `obj.field = 10`
- **索引访问**: `arr[0] = 10`, `map["key"] = 10`

---

## 6. 语句

### 6.1 表达式语句

```zeta
print("hello");
1 + 2;
```

### 6.2 块语句

```zeta
{
    var x = 1;
    let y = 2;
    print(x + y);
}
```

块会创建一个新的局部作用域。

### 6.3 条件语句

```zeta
if (x > 0) {
    print("positive");
}

if (x > 0) {
    print("positive");
} else {
    print("zero or negative");
}
```

### 6.4 循环语句

**while 循环**:

```zeta
while (x > 0) {
    x = x - 1;
}
```

**for-in 循环**（遍历可迭代对象）:

```zeta
var arr = [1, 2, 3];
for (item : arr) {
    print(item);
}

var map = {a: 1, b: 2};
for (key : map) {          // 遍历映射返回键（字符串）
    print(key);
    print(map[key]);
}
```

> for-in 循环通过内置函数 `iter()` 和 `next()` 实现。数组遍历返回元素值，映射遍历返回键。当 `next()` 返回 `Error` 时通过 `check()` 检测并终止循环。

**break / continue**:

```zeta
while (true) {
    if (condition) break;
    if (skip) continue;
}
```

### 6.5 返回语句

```zeta
return;         // 等价于 return null;
return expr;
```

每个函数最终都会返回一个值。若未显式提供返回值，函数末尾隐式插入 `return null;`。

---

## 7. 函数

### 7.1 函数声明

```zeta
fn add(a, b) {
    return a + b;
}

fn greet(name) {
    print("Hello, ");
    print(name);
    print("\n");
    // 隐式 return null;
}
```

### 7.2 函数调用

```zeta
var result = add(1, 2);
greet("World");
```

- 函数参数个数不能超过 255。
- 调用时参数个数必须与声明时的形参数目一致，否则为运行时错误。

### 7.3 函数作为一等公民

函数字面量可以赋值给变量、作为参数传递：

```zeta
var twice = fn(x) { return x * 2; };
var result = twice(5);           // 10

fn apply(f, val) {
    return f(val);
}
apply(twice, 10);                // 20
```

---

## 8. 类与对象

### 8.1 类声明

```zeta
class Point {
    var x = 0;
    var y = 0;

    fn init(thisX, thisY) {
        this.x = thisX;
        this.y = thisY;
        return this;
    }
}
```

- 类的成员可以是字段（`var` / `let`）和方法（`fn`）。
- 字段的初始值必须是常量表达式。
- 类体内不允许声明嵌套类。

### 8.2 构造函数

当调用一个类（像调用函数一样）时，将创建该类的实例，并调用与类同名的构造方法：

```zeta
var p = Point(3, 4);
```

流程如下：
1. 用字段默认值创建一个新实例。
2. 如果类定义了与类同名的构造方法，则调用该方法——其中 `this` 被绑定到新实例，第一个参数为 `this`，其余为用户传入的参数。
3. 构造方法应显式 `return this;`。
4. 若没有构造方法，则直接返回新实例。

### 8.3 继承

```zeta
class Point3D extends Point {
    var z = 0;

    fn init(x, y, z) {
        this.x = x;
        this.y = y;
        this.z = z;
        return this;
    }
}
```

- Zeta 支持单继承。
- 基类可以来自其他模块：`class A extends ModuleAlias.BaseClass { ... }`

### 8.4 字段与方法访问

```zeta
var p = Point(1, 2);
p.x = 10;               // 设置字段
print(p.x);             // 读取字段
p.init(5, 6);           // 调用方法
```

### 8.5 `this` 关键字

`this` 只能在方法内部使用，指向调用该方法的当前实例。方法体内 `this` 始终位于局部变量槽位 0。

### 8.6 自定义迭代器与特殊方法

Zeta 支持以下特殊方法：

| 方法 | 签名 | 说明 |
|------|------|------|
| `_iter` | `fn _iter()` | 返回迭代器实例，使对象可用于 for-in 循环 |
| `_next` | `fn _next()` | 返回下一个元素；迭代结束时返回 `error(0)` |
| `_equals` | `fn _equals(other)` | 自定义相等比较；用于 `==` / `!=` 运算符 |

**迭代器示例**:

```zeta
class Range {
    var start = 0;
    var end = 0;

    fn init(start, end) {
        this.start = start;
        this.end = end;
        return this;
    }

    fn _iter() {
        return RangeIter(this.start, this.end);
    }
}

class RangeIter {
    var current = 0;
    var end = 0;

    fn init(current, end) {
        this.current = current;
        this.end = end;
        return this;
    }

    fn _next() {
        if (this.current >= this.end) {
            return error(0);
        }
        var val = this.current;
        this.current = this.current + 1;
        return val;
    }
}

for (i : Range(0, 10)) {
    print(i);
}
```

> 注意：迭代器协议方法名以下划线开头（`_iter`、`_next`），而非双下划线。

**_equals 示例**（自定义相等比较）：

```zeta
class Vec2 {
    var x = 0;
    var y = 0;

    fn init(x, y) {
        this.x = x;
        this.y = y;
        return this;
    }

    fn _equals(other) {
        if (this.x == other.x && this.y == other.y) {
            return true;
        }
        return false;
    }
}

var a = Vec2(1, 2);
var b = Vec2(1, 2);
print(a == b);    // true（通过 _equals 比较）
print(a is b);    // false（不同实例）

---

## 9. 模块系统

### 9.1 模块导入

```zeta
import "foo.zt";          // 导入 foo.zt 中定义的所有全局符号
import "bar.zt" as bar;   // 导入并起别名，通过 bar.name 访问
```

> 不支持重复导入同一模块。

### 9.2 模块查找规则

按以下顺序查找模块文件：

1. 相对于导入模块所在目录。
2. 绝对路径，或相对当前工作目录。
3. VM 配置的模块搜索路径。

### 9.3 可见性

- 当前模块只能访问自己**直接**导入的模块中的符号，无法传递访问间接导入的符号。
- 若同名符号出现在多个导入模块中，使用第一个匹配到的定义。推荐使用别名来避免歧义。
- 模块别名优先于全局符号名（当两者同名时）。

### 9.4 全局符号重定位

模块编译时在局部作用域中找到的符号引用会直接绑定；未找到的符号会被标记为"外部符号"（external symbol），在模块加载时按导入列表中模块的顺序查找并重定位。

---

## 10. 内置函数

内置函数由虚拟机直接实现，调用时无需函数对象查找。

| 函数 | 签名 | 说明 |
|------|------|------|
| `print` | `print(val)` | 将值打印到标准输出。对象类型显示为 `<array>`、`<map>`、`<function>` 等标签；StrObj 打印其字符串内容 |
| `input` | `input()` | 从标准输入读取一个字符串，返回 `StrObj` |
| `iter` | `iter(container)` | 获取数组、映射或实例的迭代器。对于实例，调用 `_iter()` 方法 |
| `next` | `next(iter)` | 获取迭代器的下一个值。数组返回元素值，映射返回键。若迭代完毕返回 `Error` |
| `array` | `array(size)` | 创建指定大小的新数组（元素初始为 `null`） |
| `map` | `map(capacity)` | 创建指定初始容量的新映射 |
| `error` | `error(code)` | 返回携带整数错误码 `code` 的 `Error` 值 |
| `check` | `check(val)` | 若 `val` 不是 `Error` 类型返回 `true`，否则返回 `false` |
| `intern` | `intern(strObj)` | 将 `StrObj` 驻留为 `String`（存入 VM 字符串表）。用于需要驻留字符串的场景（如 Map 键访问） |

> 自定义函数名不能与内置函数同名，否则编译报错。

---

### 10.1 内置方法

以下内置方法通过方法调用语法直接由 VM 实现，无需类定义：

**字符串**（String 和 StrObj）：

| 方法 | 签名 | 说明 |
|------|------|------|
| `len` | `str.len()` | 返回字符串的字符长度（Int） |

```zeta
var s = "hello";
print(s.len());    // 5
```

**数组**（Array）：

| 方法 | 签名 | 说明 |
|------|------|------|
| `size` | `arr.size()` | 返回数组元素个数（Int） |
| `add` | `arr.add(val)` | 向数组末尾添加一个元素，返回 `null` |

```zeta
var arr = [1, 2, 3];
print(arr.size());  // 3
arr.add(4);
print(arr.size());  // 4
```

**映射**（Map）：

| 方法 | 签名 | 说明 |
|------|------|------|
| `size` | `map.size()` | 返回映射中的键值对数量（Int） |

```zeta
var m = {a: 1, b: 2};
print(m.size());    // 2
```

> Map 只能用驻留字符串（`String`）作为键来索引。如果使用 `StrObj`（如 `input()` 或字符串拼接的结果），需要先通过 `intern()` 驻留后再用作键。

## 11. 程序入口

程序从 `main` 函数开始执行。`main` 函数定义为无参函数：

```zeta
fn main() {
    print("Hello, World!\n");
    return 0;
}
```

```bash
$ zeta hello.zt
Hello, World!
```

---

## 12. 运行时

### 12.1 执行模型

Zeta 采用基于栈的字节码虚拟机（VM）执行。编译流程：

```
源代码 (.zt)
  → 词法分析 (Flex)
  → 语法分析 (Bison, LALR(1))
  → AST → 字节码翻译
  → 模块加载 & 符号重定位
  → 执行 (VM)
```

### 12.2 内存管理

采用分代垃圾回收（Generational GC）：

- **新生代（Young Generation）**: 复制算法，分为 Eden 区 + 两个 Survivor 半区。Minor GC 时将 Eden 和存活 Survivor 半区的对象复制到空闲 Survivor 半区。
- **老年代（Old Generation）**: 标记-压缩（Mark-Compact）算法。对象年龄达到阈值（默认 10 次 GC）后晋升至老年代。
- 大对象（> 512 字节）直接在老年代分配。
- 写屏障（Write Barrier）负责追踪老年代到新生代的引用。

### 12.3 字符串驻留

VM 管理一个字符串驻留表（string table），字面量字符串和标识符会被驻留（`String`），相同内容共享同一块内存，因此驻留字符串的相等比较效率为 O(1)。

运行时动态创建的字符串（如 `input()` 返回值、字符串拼接结果）是 `StrObj` 类型，由 GC 管理，不自动驻留。可通过内置函数 `intern()` 将其显式驻留为 `String`。

```zeta
var s1 = "hello";                     // String（驻留）
var s2 = input();                     // StrObj（非驻留）
var s3 = intern(s2);                  // String（驻留）
print(s1 == s2);                      // true（按内容比较）
print(s1 is s2);                      // false（不同类型）
print(s1 is s3);                      // true（同为驻留 String，相同内容）
```

### 12.4 VM 配置

默认配置：

| 参数 | 默认值 |
|------|--------|
| 栈大小 | 1024 KiB |
| 初始堆大小 | 1024 KiB |
| 最大堆大小 | 无限制 (-1) |

### 12.5 编译期常量折叠

编译器在编译期对常量表达式进行折叠（constant folding），包括：

- 整数和浮点数的算术运算（`+` `-` `*` `/` `%`）
- 比较运算
- 位运算
- 逻辑运算（`&&` `||` `!`）
- 三元条件表达式（若条件为常量）
- 数组/映射字面量（若所有元素为常量）

### 12.6 错误处理

编译错误分为三类：

1. **词法错误（Lexical Error）** — 无法识别的字符或词法格式。
2. **语法错误（Syntax Error）** — 不符合语法的程序结构。
3. **语义错误（Semantic Error）** — 重复定义、无效赋值目标、`this` 使用不当等。

运行时错误包括类型错误、除零、越界、参数数量不匹配等。运行时错误会终止程序执行。

---

## 13. 字节码参考

Zeta 编译为自定义字节码。字节码指令的完整说明见 [bytecode.md](bytecode.md)。

指令概览（47 条指令）：

| 类别 | 指令 |
|------|------|
| 内存 | `LoadConst`, `LoadGlobal`, `StoreGlobal`, `LoadVar`, `StoreVar` |
| 算术 | `Add`, `Sub`, `Mul`, `Div`, `Mod`, `Neg` |
| 位运算 | `BitAnd`, `BitOr`, `BitXor`, `BitNot`, `Shl`, `Shr` |
| 逻辑 | `Not` |
| 比较 | `Eq`, `Neq`, `Lt`, `Gt`, `Le`, `Ge`, `Is` |
| 控制流 | `Jump`, `JumpIfFalse`, `JumpIfTrue`, `Ret`, `Call` |
| 对象 | `GetField`, `SetField`, `CallMethod`, `IndexGet`, `IndexSet` |
| 杂项 | `Pop`, `Dup`, `CallBuiltin`, `Nop`, `Halt` |

---

## 语法速查

```
Program      → ImportList DecList

Import       → "import" STR ";"
             | "import" STR "as" ID ";"

Dec          → VarDec
             | FuncDec
             | ClassDec

VarDec       → ("var" | "let") Var ("," Var)*

Var          → ID
             | ID "=" Exp

FuncDec      → "fn" ID "(" [ParamList] ")" BlockStmt

ClassDec     → "class" ID "{" ClassBody "}"
             | "class" ID "extends" ID "{" ClassBody "}"
             | "class" ID "extends" ID "." ID "{" ClassBody "}"

Stmt         → VarDec ";"
             | Exp ";"
             | Exp AssignOp Exp
             | "if" "(" Exp ")" Stmt ["else" Stmt]
             | "while" "(" Exp ")" Stmt
             | "for" "(" ID ":" Exp ")" Stmt
             | "break" ";"
             | "continue" ";"
             | "return" [Exp] ";"
             | BlockStmt
             | ";"

BlockStmt    → "{" StmtList "}"

Exp          → Exp "?" Exp ":" Exp
             | Exp Relop Exp
             | Exp ("&&" | "||") Exp
             | Exp ("<<" | ">>") Exp
             | Exp ("+" | "-") Exp
             | Exp ("*" | "/" | "%") Exp
             | Exp ("&" | "|" | "^") Exp
             | ("-" | "!" | "~") Exp
             | "(" Exp ")"
             | Exp "." ID "(" [ArgList] ")"
             | Exp "." ID
             | Exp "[" Exp "]"
             | ID "(" [ArgList] ")"
             | ID
             | Literal
             | "this"

Literal      → INT | FLOAT | BOOL | "null" | STR
             | "[" [ElemList] "]"            // 数组字面量
             | "{" [MapElemList] "}"          // 映射字面量
             | "fn" "(" [ParamList] ")" BlockStmt  // 函数字面量
```
