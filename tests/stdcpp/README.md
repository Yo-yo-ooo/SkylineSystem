# tests/stdcpp — STL 独立测试工程

针对 `lib/include/stdcpp` 的语义测试。**刻意不接入任何自动化门禁**：

- 不被 `tests/Makefile` 的 `all` 目标引用
- 不被 `res/scripts/test/ci.sh` 调用
- 不被 `.github/workflows/ci.yml` 触发

只有在本目录显式执行 `make` 才会构建和运行。这样做的原因：这套用例会
主动构造分配失败、异常路径和大量随机数据，属于「库自身的正确性验收」，
与内核回归门禁（slub/fc/sched/memops）关注点不同，混在一起会拉长门禁
时间并让失败归因变模糊。

## 一键运行

```bash
cd tests/stdcpp
make            # gnu++17（内核基线），构建并运行
make cxx20      # gnu++20（用户态标准），覆盖 std::span
make asan       # ASAN+UBSAN（建议在 WSL/Linux 下用）
make clean
```

`make` 之后可带名称过滤：`./../bin/stdcpp_test vector` 只跑名字含 `vector` 的用例。

## 覆盖范围

| 文件 | 主题 |
|---|---|
| `t_00_smoke.cpp` | 类型工具、limits、placement new、框架自检 |
| `t_vector.cpp` | 元素生命周期、扩容、insert/erase 全重载、自引用、OOM 回滚 |
| `t_string.cpp` | SSO、c_str、自引用 append/insert、查找族、数值转换、string_view |
| `t_sequence.cpp` | list / forward_list / deque 的增删改查、排序、splice |
| `t_assoc.cpp` | map/set/multimap/multiset 顺序性与**树平衡性**、unordered_* |
| `t_algorithm.cpp` | 排序与参考实现比对、二分族、堆、排列、集合运算 |
| `t_memory.cpp` | allocator、unique_ptr、shared_ptr/weak_ptr 引用计数 |
| `t_functional.cpp` | 函数对象、std::function、hash、tuple、apply |
| `t_adaptors.cpp` | array / span / stack / queue / priority_queue / inplace_vector / flat_* / hive |
| `t_allocator.cpp` | 有状态分配器：POCCA / POCMA / POCS 传播、SSO 与分配器交互 |
| `t_atomic.cpp` | 4 线程真实并发：fetch_add / CAS / release-acquire / atomic_flag |
| `t_numbers_ratio.cpp` | `<numbers>` 常量恒等式；`<ratio>` 编译期约分/四则/比较/SI 词头 |
| `t_complex.cpp` | `<complex>` 四则、模/幅角/共轭/极坐标、exp/log/pow/sqrt/三角双曲，与内建基准比对 |
| `t_random.cpp` | mt19937 / mt19937_64 标准测试向量；分布统计特征；`generate_canonical` |
| `t_regex.cpp` | ECMAScript 子集语义：交替回溯、整串匹配下的懒惰量词、空循环终止、长输入不爆栈 |

## 两条关键设计

**1. 分配器可观测**

框架替换了全局 `::operator new/delete`（见 `framework/test_framework.cpp`），
而 stdcpp 的所有容器统一经 `detail/alloc.h` → `::operator new` 分配，因此：

- 每个用例结束后自动统计净泄漏（用例抛异常/提前 return 也能捕获）
- `tst::alloc_fail_after(n)` 可让第 n+1 次分配抛 `bad_alloc`，用来验证
  容器在扩容失败时是否遗留泄漏或破坏原有元素

这也是为什么头文件里不能直接调 `malloc`：绕开 `operator new` 就绕开了这层
观测，同时内核侧根本没有 `malloc`。

**2. 不用被测容器写测试**

框架自身只用 POD 数组和 `printf`，不用 `std::vector`/`std::string` 存用例，
避免「容器有 bug 导致测试结果不可信」。

**3. 复杂度也用断言锁住**

`vector` 批量插入、`hive` 空洞复用都用「元素操作次数 / 容量不变」来断言，
而不是只看结果对不对——O(n·size) 的实现同样能算对结果，只是慢。

## 并发用例的依赖

`t_atomic.cpp` 需要 pthread（`-pthread` 已在 Makefile 里）。若平台没有
`<pthread.h>`（例如某些 Windows 工具链），该文件会自动降级为一个
"skipped" 用例，不会导致构建失败。

## 与标准版本的对应关系

部分头文件按标准版本条件暴露（与真实标准库一致）：

| 头文件 | 起始标准 | 本工程中的覆盖方式 |
|---|---|---|
| `<span>` | C++20 | `make cxx20` |
| `<flat_map>` / `<flat_set>` | C++23 | `-std=gnu++23` / `gnu++26` |
| `<inplace_vector>` / `<hive>` | C++26 | `-std=gnu++26` |

`gnu++17` 构建下这些用例被 `#if __cplusplus` 自动跳过，因此同一份源码
在三种标准下都能编过。
