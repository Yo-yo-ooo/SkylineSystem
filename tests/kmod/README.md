# tests/kmod/ — kmod 子系统宿主测试门禁

把 `kernel/src/arch/x86_64/kmod/` 的**真实源码**搬到 WSL/Linux 宿主编译运行，
对私有契约型可加载内核模块（kmod）与内建组件（builtin component）机制层做
正确性 + 负向 + fuzz + ASAN 门禁。同一份加载器/拓扑/能力/治理/组件代码在宿主
上跑，内核构建零影响。

## 一键复现

```bash
cd tests
make -B kmod && ./bin/kmod_test          # 普通构建（-O2）
make -B kmod-asan && ./bin/kmod_test_asan # ASAN+UBSAN（ELF 负向/fuzz 必跑）
```

QEMU 串口冒烟门禁（Windows，需先构建 ISO，与 `net_smoke.ps1` 同为手动门禁）：

```powershell
pwsh -File res/scripts/test/kmod_smoke.ps1 [-Root <repo>] [-Qemu <qemu.exe>] [-WaitSeconds 90] [-MinBuiltins 1]
```

> 宿主机负载高时完整启动 + KMOD_SELFTEST 可能 >120s；独立验收用的是
> `-WaitSeconds 200 -MinBuiltins 30`。若 a)/(c) 断言失败且日志停在 selftest
> 中途，先增大 WaitSeconds 而不是判缺陷。

输出协议（CI/金样可 grep）：

```
KMOD-TEST <name> RUN
KMOD-TEST <name> PASS|FAIL|SKIP
KMOD-TEST RESULT PASS x/y [ (skipped: ...) ]
```

`topology` / `capability` / `elf_loader` / `component` 是核心门禁：缺失（源文件
未落地或测试未链接）时汇总为 `INCOMPLETE` 并退出 1，避免门禁假绿；`gov` /
`syscall` 由各自实现任务交付，缺失时 `SKIP` 不算失败。全部执行通过退出 0，
否则 1。可选命令行参数按测试名筛选：`./bin/kmod_test elf_loader`。

## 构建结构

| 文件 | 作用 |
|---|---|
| `Makefile` | 存在性/依赖过滤收集内核源码与测试；fixture 规则；`kmod`/`kmod-asan` 目标 |
| `host_platform.cpp` | `kmod_platform.h` 宿主实现：mmap RWX 分配（镜像可执行）、记账、8-bucket 锁、steady_clock；`.hostbuiltin.desc` 宿主描述表替身 |
| `test_main.cpp` | 唯一 `main`：weak 符号测试表、PASS/FAIL/SKIP 协议、汇总与退出码 |
| `test_common.h` | 断言宏、固定种子 RNG、fixture 读取、最小 ELF64 解析/改字节工具 |
| `topology_test.cpp` | `KmodTopoSort` 随机 DAG 先序性质、环/自环/重名/外部依赖；版本/命名/段名 |
| `capability_test.cpp` | STRICT/OPEN/DISABLED、可信清单交集、required 缺口、RESERVED_CORE/未定义位 |
| `elf_loader_test.cpp` | `KmodElfInspect/LoadImage/ValidateManifest/FreeImage`：正例、结构/清单负向、2000 次 fuzz |
| `gov_test.cpp` | 治理事务层：CheckLoadable 矩阵、导出指纹、注册表门禁、prepare 失败零残留 |
| `syscall_test.cpp` | 固定契约表/治理槽、动态命名空间注册、INFO 快照、座位注册表 |
| `component_test.cpp` | 内建组件模型：条目解析正负向、`KmodModuleAllocBuiltin`、描述表 Init/BootLoad、BUILTIN unload 拒绝、热替换失败回退 |
| `fixtures/` | 模块作者视角的 `fixture_a.o`（provider）/`fixture_b.o`（consumer）/`fixture_hostcomp_v2.o`（热替换回退） |

宿主构建只编 `host_platform.cpp` + `kernel/src/arch/x86_64/kmod/` 下的
`elf_loader/topology/capability/governance/syscall_registry/seat/kernel_exports/kmod_pool/builtin`；
**不编** `platform_kernel.cpp`、`kmod_core.cpp`、`selftest.cpp`（依赖内核设施）。
`kmod_core.cpp` 的全局注册表由 `host_platform.cpp` 提供 weak 兜底；
`builtin.cpp` 依赖的链接脚本符号 `__kbuiltin_desc_start/__kbuiltin_desc_end`
由宿主 `.hostbuiltin.desc` 段提供（默认 1 个空槽位，测试通过
`kmod_host_builtin_set_raw()` 写入合成描述符）。

## 新增测试的约定

- 每个 `tests/kmod/*_test.cpp` **不得有 main**，提供
  `extern "C" int32_t test_<name>_run(void)`，返回 0 = PASS；失败时先
  `printf("  FAIL ...")` 再返回 1（可用 `test_common.h` 的 `KCHECK*`）。
- 在 `test_main.cpp` 的 `kTests[]` 登记 `{name, test_<name>_run, required}`；
  `required=false` 时缺失只 SKIP。
- Makefile 会自动收集 `*_test.cpp`；依赖内核源文件的测试请在该文件的过滤段
  增加"实现存在才编入"的条件。

## fixtures 的两个硬约束

- **`-mcmodel=large` + `-fno-pic -fno-pie`**：宿主 mmap 地址通常 >4GiB，小代码
  模型会产生 `R_X86_64_32S/32` 重定位，加载器按设计拒绝。
- 宿主 g++/as 默认会生成 SHF_ALLOC 的 `.note.gnu.property`（CET/IBT 属性）：
  fixture **刻意保留**该段，加载器已显式跳过 `SHT_NOTE` alloc 段，正好用真实
  工具链输出覆盖该路径（内核模块用 clang 构建，默认无该段）。

## 随机种子（固定、可重放）

| 套件 | 种子 |
|---|---|
| topology 随机 DAG | `0x6b6d6f64`（"kmod"） |
| elf_loader fuzz（2000 次） | `0xdeadc0de` |

## 覆盖清单（elf_loader 负向，均断言负错误码 + 无泄漏）

截断（10 种长度）/坏 magic/坏 class/坏 endian/坏 ident version/ET_EXEC/EM_386/
shoff 越界·错位/shnum=0·超限/shentsize/shstrndx/段对齐>16/alloc 段类型非法/
未知重定位类型/rela entsize 非法/manifest magic·abi·tier·flags·name·version·
budget·段大小/`.in`·`.out` 大小非整除/导入槽越界/导出重名/契约命名空间非法/
导出字符串越界/未定义符号（kexp 缺失或缺项）/重复失败不泄漏。

## component_test 覆盖点

条目解析：BEGIN/END 哨兵缺失、非整除、条目数超限、begin>=end、坏 manifest
（magic/abi/name）；`KmodModuleAllocBuiltin` 正向落成（BUILTIN 标记/能力/条目
布局）+ 重名/坏描述符/保留能力位拒绝；描述表 `KmodBuiltinInit` 坏描述符全量
预校验 + `KmodBuiltinBootLoad` 拓扑化装载（kernel 导入回填、导出可调用、
READY、幂等）；`KmodGovUnload` 对 BUILTIN 返回 `KMOD_ERR_POLICY` 且状态不变；
`fixture_hostcomp_v2.o`（同名 .ko、init 故意失败）热替换失败后完整回退：
内建组件恢复 1.0.0/READY/BUILTIN、旧导出仍可调用、新镜像无泄漏。

## CI 接入

- `res/scripts/test/ci.sh`：`== build kmod == / == run kmod == / ASAN ==` 段，
  日志 `ci_kmod.log` / `ci_kmod_asan.log`。
- `.github/workflows/ci.yml` 的 `host-suites` job：两个 step（普通 + ASAN）。
- `res/scripts/test/kmod_smoke.ps1`：QEMU 无头串口冒烟（Windows 手动门禁，
  断言 KMOD_SELFTEST PASS / builtin ready 数量 / demo init / 无异常 / desktop）。
- `tests/Makefile`：`kmod` / `kmod-asan` 委托目标（细节在本目录 Makefile）。
