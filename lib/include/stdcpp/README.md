# stdcpp — SkylineSystem 的 freestanding C++ 标准库

`lib/include/stdcpp/` 是一套**头文件实现**的 C++ 标准库子集，供内核与用户态
C++ 程序共用。本文档记录真实状态、设计约束与关键语义的实现方式。

---

## 一、构建前提

| 场景 | 编译选项 | 来源 |
| :--- | :--- | :--- |
| 内核 | `-std=gnu++17 -nostdinc -ffreestanding -fno-exceptions -fno-rtti` | `kernel/Makefile` |
| 用户态 | `-std=gnu++20 -nostdinc -nostdlib -fno-threadsafe-statics` | `programs/*/Makefile` |

因此本库有两条硬约束：

1. **不得假设宿主 libc**。没有 `<string.h>`、`<stdio.h>`、`malloc`、`strtol`、
   `snprintf`。字符/内存原语一律用 `__builtin_memcpy/memmove/memset/memcmp/strlen`，
   数值转换在头内实现。
2. **不得无条件使用异常**。`-fno-exceptions` 下连 `try/catch` 都是编译错误
   （不只是 `throw`）。所有失败路径统一走 `detail/throw.h` 的
   `STDCPP_THROW` / `STDCPP_TRY` / `STDCPP_CATCH_ALL` / `STDCPP_RETHROW`。

`<cstddef>`、`<cstdint>`、`<climits>`、`<cfloat>`、`<cstdarg>`、`<cstdalign>`
只是转发编译器自带的 freestanding 头（`ablib/freestndchdrs/`）。

---

## 二、内存模型（重要）

**所有容器都经 `::operator new` / `::operator delete` 分配，不再直接调
`malloc`。** 统一入口在 `detail/alloc.h`：

```
容器 → __detail::alloc_bytes_aligned(n, align) → ::operator new
```

原因有三：

- 内核没有 `malloc`（只有 `kmalloc`），直接调 `malloc` 会导致内核链接失败；
- 绕开 `operator new` 会丢掉内核的 OOM 诊断（`kernel/src/mem/new.cpp`）；
- 测试里替换 `::operator new` 后可以精确统计每次分配、注入失败，从而验证
  容器的异常安全与泄漏。

平台需要提供的符号：

| 平台 | `operator new/delete` 定义位置 |
| :--- | :--- |
| 内核 | `kernel/src/mem/new.cpp` |
| 用户态 | `lib/stdc/stdcpp_new.cpp` |

过度对齐（`alignof(T) > max_align_t`）用「多分配 + 前置原始指针」实现，
不依赖 C++17 的 `operator new(size_t, align_val_t)`（内核未提供该重载）。

---

## 三、状态表

标注说明：
- **Verified** — 有 `tests/stdcpp` 用例覆盖并通过（C++17/20/26 三档）
- **Compiles** — 能在 freestanding + 无异常模式下编过，缺深度用例
- **Partial** — 接口子集，缺部分标准成员或有简化实现
- **Absent** — 未提供

### 语言支持

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<cstddef>` | Verified | 含 `std::byte` 全套位运算（旧版漏了类型本身） |
| `<cstdint>` | Verified | 定宽类型 + `intptr_t`/`uintptr_t`/`intmax_t`/`uintmax_t`（旧版缺这四个） |
| `<climits>` `<cfloat>` `<cstdarg>` `<cstdalign>` | Verified | 转发 freestanding 头；`<cstdalign>` 在 C++20 起退化为空实现而非 `#error` |
| `<new>` | Verified | placement/nothrow/`bad_alloc`/`align_val_t`；旧版 nothrow 版非 inline 会多重定义 |
| `<type_traits>` | Verified | 常用 traits + `is_nothrow_*` |
| `<initializer_list>` | Verified | 编译器内建支撑 |

### 工具

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<utility>` | Verified | `move/forward/swap/pair/index_sequence/exchange` |
| `<tuple>` | Verified | `make_tuple/tie/apply/get/tuple_size` |
| `<functional>` | Verified | 算术/比较/逻辑函数对象、`reference_wrapper`、`hash`、`std::function` |
| `<memory>` | Verified | `allocator`、`unique_ptr`、`shared_ptr`/`weak_ptr`、`make_*`、`addressof` |

> 修复记录：`static_pointer_cast` 曾用裸指针重新包一个控制块（同一对象两个
> 引用计数 → 二次析构），现改为标准别名构造；`weak_ptr::lock()` 曾无法编译。

### 迭代器与算法

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<iterator>` | Compiles | 标签、traits、`reverse_iterator`、`move_iterator`、插入器 |
| `<algorithm>` | Verified | 排序/二分/堆/排列/集合运算/划分等；本轮补齐约 30 个缺失接口 |
| `<numeric>` | Absent | 未提供（`accumulate` 等） |

### 容器

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<vector>` | Verified | 重写；批量插入 O(size+n)、分配器传播、元素生命周期、自引用、OOM 回滚均有用例 |
| `<string>` / `<string_view>` | Verified | 重写；真 SSO、自引用安全、分配器传播、浮点格式化正确舍入 |
| `<array>` | Verified | 含比较运算符（比较逻辑改为内联循环，不再依赖 `<algorithm>`） |
| `<list>` | Verified | 含 `sort/reverse/unique/splice` |
| `<forward_list>` | Verified | 含 `sort/reverse/remove/insert_after` |
| `<deque>` | Verified | 分块存储；修复区间 `erase` 因裸指针迭代器导致的错删 |
| `<map>` `<set>` `<multimap>` `<multiset>` | Verified | 红黑树；用「比较次数」用例验证未退化为链表 |
| `<unordered_map>` `<unordered_set>` | Verified | 链地址哈希；`calloc` 改为内部清零分配 |
| `<stack>` `<queue>` `<priority_queue>` | Verified | 适配器 |
| `<span>` | Verified (C++20) | |
| `<flat_map>` `<flat_set>` | Verified (C++23) | `flat_map` 重写为单个有序 `vector<pair<Key,T>>` |
| `<inplace_vector>` | Verified (C++26) | 固定容量 |
| `<hive>` | Verified (C++26) | 重写；块内空闲链 + 全局空闲块链，插入/删除 O(1)，空块即时回收 |
| `<mdspan>` | Compiles | 最小实现 |

### 数学 / 数值

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<ratio>` | Verified | 编译期有理数：编译期约分（gcd）、四则、比较、SI 词头 typedef；乘法按 `__int128` 中间量做溢出检测 |
| `<numbers>` | Verified (C++20) | `pi_v/e_v/sqrt2_v/...` 及其 `float`/`long double` 变体；内联变量地址唯一 |
| `<complex>` | Verified | `abs/sqrt/exp/log/pow` 与三角/双曲；`abs`/`sqrt` 用自包含牛顿迭代，不依赖 libm |
| `<random>` | Verified | mt19937 / mt19937_64 与标准测试向量逐值一致；LCG、SWC、seed_seq、适配器、常用分布、`generate_canonical` |

### 文本

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<regex>` | Verified | ECMAScript 子集：分组/捕获/交替/贪婪与懒惰量词/字符类/锚点/`\b`/反向引用/icase；迭代式回溯匹配 |

### 并发

| Header | 状态 | 说明 |
| :--- | :--- | :--- |
| `<atomic>` | Verified | 基于 `__atomic_*`；4 线程真实并发压测（fetch_add / CAS / release-acquire / atomic_flag 自旋锁） |
| `<thread>` `<mutex>` `<condition_variable>` `<future>` | Absent | 依赖内核调度器接口，未提供 |

### 未提供

`<iostream>` 系列、`<locale>`、`<filesystem>`、`<chrono>`、`<valarray>`、
`<exception>`（异常基类在 `<stdexcept>` 内）、`<ranges>`、`<execution>`、
`<scoped_allocator>`、`<any>`。

`<chrono>` 依赖 steady_clock 等时间源，内核侧用 `timer` 子系统直接读数，
暂不引入；`<regex>` / `<random>` / `<complex>` / `<ratio>` / `<numbers>` 已实现。

内核侧没有流式 I/O 需求（用 `kprintf`），用户态也没有终端抽象，因此流库
不在范围内。

---

## 四、关键语义的实现方式

这一节记录几个容易"看起来能跑、其实语义不对"的点，以及当前实现的做法。

### 1. `vector` 批量插入是 O(size+n)

`insert(pos, n, v)` 与 `insert(pos, first, last)` 采用标准的两分支原地算法
（与 libstdc++ `_M_fill_insert` 同构）：

```
elems_after > n :  末尾 n 个 move 到 [end, end+n)，整体后移 [idx, end-n)，再赋值
elems_after <= n:  末尾构造 n-elems_after 个新值，[idx, end) move 到新末尾之后，再赋值
```

要点是"已构造区用赋值、未初始化区用 placement new"不能混；异常时回滚已构造
区间。测试用元素拷贝/移动计数断言：500 元素插入 100 个，操作数必须 < 5000
（逐个插入的旧写法约为 5 万次）。

### 2. 浮点 `to_string` 与 `printf("%.6f")` 逐字符一致

走精确路径，不做浮点估算：

1. 把 `v` 拆成 `m × 2^e`，**只用乘/除以 2 的幂**（对二进制浮点精确），
   不依赖 `frexpl` 等 libm 符号；
2. `v × 10^6 = m × 5^6 × 2^(e+6)`，用二进制大整数精确表示；
3. 需要右移时按 **ties-to-even** 舍入（与 printf 默认舍入模式一致）；
4. 十进制输出后按小数点插在倒数第 6 位。

测试拿 3000 组随机位模式 + 次正规数/`DBL_MAX`/`1e308` 与 `snprintf` 对拍。

### 3. `hive` 的空洞复用与块回收都是 O(1)

每块自带槽位空闲链（`next_free[]` + `free_head`）与高水位 `hi`；全局再维护
一条"有空闲槽的块"双向链。于是 `insert` 取链头空洞、`emplace_back` 用尾块、
`erase` 把槽位压回块内链，都是 O(1)。块内元素清空时整块摘链释放，`erase`
的返回值取**解链后**的后继块（先算 next 再 delete 会返回悬垂迭代器）。

### 4. 分配器传播（POCCA / POCMA / POCS）

`allocator_traits` 会**探测分配器自己声明的策略**（旧实现写死成 `false_type`，
等于永远不传播）：

| 场景 | 行为 |
| :--- | :--- |
| 拷贝构造 | `select_on_container_copy_construction(other.alloc)` |
| 拷贝赋值 | POCCA 为真且分配器不同 → 先用对方分配器重建，成功后接管；旧缓冲区用旧分配器释放 |
| 移动赋值 | POCMA 为真或分配器相等 → 窃取缓冲区；否则退化为逐元素搬（不能跨分配器释放） |
| `swap` | POCS 为真 → 连分配器一起换；否则断言两者相等 |

`vector` 与 `string` 均已实现；测试用带 id 与计数器的有状态分配器验证
"用 A 分配、用 B 释放"这类错误不会发生。

### 5. `<regex>` 的匹配引擎是迭代式的

不用递归回溯，有两个在内核里不可接受的理由：

1. 递归深度随输入长度增长（`a*` 匹配 10 万字符 = 10 万层栈帧）；
2. 循环体能匹配空串时（`(a*)*`）会无限递归直到栈溢出。

改用显式栈，栈上放三类撤销记录（备选分支 / 回滚捕获槽 / 回滚空循环哨兵），
失败时按 LIFO 弹出：先回滚捕获，遇到备选分支就换路继续。空循环哨兵记录
"每个 Split 上次进入时的 sp"，同一 sp 再次进入同一 Split 说明循环体吃了空串，
此时只走退出分支。

另外两个编译器侧容易写错的地方（都有用例锁住）：

- 交替**必须**发 `Split`，只发 `Jmp` 会把 `cat|dog` 编译成 `catdog`；
- 最后一支**不能**发 `Split(分支, End)`，否则分支失败时跳到 End 等于"跳过整个
  分组"，`a(b|c)d` 就会错误地匹配 `ad`。

### 6. `<complex>` 不依赖 libm

内核没有 libm，`sqrt`/`abs` 用牛顿迭代自实现（见 `detail/math.h`）；
`exp/log/三角/双曲` 走复数公式展开到实数运算，同样只用内建算术。
尾数宽度按 `__LDBL_MANT_DIG__` 选择，避免无条件依赖 `__int128`。

### 7. `<atomic>` 有真实并发验证

不再是"能编过"：4 线程 × 10 万次 `fetch_add` 总数必须精确；CAS 循环除总数
正确外还断言重试次数 > 0（证明真的在竞争）；release/acquire 交接、`atomic_flag`
自旋锁保护非原子计数器各有独立用例。

---

## 五、测试

```bash
cd tests/stdcpp
make          # gnu++17
make cxx20    # gnu++20
```

该工程**不接入 CI**（不被 `tests/Makefile`、`ci.sh`、`.github/workflows/ci.yml`
引用），原因与覆盖范围见 `tests/stdcpp/README.md`。

当前规模：gnu++17 115 例 / gnu++20 116 例 / gnu++26 124 例，全部通过。

验证方式不止「能跑」：

- 每个用例结束自动检查净泄漏（框架替换了 `::operator new`）
- `alloc_fail_after(n)` 注入 OOM，验证扩容失败后容器仍自洽
- 关联容器用「查找比较次数」断言树未退化
- 排序类算法与朴素参考实现逐项比对，并校验多重集合一致性
- 浮点格式化与 `snprintf("%.6f")` 对拍 3000 组随机位模式
- 批量插入用元素操作计数断言复杂度，而不是只测结果
- `<atomic>` 用 4 线程真实并发压测
- `<random>` 用标准测试向量核对 mt19937 / mt19937_64 的前若干个输出
- `<regex>` 覆盖交替回溯、整串匹配下的懒惰量词、空循环不终止、长输入不爆栈
- 有状态分配器验证传播语义（分配器 id 与分配/释放计数配对）

## 六、内核使用注意事项

`kernel/include/mem/new.hpp` 为了让 `new` 带上调用点信息，定义了
`#define new (...)` 宏。**在包含该头之后再包含 STL 头，placement new 会被
宏改写。** 内核代码若要使用本库，请先包含 STL 头，或在包含前后
`#undef new`（`kernel/src/mem/new.cpp` 已有同类处理）。
