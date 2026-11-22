<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# System call ABI (not POSIX)

Source: `kernel/include/arch/x86_64/schedule/syscalln.h`,
`kernel/src/arch/x86_64/schedule/syscall.cpp`, `syscall/*.cpp`.

SkylineSystem doesn't try to be Linux/POSIX ABI compatible. The table below lists the 17 base calls
numbered 0-16 in numeric order. The kernel also registers 17/18/19/20/21/24/25/26, 25 slots in total
(see `syscall.cpp`). Register conventions are pinned by `lib/base/arch/x86_64/syscall.c` and the
per-call implementations under `syscall/`; this document only fixes the numbers and semantics.

Additions in 2026-11: fixed slot 22 = `mkdir` (`ext4_dir_mk`, used by Setup to create
`/mp/system`), fixed slot 23 = `readdir` (`ext4_dir_open` / `ext4_dir_entry_next`, used by Explorer
to enumerate real ext4 directories), fixed slot 31 = `child_event` (the desktop drains child exit
events, replacing the 500ms `sys_kill(pid,0)` polling). The kmod fixed-contract table is now 0..31,
32 entries with no reserved holes.

## 1. Calling convention

- Entry goes through MSR `LSTAR`, installed by `syscall_init()`.
- Call number in `rax`; arguments follow System V register order.
- Return value in `rax`; negative means an error code.
- Kernel-side handling finishes with interrupts disabled or under a spinlock. On return `swapgs`
  switches back to the user GS.

## 2. The table

### File I/O (0-5)

| # | Name | Semantics |
|---|---|---|
| 0 | `FOPEN` | open a path, return a file descriptor |
| 1 | `FWRITE` | write by fd |
| 2 | `FREAD` | read by fd |
| 3 | `FCLOSE` | close the fd |
| 4 | `FLSEEK` | move the file offset |
| 5 | `FSIZE` | query file size |

Behind it sits the VFS layer: hashmap mount-point resolution plus a sharded red-black tree fd
allocator. See [filesystems.md](filesystems.md).

### Process and threads (6-13)

| # | Name | Semantics |
|---|---|---|
| 6 | `THREAD_LAUNCH` | start a thread inside the current process |
| 7 | `GETTID` | current thread id |
| 8 | `GETPID` | current process id |
| 9 | `EXIT` | thread or process exit |
| 10 | `PMMAP` | map physical or device memory into the user address space |
| 11 | `YIELD` | yield the CPU voluntarily (3EVDF rarely needs it, kept anyway) |
| 12 | `LOAD` | load an ELF image |
| 13 | `LAUNCH` | create a process and start an ELF; this is how `/mp/desktop.elf` gets started |

Process and thread descriptors hang off `pid2proc_tree`, an ART radix tree, protected by
`PID2PROC_TREE_LOCK`. `NOT_RUNQ_P` is the set of threads not currently on a run queue.

### Memory (14-15)

| # | Name | Semantics |
|---|---|---|
| 14 | `MMAP` | user mmap: anonymous, file mapping or shared frames |
| 15 | `MUNMAP` | unmap |

Underneath: the VMA layer (`vma.cpp`), huge pages and CoW. See [memory.md](memory.md).

### System info (16)

| # | Name | Semantics |
|---|---|---|
| 16 | `SYSINFO` | online CPU count, memory layout, feature bits; the compositor reads it to decide how many workers to start |

### Directory enumeration (23, 2026-11)

| # | Name | Semantics |
|---|---|---|
| 23 | `READDIR` | `(const char* path, char* buf, uint64_t cap, uint64_t flags)` → bytes written, or a negative errno |

`sys_readdir` lives in `kernel/modules/x86_64/syscall/fops.cpp` and is registered by the
builtin component `kernel.syscall.file` on fixed slot 23, the slot that used to be a reserved hole.
It resolves the mount with `GetMount()`, accepts ext4 only (anything else returns `-ENOSYS`), and
iterates with `ext4_dir_open()` / `ext4_dir_entry_next()` / `ext4_dir_close()` from the in-tree
lwext4 layer. No extra mount lock is taken.

The output is line-oriented text rather than a binary struct, so kernel and userspace never have to
agree on layout or alignment:

```
d|<name>\n     directory
f|<name>\n     regular file
?|<name>\n     other inode type
\0             terminator (not counted in the return value)
```

- `.`, `..` and `inode == 0` holes are filtered in the kernel.
- Names are raw ext4 directory-entry bytes (≤ 255) and never path-qualified.
- `cap` is clamped to 64 KiB. On overflow the kernel stops at the last complete line, never mid-name.
- Errors: `-EINVAL` (cap=0 or too large), `-EFAULT` (bad user pointer), `-ENOENT` (no mount point),
  `-ENOMEM`, `-ENOSYS` (non-ext4), `-EIO` (ext4 return code).

Userspace calls it as `syscall(SYSCALL_READDIR, path, buf, cap, 0, 0, 0)`. `programs/explorer/` is
the first consumer: it sorts entries with directories first, asks sizes through `fopen`/`fsize`, and
falls back to the legacy static `/mp/files.index` when the kernel returns `-ENOSYS`.

## 3. Trade-offs

- Deliberately thin. No signal delivery (`syscall/signal.cpp` is a stub), no full socket family, no
  futex. That policy lives in userspace.
- Minimal mechanism: threads, memory mapping, files, sysinfo. Window system, compositor and shell are
  all assembled in userspace.
- Number ranges stay stable. New calls go after 26 (17/18/19/20/21/24/25/26 are already taken past
  the 0-16 block), and existing numbers never get reshuffled — otherwise rebuilt user ELFs would call
  into the wrong entries.
