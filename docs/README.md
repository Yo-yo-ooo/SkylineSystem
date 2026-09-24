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
| [scheduler.md](./scheduler.md)       | **3EVDF / Rate-aware EEVDF**：RIP 速率反馈、动态量子、负载均衡    | `kernel/src/arch/x86_64/schedule/`               |
| [memory.md](./memory.md)             | 五级页表、巨页 + CoW、SLUB 内核堆、per-CPU 缓存、批量 TLB shootdown | `kernel/src/arch/x86_64/vmm/`、`kernel/src/mem/`  |
| [smp.md](./smp.md)                   | AP bring-up、per-CPU 数据段、GS base、XSave、SMAP/SMEP    | `kernel/src/arch/x86_64/smp/smp.cpp`             |
| [syscall.md](./syscall.md)           | 非 POSIX 系统调用 ABI 参考（全量 17 号）                       | `kernel/include/arch/x86_64/schedule/syscalln.h` |
| [gui.md](./gui.md)                   | CPU 并行软件合成器、窗口层、光标层、SDF 圆角与阴影                      | `programs/desktop/`                              |
| [filesystems.md](./filesystems.md)   | VFS / SAF /lwext4 / FAT、per-CPU 文件缓存               | `kernel/src/fs/`                                 |
| [commit.md](./commit.md)             | 提交信息约定（已有）                                         | —                                                |

## 设计哲学速记

SkylineSystem **不追 POSIX/Linux ABI**，所有接口按 "一个现代桌面内核该长什么样" 从头设计。核心取舍：



* **深度优先**：x86\_64 一条架构做穿，aarch64/RISC-V 只留构建模板。

* **机制与策略分离**：窗口装饰、合成策略、shell 全在用户态；内核只暴露共享帧、线程、sysinfo 这些最小机制。

* **可剥夺性优先**：用户态就算在 `for(;;)` 里死循环，鼠标和屏幕合成仍然流畅 —— 这是 3EVDF 和独立光标层存在的全部理由。