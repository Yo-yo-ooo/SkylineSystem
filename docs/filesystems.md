# Filesystem stack

Source: `kernel/src/fs/` (`fc.cpp` file cache, `fd.cpp` descriptors, `saf/`, `fatfs/`, `lwext4/`), `kernel/src/drivers/` (block devices).

## 1. Overall stack



```
syscall FOPEN/FREAD/FWRITE/... (0..5)

\&#x20;       ↓

fd.cpp        sharded red-black-tree fd allocator (linear probing)

\&#x20;       ↓

VFS / mount    hashmap mount-point resolution

\&#x20;       ↓

┌─────────────┬────────────────────────────┬────────────────┐

│ SAF         │ lwext4 (ext4)              │ fatfs          │

│ read-only   │ real on-disk filesystem    │ FAT (stub)     │

│ archive     │ (journaled read/write)     │ not wired up   │

└─────────────┴────────────────────────────┴────────────────┘

\&#x20;       ↓

block-device abstraction (blockdev / Disk\\\_Interfaces)

\&#x20;       ↓

NVMe / AHCI / USB MSC (ATA/ATAPI/RAM disk unregistered)
```

## 2. Supported filesystems



| Filesystem | Role                                                           | Implementation                                                                                                              |
| ---------- | -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| **ext4**   | root filesystem `/mp/`, mounted on `sata0`                     | ported from [lwext4](https://github.com/gkostka/lwext4), with its own journal/extent/bitmap                                 |
| **FAT**    | removable media / interoperability (**currently unavailable**) | ported fatfs, but the `diskio.cpp` glue layer is a stub (explicitly returns RES_ERROR; see the README feature table)        |
| **SAF**    | boot-time / resource packaging format (read-only archive)      | ported from [chocabloc/SAF](https://github.com/chocabloc/saf), see `fs/saf/`; images at `programs/programs.saf`, `res/saf/` |

At boot, if `ext4_kernel_init("sata0", "/mp/", 0)` fails it goes straight to `hcf()` — if the root filesystem cannot be mounted, boot does not continue.

### The SAF archive format

SAF (Simple Archive Format) is a **read-only tree archive**, similar in nature to initramfs: the whole image is a directory tree with `offset` values, and the kernel can read files by path directly without a block device. It is **not a Skyline-original format** but a port of [chocabloc/SAF](https://github.com/chocabloc/saf) (see the Thanks list in the root README).

Image layout (`fs/saf/saf.h`):



```
\#define MAGIC\_NUMBER 0x766863726c706d73   // per-node header check

// node header: magic | len | name\[256] | flags(bit0=IS\_FOLDER)

// file node: hdr + size + addr(content offset within the image)

// directory node: hdr + num\_children + children\[]\(array of child-node offsets)
```



* `initrd_mount(image)` wraps the whole image into a VFS `initrdMount` and mounts it on the VFS;

* `initrd_find(path, base, cur)` walks the `children[]` offset tree recursively to resolve paths;

* the `open/read/dir_at` callbacks all go through the VFS interface and are dispatched at the same layer as ext4/FAT;

* purpose: pack the ELFs, fonts and resources needed before the disk is mounted early in boot into the ISO/HDD image.

## 3. per-CPU file cache (fc.cpp)

`file_cache_cpu_t` is allocated when each CPU starts (done for both the BSP and the APs in `smp.cpp`), with a writeback callback:



```
int32\\\_t file\\\_cache\\\_writeback\\\_callback(const uint8\\\_t \\\*key, uint32\\\_t key\\\_len,

\&#x20;                                    void \\\*data, size\\\_t data\\\_len);
```



* each core caches the file blocks it accessed recently, cutting repeated NVMe/AHCI reads;

* dirty-page writeback runs through the registered callback (`file_cache_writeback_callback`, whose return contract has been fixed to 0=success/negative=failure) — **the mechanism exists but is not activated in production** (writes go write-through straight to disk + cross-core invalidation broadcast; consistent with fc-semantics.md: file_cache_fsync has no callers);

* the hit path already checks entry state (INVALID entries no longer hit), cross-core invalidation after write has been added (stale reads fixed); CRC covers only the first 256 bytes;

* consistent with the per-CPU physical-page cache and the per-CPU SLAB free lists — **contention is pushed down to the local core; cores only meet during batched reconciliation**.

## 4. Block-device interface

`drivers/Disk_Interfaces/` abstracts "a disk that can be read/written randomly" into one uniform interface, so upper-layer filesystems don't care which bus sits underneath:



* `sata/`: SATA disk interface;

* `ram/`: RAM disk (the implementation exists but is **never registered**; do not read it as "for diskless boot");

* real bus drivers: `ahci/`, `nvme/`, USB `msc` (`ata/`, `atapi/` are unregistered dead code).

## 5. Partitioning

`kernel/src/partition/`: `mbrgpt.cpp` (MBR/GPT identification), `identfstype.cpp` (probes the filesystem type on each partition), `mgr.cpp` (partition manager).

## 6. Current status



* ext4/SAF work and can mount and load `desktop.elf` / `hw.elf`; **FAT is not wired up**;

* lwIP is wired up (e1000 82574L driver + DHCP/ICMP ping/TCP, brought online late via the network stack), see network.md;

* **the partition layer is dormant**: `USE_VIRT_IMAGE` makes the partition manager bypass offset computation, and ext4 actually accesses raw LBAs (MBR/GPT parsing exists but is not in effect).

* real-machine disk compatibility is under rapid iteration — the README explicitly warns **not to run on real hardware for now**.
