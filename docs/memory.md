# 内存子系统

源码：`kernel/src/arch/x86_64/vmm/`（`vmm.cpp` 约 46 KB、`vma.cpp`、`ua.cpp`）、`kernel/src/mem/`（`pmm.cpp`、`heap.cpp`、`new.cpp`、`new2.cpp`）。

## 1. 分层

```text
用户态 mmap / munmap  ← syscall 14/15
        ↓
VMA 管理 (vma.cpp)        区域抽象、缺页处理、扩展/拆分
        ↓
VMM 五级页表 (vmm.cpp)     PML5→PML4→PDPT→PD→PT，巨页映射
        ↓
PMM (pmm.cpp)             物理页帧分配，三级结构、per-CPU 缓存
        ↓
SLUB / SLAB (mem/heap)     小对象内核堆 kmalloc(16..1024B)
```

## 2. VMM：五级页表与巨页

- 启用 **5-level paging**，支持 **1 GB / 2 MB 巨页**；
- **Copy-on-Write**：fork/共享映射时页表项标只读，写触发缺页后动态分配新页；
- **动态巨页拆分**：一个 2 MB 巨页里只有一小段要 CoW 时，拆成普通 4 KB 页处理，拆完再拼回；
- 用户态地址空间与内核 `kernel_pagemap` 隔离，每个进程自己的顶层 PML4；
- `VMM::Alloc(pagemap, npages, is_volatile)` 是最底层分配原语，SMP 启动栈、IST 栈都从这里要。

## 3. TLB shootdown：批量合并

跨 CPU 修改页表后需要发 IPI 让其他核刷 TLB。`vmm.cpp` 里实现了 **per-CPU 批量上下文**：

```c
struct tlb_batch_ctx { ... };
static tlb_batch_ctx g_tlb_batch[MAX_CPU];   // 每 CPU 一份
```

- 一个 CPU 在一段临界区内改了多页，先把这些虚拟地址**攒在本 CPU 的 batch 里**；
- 临界区结束时，**每个目标 CPU 只发一次 IPI**，由它一次刷完整批页，而不是每页一发；
- batch 溢出时退化为逐页 flush（`overflow: collapse to one per-pm flush`）。

这是多核内核里最容易被新手漏掉的扩展性点——逐页 IPI 在 4 核以上会让页表操作变成 IPI 风暴。

## 4. PMM：per-CPU 物理页缓存

README 描述为"三级物理管理器 + 惰性位图初始化 + per-CPU 缓存"：

- 物理页帧用**锁-free 位图 + CAS** 管理，避免全局自旋锁；
- 每个 CPU 维护自己的 per-CPU 页帧缓存，本地分配不碰全局锁；
- 缓存耗尽/过量时才与全局层批量对账（batch drain），把锁争用摊到批处理粒度。

## 5. SLUB / SLAB 内核堆

启动顺序（见 `init.cpp`）有严格依赖：

```text
PMM::Init() → VMM::Init() → SLAB::Init()
   → SLUB::InitKmalloc()   // 16..1024B kmalloc 缓存上线
   → SLUB::SelfTest()      // named cache + kmalloc 融合自检
```

> SLUB 借 SLAB 给自己的 cache descriptor 分配内存，所以 SLAB 必须先活；自检失败会打印 `LastFailStage()` 并回退到 SLAB，而不是直接 hcf。

- `kmalloc`/`kfree`/`kcalloc`/`krealloc` 是内核一切小对象的来源；
- 带 **QSBR**（read-copy 更新风格）的垃圾回收、TLS 批分配；
- 典型用户：ART 树节点、file cache、`cpu_t` 结构、thread/process 描述符。

## 6. 高频原语加速

`ablib/arch/x86_64/x86mem/` 里手写了 `memcpy/memset/memmove/memcmp` 的多版本分发：

- `*_base`：朴素标量版；
- `*_avx` / `*_avx2`：启动时按 CPUID 探测，选最快版本；
- ART 树节点拷贝、合成器扫线 blit、SLUB 批量清零都走这条快路径。

## 7. VMA 与缺页

`vma.cpp`（约 18 KB）管理用户态连续虚拟地址区间：

- 记录 `[start, end)`、权限、背后的文件/匿名映射；
- 缺页异常按区间类型分发：匿名页 → 分配物理页；文件页 → 从页缓存/file cache 取回；CoW 页 → 复制后改映射；
- `ua.cpp`（约 8 KB）处理用户态访问边界——SMAP 开启后，内核碰用户缓冲区要显式开关，这里做统一封装。
