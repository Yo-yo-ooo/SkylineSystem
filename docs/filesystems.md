# 文件系统栈

源码：`kernel/src/fs/`（`fc.cpp` 文件缓存、`fd.cpp` 描述符、`saf/`、`fatfs/`、`lwext4/`）、`kernel/src/drivers/`（块设备）。

## 1. 整体栈



```
syscall FOPEN/FREAD/FWRITE/... (0..5)

\&#x20;       ↓

fd.cpp        分片红黑树 fd 分配器(线性探测)

\&#x20;       ↓

VFS / mount    hashmap 挂载点解析

\&#x20;       ↓

┌───────┬──────────────┬───────────┐

│ SAF   │ lwext4 (ext4) │ fatfs     │

│ 只读  │  磁盘上的真实文件系统 │ FAT(空壳) │

│ 归档  │  (可读写日志) │ 未接入     │

└───────┴──────────────┴───────────┘

\&#x20;       ↓

块设备抽象 (blockdev / Disk\\\_Interfaces)

\&#x20;       ↓

NVMe / AHCI / USB MSC（ATA/ATAPI/RAM disk 未注册）
```

## 2. 支持的文件系统



| 文件系统     | 角色                      | 实现                                                                                                       |
| -------- | ----------------------- | -------------------------------------------------------------------------------------------------------- |
| **ext4** | 根文件系统 `/mp/`，挂在 `sata0` | 移植 [lwext4](https://github.com/gkostka/lwext4)，自带 journal/extent/bitmap                                  |
| **FAT**  | 可移动介质 / 互操作（**当前不可用**） | 移植 fatfs，但 `diskio.cpp` 胶水层为空壳（显式返回 RES_ERROR，见 README 特性表） |
| **SAF**  | 启动期 / 资源打包格式（只读归档）      | 移植自 [chocabloc/SAF](https://github.com/chocabloc/saf)，见 `fs/saf/`；镜像见 `programs/programs.saf`、`res/saf/` |

启动时 `ext4_kernel_init("sata0", "/mp/", 0)` 失败直接 `hcf()`—— 根文件系统挂不上就不继续。

### SAF 归档格式

SAF（Simple Archive Format）是一个 **read-only 的树形归档**，性质接近 initramfs：整个镜像就是一棵带 `offset` 的目录树，内核不需要块设备就能直接按路径读文件。它**不是 Skyline 自研格式**，而是移植自 [chocabloc/SAF](https://github.com/chocabloc/saf)（见根 README Thanks 列表）。

镜像布局（`fs/saf/saf.h`）：



```
\#define MAGIC\_NUMBER 0x766863726c706d73   // 每个节点头校验

// 节点头：magic | len | name\[256] | flags(bit0=IS\_FOLDER)

// 文件节点：hdr + size + addr(内容在镜像内的偏移)

// 目录节点：hdr + num\_children + children\[]\(子节点偏移数组)
```



* `initrd_mount(image)` 把整段镜像包成一个 VFS `initrdMount`，挂到 VFS 上；

* `initrd_find(path, base, cur)` 递归沿 `children[]` 偏移树查路径；

* `open/read/dir_at` 等回调全部走 VFS 接口，和 ext4/FAT 在同一层被统一调度；

* 用途：把启动早期还没挂磁盘前就要用的 ELF、字体、资源打包进 ISO/HDD 镜像。

## 3. per-CPU 文件缓存（fc.cpp）

`file_cache_cpu_t` 在每个 CPU 启动时分配（`smp.cpp` 里 BSP 与 AP 都做），带一个写回回调：



```
int32\\\_t file\\\_cache\\\_writeback\\\_callback(const uint8\\\_t \\\*key, uint32\\\_t key\\\_len,

\&#x20;                                    void \\\*data, size\\\_t data\\\_len);
```



* 每个核缓存自己近期访问的文件块，减少重复 NVMe/AHCI 读；

* 脏页回写通过注册的回调（`file_cache_writeback_callback`，返回值契约已修复为 0=成功/负=失败）执行——但**当前 `is_dirty` 恒假，写回机制尚未被真正激活**（写入走 write-through 直接落盘 + 跨核失效广播）；

* 命中路径已检查条目状态（INVALID 条目不再命中）、写后跨核失效已补上（陈旧读已修）；CRC 只覆盖前 256 字节；

* 与 per-CPU 物理页缓存、per-CPU SLAB 空闲链一致 ——**争用下沉到核本地，跨核只在批对账时相遇**。

## 4. 块设备接口

`drivers/Disk_Interfaces/` 把 "一块可随机读写的盘" 抽象成统一接口，上层文件系统不关心底下是哪种总线：



* `sata/`：SATA 磁盘接口；

* `ram/`：内存盘（实现存在但**从未注册**，勿按"无盘启动用"理解）；

* 真实总线驱动：`ahci/`、`nvme/`、USB `msc`（`ata/`、`atapi/` 为未注册死代码）。

## 5. 分区

`kernel/src/partition/`：`mbrgpt.cpp`（MBR/GPT 识别）、`identfstype.cpp`（探测每个分区上的文件系统类型）、`mgr.cpp`（分区管理器）。

## 6. 当前状态



* ext4/SAF 已通，能挂载并加载 `desktop.elf` / `hw.elf`；**FAT 未接入**；

* lwIP vendored 但**从未初始化**（编译进镜像、无调用者），见 README 特性表；

* **分区层休眠**：`USE_VIRT_IMAGE` 使分区管理器旁路偏移计算，ext4 实际按裸 LBA 访问（MBR/GPT 解析存在但未生效）。

* 真机磁盘兼容性在快速迭代中 ——README 明确警告**暂勿在真机上跑**。