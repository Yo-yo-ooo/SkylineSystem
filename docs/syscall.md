# System Call ABI (Non-POSIX)

Source: `kernel/include/arch/x86_64/schedule/syscalln.h`, `kernel/src/arch/x86_64/schedule/syscall.cpp`, `syscall/*.cpp`.

> *SkylineSystem&#x20;*
>
> *is not compatible with Linux/POSABI*
>
> *. The table below lists the 17 base system calls numbered 0-16 in numeric order; the kernel additionally registers numbers 17/18/19/20/21/24/25/26 (25 slots in total, see `syscall.cpp`). For the specific parameter register conventions, see&#x20;*
>
> 2026-11 additions: fixed slot **22 = mkdir** (`ext4_dir_mk`, Setup creates
> `/mp/system`), fixed slot **23 = readdir** (`ext4_dir_open` / `ext4_dir_entry_next`,
> Explorer enumerates real ext4 directories) and fixed slot **31 = child_event**
> (desktop drains child exit events; replaces 500ms `sys_kill(pid,0)` polling).
> The kmod fixed-contract table is now 0..31 (32 entries, no reserved holes).
>
> `lib/base/arch/x86_64/syscall.c`
>
> *&#x20;and the individual&#x20;*
>
> `syscall/*.cpp`
>
> *&#x20;implementations; this document only pins down the*
>
> *numbers and semantics*
>
> *. *

## 1. Calling Convention



* Entry goes through MSR `LSTAR` (installed by `syscall_init()`);

* the call number is in `rax`; arguments are passed in order through registers, System V style;

* the return value is in `rax`; a negative value is an error code;

* kernel-side handling completes with interrupts disabled / under spinlock protection; on return, `swapgs` switches back to the user GS.

## 2. Full Table

### File I/O (0–5)



| # | Name | Semantics |
| - | -------- | ------------ |
| 0 | `FOPEN` | open a path, return a file descriptor |
| 1 | `FWRITE` | write by fd |
| 2 | `FREAD` | read by fd |
| 3 | `FCLOSE` | close the fd |
| 4 | `FLSEEK` | move the file offset |
| 5 | `FSIZE` | query the file size |

> VFS layer: hashmap mount-point resolution + sharded red-black tree fd allocator. See 
>
> filesystems.md
>
> .

### Process / Thread (6–13)



| # | Name | Semantics |
| -- | --------------- | -------------------------------------- |
| 6 | `THREAD_LAUNCH` | start a new thread in the current process |
| 7 | `GETTID` | current thread id |
| 8 | `GETPID` | current process id |
| 9 | `EXIT` | thread / process exit |
| 10 | `PMMAP` | map a range of physical / device memory into the user address space |
| 11 | `YIELD` | voluntarily yield the CPU (generally unneeded under 3EVDF, but kept) |
| 12 | `LOAD` | load an ELF image |
| 13 | `LAUNCH` | create a new process and start an ELF (this is how `/mp/desktop.elf` is started) |

> Process / thread descriptors hang on 
>
> `pid2proc_tree`
>
> (an ART radix tree), protected by 
>
> `PID2PROC_TREE_LOCK`
>
> ;
>
> `NOT_RUNQ_P`
>
> is the set of threads that are not on a run queue.

### Memory (14–15)



| # | Name | Semantics |
| -- | -------- | ------------------------- |
| 14 | `MMAP` | user-space mmap (anonymous / file mapping / shared frames) |
| 15 | `MUNMAP` | unmap |

> Behind it are the VMA layer (
>
> `vma.cpp`
>
> ) + five-level page tables + CoW, see 
>
> memory.md
>
> .

### System Info (16)



| # | Name | Semantics |
| -- | --------- | ---------------------------------------------- |
| 16 | `SYSINFO` | expose to user space the online CPU count, memory layout, feature bits, etc. — the compositor relies on it to know how many workers to start |

### Directory enumeration (23, 2026-11)

| # | Name | Semantics |
| - | -------- | ------------ |
| 23 | `READDIR` | `(const char* path, char* buf, uint64_t cap, uint64_t flags)` → bytes written (>=-0) or negative errno |

`sys_readdir` (`kernel/modules/builtin/x86_64/syscall/fops.cpp`) is registered by
the builtin component `kernel.syscall.file` on fixed slot 23 (the former
`reserved` hole). It resolves the mount point with `GetMount()`, accepts **ext4
only** (other mounts return `-ENOSYS`), then iterates with
`ext4_dir_open()` / `ext4_dir_entry_next()` / `ext4_dir_close()` from the in-tree
lwext4 layer — no extra mount lock is taken here.

The output is deliberately **line-oriented text**, not a binary struct, so the
kernel and userspace never have to agree on struct layout/alignment:

```
d|<name>\n     directory
f|<name>\n     regular file
?|<name>\n     other inode type
\0             terminator (not counted in the return value)
```

- `.`, `..` and `inode == 0` holes are filtered in the kernel;
- names are raw ext4 directory-entry bytes (<= 255), never path-qualified;
- `cap` is clamped to 64 KiB; on overflow the kernel stops at the last **complete**
  line (never a half name);
- errors: `-EINVAL` (cap=0 / too large), `-EFAULT` (bad user pointer),
  `-ENOENT` (no mount point), `-ENOMEM`, `-ENOSYS` (non-ext4), `-EIO` (ext4 rc).

Userspace calls it as `syscall(SYSCALL_READDIR, path, buf, cap, 0, 0, 0)`.
`programs/explorer/` is the first consumer; it sorts entries (directories first),
queries sizes with `fopen`/`fsize`, and falls back to the legacy static
`/mp/files.index` when the running kernel returns `-ENOSYS` (pre-23 kernel).

## 3. Design Trade-offs



* **Deliberately thin**: no signal delivery (`syscall/signal.cpp` is currently a stub), no full socket family, no futex — policy is pushed to user space.

* **Minimal mechanism**: the kernel provides four things — "threads + memory mapping + files + sysinfo"; the window system, compositor, and shell are all assembled in user space.

* **Stable number ranges**: new calls are appended after 26 (17/18/19/20/21/24/25/26 are already taken beyond 0-16); existing numbers are never reshuffled, to avoid user-space ELFs running wild after a rebuild.
