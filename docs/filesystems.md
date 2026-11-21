# Filesystem stack

Source: `kernel/modules/common/fs/` (`fc.cpp` file cache, `fd.cpp` descriptors, `saf/`,
`fatfs/`, `lwext4/`), `kernel/modules/common/drivers/` (block devices).

## 1. Stack

```
syscall FOPEN/FREAD/FWRITE/... (0..5)
        ↓
fd.cpp        sharded red-black-tree fd allocator (linear probing)
        ↓
VFS / mount    hashmap mount-point resolution
        ↓
┌─────────────┬────────────────────────────┬────────────────┐
│ SAF         │ lwext4 (ext4)              │ fatfs          │
│ read-only   │ real on-disk filesystem    │ FAT (stub)     │
│ archive     │ (journaled read/write)     │ not wired up   │
└─────────────┴────────────────────────────┴────────────────┘
        ↓
block-device abstraction (blockdev / Disk_Interfaces)
        ↓
NVMe / AHCI / USB MSC (ATA/ATAPI/RAM disk unregistered)
```

## 2. What is supported

| Filesystem | Role | Implementation |
|---|---|---|
| ext4 | root, `/mp/`, mounted on `sata0` | ported from [lwext4](https://github.com/gkostka/lwext4), with its own journal, extent and bitmap code |
| FAT | removable media / interoperability, currently unavailable | fatfs is ported, but the `diskio.cpp` glue is a stub that returns RES_ERROR; see the feature table in the root README |
| SAF | boot-time resource packaging (read-only archive) | ported from [chocabloc/SAF](https://github.com/chocabloc/saf), see `fs/saf/`; images at `programs/programs.saf`, `res/saf/` |

At boot, if `ext4_kernel_init("sata0", "/mp/", 0)` fails the kernel goes straight to `hcf()`. No root
filesystem, no boot.

### SAF

SAF (Simple Archive Format) is a read-only tree archive, close in spirit to initramfs: the image is a
directory tree with offset values, so the kernel can read a file by path without a block device. It's a
port of [chocabloc/SAF](https://github.com/chocabloc/saf), not an original format — see the Thanks list
in the root README.

Image layout (`fs/saf/saf.h`):

```
#define MAGIC_NUMBER 0x766863726c706d73   // per-node header check

// node header: magic | len | name[256] | flags(bit0=IS_FOLDER)
// file node: hdr + size + addr(content offset within the image)
// directory node: hdr + num_children + children[](array of child-node offsets)
```

- `initrd_mount(image)` wraps the image into a VFS `initrdMount` and mounts it.
- `initrd_find(path, base, cur)` walks the `children[]` offset tree recursively to resolve a path.
- `open/read/dir_at` callbacks all go through the VFS interface, dispatched at the same layer as
  ext4 and FAT.
- The point is to carry the ELFs, fonts and resources needed before the disk is mounted inside the
  ISO or HDD image.

## 3. The per-CPU file cache (fc.cpp)

`file_cache_cpu_t` is allocated when each CPU comes up — BSP and APs both, in `smp.cpp` — with a
writeback callback:

```c
int32_t file_cache_writeback_callback(const uint8_t *key, uint32_t key_len,
                                      void *data, size_t data_len);
```

- Each core caches the file blocks it touched recently, which cuts repeated NVMe/AHCI reads.
- Dirty-page writeback goes through the registered callback
  (`file_cache_writeback_callback`; its return contract is 0=success, negative=failure). At the time
  this section was written the mechanism existed but wasn't on the production path — writes were
  write-through plus cross-core invalidation broadcast, and `file_cache_fsync` had no callers. The
  later audit in [fc-semantics.md](fc-semantics.md) found `sys_fclose` and process exit do flush, so
  that file is the current answer where the two disagree.
- The hit path checks entry state (INVALID entries no longer hit), and cross-core invalidation after a
  write was added, which fixed stale reads. The CRC only covers the first 256 bytes.
- Same shape as the per-CPU page cache and per-CPU SLAB free lists: contention is pushed down to the
  local core, and cores only meet during batched reconciliation.

## 4. Block devices

`drivers/Disk_Interfaces/` reduces "a disk you can read and write at random" to one interface, so
filesystems never care which bus is underneath.

- `sata/`: SATA disk interface.
- `ram/`: RAM disk. The implementation exists but is never registered; don't read it as diskless boot
  support.
- Real bus drivers: `ahci/`, `nvme/`, USB `msc`. `ata/`, `atapi/` are unregistered dead code.

## 5. Partitions

`kernel/modules/common/partition/`: `mbrgpt.cpp` (MBR/GPT identification), `identfstype.cpp`
(probes the filesystem type on each partition), `mgr.cpp` (the partition manager).

## 6. Current state

- ext4 and SAF work: they mount and load `desktop.elf` / `hw.elf`. FAT is not wired up.
- lwIP is wired up (e1000 82574L plus DHCP/ICMP ping/TCP, brought up late through `NetStackInit`), see
  [network.md](network.md).
- The partition layer is dormant. `USE_VIRT_IMAGE` makes the partition manager skip offset computation,
  and ext4 reads raw LBAs; MBR/GPT parsing exists but isn't in effect.
- Real-hardware disk compatibility is still churning. The README says not to run on real hardware for
  now, and that still holds.
