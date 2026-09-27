# 内存子系统

源码：`kernel/src/arch/x86_64/vmm/`（`vmm.cpp`、`vma.cpp`、`ua.cpp`）、`kernel/src/mem/`（`pmm.cpp`、`heap.cpp`、`new.cpp`）。

> 本文档只描述代码实际状态，包括与旧宣传稿不一致的地方。

## 1. 分层

```text
用户态 mmap / munmap  ← syscall 14/15
        ↓
VMA 管理 (vma.cpp)        区域抽象、缺页处理、扩展/拆分
        ↓
VMM 页表 (vmm.cpp)        4 级页表(PML4→PDPT→PD→PT)、巨页映射
        ↓
PMM (pmm.cpp)             物理页帧分配(全局锁 + per-CPU 缓存)
        ↓
SLUB / SLAB (mem/heap)     小对象内核堆 kmalloc(16..1024B)
```

## 2. VMM：4 级页表与巨页

- 构建配置 `CONFIG_VMM_5LVL_MAP = 0`，实际跑在 **4 级分页**；5 级路径被编译条件排除（早期曾因 `IsPM5LVL` 语义写反导致用户态地址上界误用 5 级常量，已修复并统一到 4 级上界）；
- 支持 **1 GB / 2 MB 巨页**分配与映射；
- **Copy-on-Write**：fork/共享映射时页表项标只读 + COW 位，写触发缺页后分配新页。**粒度限制**：2 MB 巨页触发 CoW 时整页复制（不拆 4K）；仅 1 GB 页会在缺页时拆为 2 MB；**不存在"拆完再拼回"**（旧文档此句不实）；
- 用户态地址空间与内核 `kernel_pagemap` 隔离（每个进程的 PML4 复制内核高半区条目）；
- `VMM::Alloc/EAlloc/Free` 是内核侧分配原语；`sys_munmap` 已按 length 做 VMA 局部拆分释放。

## 3. TLB shootdown：批量合并

- per-CPU 批量上下文（`g_tlb_batch[MAX_CPU]`）：临界区内多次失效先攒批，提交时每个目标 CPU 只发一次 IPI；
- 批次溢出时退化为**单次全刷**（并非逐页回退——旧文档措辞不准）；
- `DestroyPM` 释放 pagemap 前会等待各远程 shootdown 队列排空（**lite 版**：自旋上限 + 日志；per-CPU 完成计数仍未实现，理论窗口只是收窄而非关闭）；
- PCID 路径下 `cpus_with_tlb` 只增不减，跨核失效会广播给历史上跑过该地址空间的所有核（与"反 IPI 风暴"的理想有差距）。

## 4. PMM：物理页管理

- 位图帧分配器由**单一全局 `pmm_lock`** 串行化（旧宣传"锁-free 位图 + CAS"不实；代码里没有 CAS 位图）；
- 每 CPU 维护一个小页缓存：**单页请求**本地命中时不碰全局锁；大页（2MB/1GB）请求在全局锁下从低地址线性扫描；
- 释放路径对 per-CPU 缓存已有基本防护；内核/reserved 区域在初始化时正确剔除。

## 5. SLUB / SLAB 内核堆

启动顺序严格依赖：`PMM::Init() → VMM::Init() → SLAB::Init() → SLUB::InitKmalloc() → SLUB::SelfTest()`；自检失败打印 `LastFailStage()` 并回退 SLAB。

- 真实 slab 家族：16..1024B 尺寸类、per-CPU magazine、页内空闲链（XOR cookie）、CAS Treiber 空闲栈；
- `krealloc/kcalloc` 有溢出检查；SLUB `Free` 对 **双重释放**（`inuse==0`）检测并 `slab_fatal` 拒绝（SLAB 侧原有同款防护）；
- **QSBR 不在这里**：deferred reclamation 只存在于**用户态分配器** `lib/stdc/allocator/allocator.c`，内核堆没有宽限回收（旧文档张冠李戴，已更正）。

## 6. 高频原语加速

`ablib/arch/x86_64/x86mem/` 提供 `memcpy/memset/memmove/memcmp` 多版本（base/AVX/AVX2，启动时按 CPUID 选择）。该目录源自第三方 x86mem 库，许可归属见文件头。

## 7. VMA、缺页与用户/内核拷贝

- `vma.cpp` 记录 `[start, end)`、权限与红黑树索引；缺页按匿名/文件/CoW 分发；
- `ua.cpp`（`CopyToUser/CopyFromUser`）强制 `MM_USER` 校验并采用 4 级用户半区上界 —— 此前存在"用户态经 `0x800000000000+p` 别名读写任意物理内存"的漏洞，已修复；
- 缺页对 CoW 页授予写权限时基于 COW 位判断（对只读映射 fork 后写会新建页）。

## 8. 安全现状

- **NX/W^X**：EFER.NXE 已按 CPUID 启用；ELF 装载完成后按段 `p_flags` 收紧（不可写去 W、不可执行加 NX）。此前 NXE 未启用时 NX 位是保留位，设置即 #PF；
- **KASLR**：仅由引导器 `-fPIE/-pie` 提供装载随机化，内核自身无随机化逻辑；构建后 `make kaslr-check` 校验无 32 位绝对重定位残留；
- SMEP/SMAP 已启用；内核访问用户缓冲区走 `ua.cpp` 统一封装。
