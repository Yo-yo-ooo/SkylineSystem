# 启动卡死在 VMM 初始化 —— 根因与修复

## 症状

`make cm` 全量重建后，内核启动到下面这里就再无任何输出，**没有任何异常打印**：

```
[ OK ] PMM Initialised!
[ OK ] IRQ 49 Installed
        ← 死在这里
```

WSL 的 QEMU 8.2 和 Windows 的 QEMU 10.1 都一样；`-m 1G/2G/4G`、`-smp 2/4`、
`-cpu max/qemu64` 全部复现。WHPX 硬件加速不可用（QEMU 自身崩：
`WHPX: Unexpected VP exit code 4`）。

## 定位过程

在 `VMM::Init()` 里逐层插桩，最后一步落在：

```
[vmmit] pre n=1025 lo=80000000 hi=ffff800080000000
[vmmit]  lo ok n=1025                     ← Map2M(0x80000000) 正常
[vmmnl] entry=2 pmreq=74000
[vmmnl] read ok probe=250                 ← 该页**可读**
[vmmnl] w0 ok / w1 ok / w4095 ok          ← 单独写 3 个字节**正常**
                                          ← 整页清零 → 死
```

即 `VMM::Internal::NewLevel()` 里对 `PMM::Request()` 返回页的清零。
`CR3=0x7FF71000`、RSP=`0xFFFF80007FF81EE8`、PMM 位图区间
`0x7DA7C000..0x7FA7C000`，三者都与被清零的 `0x74000` 无关。

**关键判据**：该页"可读、写几个字节没事、整页清零才死"。这说明不是映射缺失
（缺页会走 `[#PF]` 并打印），也不是死循环，而是**清零把仍在使用的映射本身
卸掉了**：取下一条指令立刻缺页，而缺页处理程序所在的映射也一并消失 →
double/triple fault → QEMU 带 `--no-shutdown` 时直接停机，看起来就是"卡死
且无输出"。

换句话说：**PMM 把 bootloader 自己页表占用的物理页当成空闲页发了出去。**

## 走过的弯路（记录以免重犯）

1. 先怀疑"映射越过 RAM"，`-m 4G` 一样炸 → 否掉。
2. 怀疑 CPU 特性，`-cpu qemu64` 一样炸 → 否掉。
3. 怀疑 PMM 元数据被覆盖：打印 `pmm_bitmap_start/size`，区间在
   `0x7DA7C000..0x7FA7C000`，与 `0x74000` 无交集 → 否掉。
4. 怀疑 RSP 落在该页（启动栈被清零）：RSP 在 `0xFFFF80007FF81EE8` → 否掉。
5. 尝试**精确遍历 CR3**、把页表页逐页标为已用 —— 直接 `#PF`：
   `CR2 == CR3 == 0x7FF71000`。原因是 bootloader 的页表页所在物理内存
   **并没有被映射进当前地址空间**（CPU 用物理地址走页表，不需要映射），
   软件无法读取，此路不通。

## 修复

在 `PMM::Init()` 里把 memmap 中 `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE`
的整段标记为已分配：

```cpp
static void reserve_bootloader_memory() {
    for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
        struct limine_memmap_entry* e = pmm_memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) continue;
        if (e->length == 0) continue;
        uint64_t first = e->base >> 12;
        uint64_t count = (e->length + PAGE_SIZE - 1) >> 12;
        if (first >= pmm_bitmap_pages) continue;
        if (count > pmm_bitmap_pages - first) count = pmm_bitmap_pages - first;
        mark_allocated(first, count);
    }
}
```

调用点在 `mark_allocated(0, 1);` 之后、`free_pages` 统计**之前** ——
顺序很关键，否则空闲页计数会把它们又算回去。

这是保守做法：会浪费几 MB（本系统本来也没有 reclaim 回收器），但换来
"绝不把活页表发出去"的硬保证。精确做法（只保留真正被 CR3 引用的页）需要
能读到那些页，而上面第 5 条已证明做不到。

## 验证

修复后同一镜像、同一 QEMU 参数启动：

```
dynlink DT_NEEDED      : 2      ← 动态链接生效
dynlink TLS layout     : 2
desktop main reached   : 1
compositor up          : 1      ← 桌面起来了
hw syscall check       : 1      ← 动态链接版 hw.elf 正常
PROC->ID               : 3
Exit PROC 7 (want 0)   : 0      ← 桌面没有自杀
Segfault (want 0)      : 0
unresolved symbol      : 0
```

## 仍未解决：ext4 bcache 红黑树被破坏（内核 #PF @ NULL+0x70）

修好 PMM 之后系统能走到桌面，但很快出现内核缺页并 `hcf()`：

```
Segfault: unmapped (addr=0000000000000070 rip=FFFFFFFF8002B516)
KERNEL FAULT AT CPU RSVD INT :14
CR2: 0x70     RFLAGS: 0x10007 (IF=0)
On thread 6
```

`addr2line` 还原出的调用链（内核 PIE，bias=0）：

```
syscall_entry
 └ syscall_handler                                  syscall.cpp:47
   └ sys_fopen                                      fops.cpp:374
     └ ext4_fopen2                                  ext4.cpp
       └ ext4_generic_open2                         ext4.cpp:994
         └ ext4_dir_find_entry                      ext4_dir.cpp:512
           └ ext4_block_get_noread                  ext4_blockdev.cpp:254
             └ ext4_bcache_alloc                    ext4_bcache.cpp:246
               └ RB_REMOVE(ext4_buf_lru, ...)       ext4_bcache.cpp:245
                 └ RB_REMOVE_COLOR  ← NULL + 0x70
```

`0x70` 正是 `struct ext4_buf` 里树节点字段的偏移，即**对一个 NULL 父节点做旋转**。

### 判断

`ext4_bcache_find_get()` 里这段的前提是 lwext4 自己的不变量：

```c
if (!buf->refctr) {              /* 未被引用 ⇒ 一定在 lru_root 里 */
    buf->lru_id = ++bc->lru_ctr;
    RB_REMOVE(ext4_buf_lru, &bc->lru_root, buf);   /* ← 这里炸 */
}
```

不变量（见 `ext4_bcache.cpp` 注释）：未被引用的 buf 同时在 `lba_root` 和 `lru_root`
里，被引用的只在 `lba_root` 里。一旦某个 buf 的 `refctr` 归零却没有被插回
`lru_root`，`RB_REMOVE` 就会操作一棵它不在其中的树，把红黑树指针改坏。

已排除"垃圾数据"这一路：`ext4_buf_alloc()` 用 `ext4_calloc(1, ...)` 分配 buf，
节点字段是清零的，不是未初始化内存。

**最可能的原因是多 CPU 并发**：`kernel/src/arch/x86_64/schedule/syscall/fops.cpp`
里**完全没有锁**，而 4 个 CPU 上的多个进程会同时进入 ext4 的
`ext4_bcache_alloc` / `RB_INSERT` / `RB_REMOVE`。lwext4 本身是单线程设计，
共享的 `ext4_bcache` 被并发改红黑树必然损坏。旁证：崩溃前紧邻一条
`TLB shootdown: ACK timeout`，同样是并发路径上的症状。

### 下一步（按优先级）

1. 在 syscall 层给 ext4 访问加**全局串行化**：`fops.cpp` 的
   `sys_fopen/fread/fwrite/fclose/fseek/fstat/readdir` 外层取一把
   irqsave 自旋锁，或做一个可重入的 ext4 大锁。
2. 加防御：给 `ext4_buf` 增加 `in_lru` 标志，`ext4_bcache_find_get` 里
   只在 `in_lru` 为真时才 `RB_REMOVE`，避免一棵树被删第二次。
3. 复现手段：多进程（桌面 + notepad + doom）同时打开文件时最易触发。

## 已修复：ext4 并发损坏（用 lwext4 自带的 os_locks）

上面第 1 条的**正确做法比预想的干净得多**：lwext4 本来就留了挂载点锁接口
（`EXT4_MP_LOCK` / `EXT4_MP_UNLOCK` 宏，走 `mp->os_locks` 回调），
`ext4_mount_setup_locks()` 就是安装它的函数。

而本内核**从来没有调用过它** —— `mp->os_locks` 恒为 NULL，那些加锁宏全是
空操作。这就是"4 个 CPU 并发改同一棵 bcache 红黑树"的直接原因。

修复（`kernel/src/fs/lwext4/src/ext4.cpp`）：

- 在 `ext4_kernel_init()` 里 `ext4_mount()` 之后调用
  `ext4_mount_setup_locks(mpname, &g_ext4_locks)`；
- 回调 `ext4_os_lock/unlock` 实现成**递归**锁：lwext4 的公开函数之间会
  互相调用（`ext4_fopen2` → `ext4_generic_open2`），非递归自旋锁会自死锁。
  用 "owner = 当前 CPU + 1，配深度计数" 实现，只有最外层真正拿锁/放锁，
  `irqsave` 的 flags 也只在最外层保存（否则内层会把中断状态恢复错）。

### 验证（3 次连续启动，结果完全一致）

| 检查项 | 修复前 | 修复后 |
|---|---|---|
| `Segfault` / `KERNEL FAULT` | 1（NULL+0x70） | **0** |
| `STALL WATCHDOG` | 多次 | **0** |
| `LOCK STALL WATCHDOG` | 有 | **0** |
| `TLB shootdown ACK timeout` | 0~3 | **0** |
| `Ext4 MP LOCK INSTALLED` | —— | **1** |
| `[doom] init done` | 0（崩溃打断） | **1** |
| `compositor up` | 1 | **1** |
| `dynlink DT_NEEDED` | 2 | **2** |
| `Exit PROC 7` | 0 | **0** |

`[doom] init done` 从"永远到不了"变成稳定复现，是最直观的回归指标：
DOOM 初始化要连续打开/读取 WAD 的多个数据段，正好是最能压 ext4 并发的场景。

顺带说明：`TLB shootdown ACK timeout` 也一并消失了。它此前一直被认为是独立的
既有问题（每轮 0~3 条），现在看来是同一根因的副作用 —— ext4 在无锁状态下
长时间占用 CPU / 关中断，导致远端核回 ACK 不及时。


