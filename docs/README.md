# SkylineSystem 文档中心

本目录是 SkylineSystem 的**深度设计文档**。根目录 [README.md](../README.md) 负责 "这是什么、怎么编译、怎么跑起来"；这里负责 "它内部到底怎么设计的、为什么这么设计"。

> 所有文档基于 
>
> `kernel/`
>
>  实际源码写成，不是宣传稿。出现 
>
> `v3 FIX`
>
>  一类注释时，意味着这块历史上真的踩过坑。

## 文档地图



| 文档                                   | 内容                                                 | 对应代码                                             |
| ------------------------------------ | -------------------------------------------------- | ------------------------------------------------ |
| [architecture.md](./architecture.md) | 整体分层、目录布局、从 Limine 到首个 ELF 的启动时序                   | `kernel/src/arch/x86_64/init.cpp`                |
| [scheduler.md](./scheduler.md)       | deadline 键调度 + **RIP 速率反馈**（只调节时间片长度、**不改 CPU 份额**）、负载均衡    | `kernel/src/arch/x86_64/schedule/`               |
| [memory.md](./memory.md)             | 4 级页表、巨页 + CoW、SLUB 内核堆、per-CPU 缓存、批量 TLB shootdown、W^X | `kernel/src/arch/x86_64/vmm/`、`kernel/src/mem/`  |
| [smp.md](./smp.md)                   | AP bring-up、per-CPU 数据段、GS base、XSave、SMAP/SMEP    | `kernel/src/arch/x86_64/smp/smp.cpp`             |
| [syscall.md](./syscall.md)           | 非 POSIX 系统调用 ABI 参考（0-16 基础号，内核共注册 25 个槽位）                       | `kernel/include/arch/x86_64/schedule/syscalln.h` |
| [gui.md](./gui.md)                   | CPU 并行软件合成器（N-1 workers）、窗口层、光标层、SDF 圆角与阴影，含已知缺陷                      | `programs/desktop/`                              |
| [filesystems.md](./filesystems.md)   | VFS / SAF /lwext4 / FAT、per-CPU 文件缓存               | `kernel/src/fs/`                                 |
| [network.md](./network.md)           | e1000 82574L 驱动 + lwIP 移植 (DHCP/ping/多核 TX/统计), 移植修正史与已知边界 | `kernel/src/drivers/net/`, `kernel/include/lwip/` |
| [dpdk-lite.md](./dpdk-lite.md)       | DPDK-lite 数据面设计 (轮询/零拷贝/卸载/多队列) + 零拷贝审计 | 同上 |
| [fc-semantics.md](./fc-semantics.md) | 文件缓存完整语义审计 (writeback/脏页/fsync/OOM/权限) | `kernel/src/fs/fc*.cpp` |
| [goal-status.md](./goal-status.md)   | 目标完成度总账 (三部分目标 + 生产级八支柱 + 六项遗留债) | — |
| [stability-report.md](./stability-report.md) | 长稳报告 (30 分钟 soak 等实测数据) | — |
| [stability-audit.md](./stability-audit.md) | 稳定性审计 (故障注入/恢复路径) | — |
| [release.md](./release.md)           | 发布流程 (构建/宿主/冒烟/长稳门禁矩阵 + 版本约定) | — |
| [commit.md](./commit.md)             | 提交信息约定（已有）                                         | —                                                |
| [../SECURITY.md](../SECURITY.md)     | 安全现状 (架构级机制/明确边界/已知风险) 与漏洞报告流程 | — |

## 设计哲学速记

SkylineSystem **不追 POSIX/Linux ABI**，所有接口按 "一个现代桌面内核该长什么样" 从头设计。核心取舍：



* **深度优先**：x86\_64 一条架构做穿；aarch64/RISC-V/LoongArch 仅有构建参数，**端口未实现**。

* **机制与策略分离**：窗口装饰、合成策略、shell 全在用户态；内核只暴露共享帧、线程、sysinfo 这些最小机制。

* **可剥夺性优先**：用户态就算在 `for(;;)` 里死循环，鼠标和屏幕合成仍然流畅 —— 这主要靠**独立光标层**与合成器结构保证；调度器的 RIP 速率反馈只调节时间片长度（详见 scheduler.md 的诚实描述）。