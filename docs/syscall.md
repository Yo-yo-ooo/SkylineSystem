# 系统调用 ABI（非 POSIX）

源码：`kernel/include/arch/x86_64/schedule/syscalln.h`、`kernel/src/arch/x86_64/schedule/syscall.cpp`、`syscall/*.cpp`。

> *SkylineSystem&#x20;*
>
> *不兼容 Linux/POSABI*
>
> *。下表是当前全部 17 个系统调用，按号排列。具体参数寄存器约定见&#x20;*
>
> `lib/base/arch/x86_64/syscall.c`
>
> *&#x20;与各&#x20;*
>
> `syscall/*.cpp`
>
> *&#x20;实现；本文档只固定*
>
> *号与语义*
>
> *。*

## 1. 调用约定



* 入口走 MSR `LSTAR`（`syscall_init()` 安装）；

* 调用号在 `rax`，参数按 System V 风格依次通过寄存器传递；

* 返回值在 `rax`，负数为错误码；

* 内核态处理在关中断 / 自旋锁保护下完成，返回时 `swapgs` 切回用户 GS。

## 2. 全量表

### 文件 I/O（0–5）



| 号 | 名称       | 语义           |
| - | -------- | ------------ |
| 0 | `FOPEN`  | 打开路径，返回文件描述符 |
| 1 | `FWRITE` | 按 fd 写       |
| 2 | `FREAD`  | 按 fd 读       |
| 3 | `FCLOSE` | 关闭 fd        |
| 4 | `FLSEEK` | 移动文件偏移       |
| 5 | `FSIZE`  | 查询文件大小       |

> VFS 层：hashmap 挂载点解析 + 位图式 fd 分配器（O (1) 尾插）。见 
>
> filesystems.md
>
> 。

### 进程 / 线程（6–13）



| 号  | 名称              | 语义                                     |
| -- | --------------- | -------------------------------------- |
| 6  | `THREAD_LAUNCH` | 在当前进程内起一个新线程                           |
| 7  | `GETTID`        | 当前线程 id                                |
| 8  | `GETPID`        | 当前进程 id                                |
| 9  | `EXIT`          | 线程 / 进程退出                              |
| 10 | `PMMAP`         | 映射一段物理 / 设备内存到用户地址空间                   |
| 11 | `YIELD`         | 自愿让出 CPU（3EVDF 下一般不再需要，但保留）            |
| 12 | `LOAD`          | 加载 ELF 镜像                              |
| 13 | `LAUNCH`        | 创建新进程并启动 ELF（`/mp/desktop.elf` 就是这么起的） |

> 进程 / 线程描述符挂在 
>
> `pid2proc_tree`
>
> （一棵 ART 基数树）上，由 
>
> `PID2PROC_TREE_LOCK`
>
>  保护；
>
> `NOT_RUNQ_P`
>
>  是不在运行队列里的线程集合。

### 内存（14–15）



| 号  | 名称       | 语义                        |
| -- | -------- | ------------------------- |
| 14 | `MMAP`   | 用户态 mmap（匿名 / 文件映射 / 共享帧） |
| 15 | `MUNMAP` | 解除映射                      |

> 背后是 VMA 层（
>
> `vma.cpp`
>
> ）+ 五级页表 + CoW，见 
>
> memory.md
>
> 。

### 系统信息（16）



| 号  | 名称        | 语义                                             |
| -- | --------- | ---------------------------------------------- |
| 16 | `SYSINFO` | 向用户态暴露在线 CPU 数、内存布局、特性位等 —— 合成器靠它知道该起几个 worker |

## 3. 设计取舍



* **故意薄**：没有 signal  delivery（`syscall/signal.cpp` 目前是空壳）、没有 socket 全家桶、没有 futex—— 策略都往用户态推。

* **机制最小化**：内核给 "线程 + 内存映射 + 文件 + sysinfo" 四样，窗口系统、合成器、shell 全在用户态拼。

* **号段稳定**：新调用往 17 往后加，不重排已有号，避免用户态 ELF 重编后跑飞。