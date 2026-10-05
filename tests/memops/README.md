# tests/memops — ablib::x86mem 宿主测试与基准

针对 `ablib/arch/x86_64/x86mem/` 下 `memcpy / memmove / memset / memcmp`
四个分层实现 (SSE4.2 / AVX / AVX2 / AVX512F) 的宿主侧验证。

## 构建与运行

```sh
make -C tests memops         # 正确性测试  -> tests/bin/memops_test
make -C tests memops-bench   # 吞吐基准    -> tests/bin/memops_bench
make -C tests memops-asan    # ASAN 变体   -> tests/bin/memops_asan
```

四个 tier 会被全部编译并 `ld -r` 合成胖对象; 运行期由 CPUID + `MEMOPS_SupportVn`
决定实际跑哪几个 tier (宿主不支持 AVX512F 时 V3 只做编译/链接校验)。
`bin/memops_shim.o` 提供 `MEMOPS_SupportVn` 的强定义, 原因见 `memops_shim.c`
头部注释 —— PE-COFF 下 `ld -r` 会把重复的 weak 定义改名而不留全局符号。
`memops-asan` 依赖 `ld -r` 合并 16 个对象, 只在 ELF (Linux/WSL) 下可用。

## 正确性测试覆盖 (`main.cpp`)

| 项 | 策略 |
| --- | --- |
| memcpy | n = 0..300 穷举 × dst/src 偏移 {0,1,2,3,5,7,8,15,16,17,31,32,33,63} 全组合 |
| memmove | 同上规模, delta = dst-src 覆盖 -64..+64 (含完全重叠/自搬), 另加 4K/64K/1M 大块重叠 |
| memset | n = 0..300 × 14 种偏移 × 6 个填充值 (含 0 / 0x80 / 0xFF) |
| memcmp | n = 1..300 × 14×14 偏移组合; 每个 (n, 偏移) 再在首/中/尾各注入正负两种差异 |
| 大尺寸 | 4K / 64K / 1M / 4M / 5M |
| NT 阈值扫描 | 把 `x86mem_cache_limit` 钉成 {64K, 4K, L3/8, SIZE_MAX}, 强制大块操作分别走 / 不走 streaming store |
| 随机模糊 | 400 轮随机长度/偏移/多处差异, 与 libc 对照 |

越界检测靠两侧各 16 字节哨兵 + 全程覆盖校验; 重叠用例把整块工作区与参考实现
逐字节比对, 任何越界写都会体现为差异。契约项: `memcpy/memmove/memset` 必须返回
原始入参, `memcmp` 与 libc 同号。

## NT 阈值是运行时变量

`x86mem_cache_limit` 不再是编译期常量: `x86mem_init_cache_limit()` 用 CPUID
探测 L3 容量 (leaf 4, 回退 AMD Fn8000_0006_EDX[31:18]), 取
`clamp(L3 / X86MEM_CACHE_LIMIT_L3_DIV)` 填进去。启动代码在两处调用它:

- 用户态 `lib/base/arch/x86_64/init.c` → `_init_runtime_and_global_variables()`
- 内核态 `kernel/src/arch/x86_64/init.cpp` → `x86_64_init()` (SSE 初始化之后,
  早于 PMM/VMM/SLAB 的大块 memset)

测试开头会打印 `默认 -> L3 -> 采用` 并校验采用值等于 `clamp(L3 / DIV)`;
探测失败时校验保持默认值。
`memops_shim.c` 只补 `MEMOPS_SupportVn` 的强定义, 阈值变量本身来自
x86mem 自己的 `x86mem_init.c` (只有 base tier 出符号, 见该文件头部注释)。

## 基准 (`bench.cpp`)

各 tier 与 libc 在 16B..4MB 十个尺寸上的 GB/s, 每格取 3 轮最好值。
两个陷阱已在代码里处理:

* 被测函数必须通过 **volatile 函数指针** 调用, 否则 GCC 会把参数循环不变、
  且编译器认为无副作用的调用 (典型是 `memcmp` 包装) 提举出循环, 测出物理上
  不可能的天文数字。
* `memcmp` 测量前必须把 A/B 的前 n 字节重新对齐 —— 前面的 `memset`/`memmove`
  已经改过 A, 不重新对齐的话 memcmp 在第 0 字节就返回, 测到的是调用开销。

## 旧实现对照

```sh
bash tests/memops/run_old.sh
```

从 `git HEAD` 取出旧版四个 .c 编成同一套测试二进制, 用来确认新实现修掉的缺陷
在旧版上确实可复现 (旧版实测 ~5.6 万次断言失败 / 新版 0)。
