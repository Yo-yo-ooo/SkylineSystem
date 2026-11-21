# File-cache semantics: what is actually implemented

The goal list asks for "full semantics" on the file cache: writeback, dirty pages, fsync, OOM
reclamation, permissions. This file goes through the list against the code and the tests that cover it.
Status means: `yes` = implemented and tested, `partial` = implemented but not specifically tested,
`no` = missing.

## 1. Writeback, dirty pages, fsync

| Semantics | Status | Evidence |
|---|---|---|
| dirty-page tracking | yes | `fc.cpp` per-entry `is_dirty` plus the global `dirty_cache_bytes` counter (L576/1048/1086) |
| background writeback (idle) | partial | `fc_idle.cpp` idle_handler flushes in batches, batch size is dynamic (total entries/4, clamped 16..64). The mechanism exists but production doesn't drive it: idle flushing depends on the tick being wired |
| writeback callback | yes | `writeback_cb(key, key_len, data, data_len)` calls into `fd.cpp file_cache_writeback_callback`; the callback is complete and `test_flush_writeback_content` checks the written bytes |
| fsync | yes | `file_cache_fsync` collects dirty entries per file_id, runs the callback, clears the dirty flag on success. Production path: `sys_fclose` and process exit (`fd_manager_destroy`) both flush before closing (file_id is the filedesc pointer, same as the promote key) |
| writeback retry / failure state | yes | `writeback_retries` counter; at 5 the entry goes to `FC_STATE_WRITEBACK_FAILED`, and retry waits out a 30 s cooldown |
| I/O congestion adaptation | yes | io_congestion ≥90 stops flushing, ≥70 batch=1, ≥30 batch/4 (fc_idle.cpp:159-161); the dirty-ratio bound moves with congestion, 80-congestion/2 upper and 20 lower (fc.cpp:1103-1105) |
| eviction never drops dirty pages | yes | eviction only picks `!is_dirty` (fc_idle.cpp:175/229/284); entries with a failed writeback are left alone; broadcast invalidation writes back before removing |
| ext4-layer dirty blocks | yes | `ext4_blockdev` dirty_list + `ext4_block_cache_flush` (ext4_blockdev.cpp:469); jbd dirty superblock writeback (ext4_journal.cpp:442) |
| FatFs dirty sectors | yes | `FA_DIRTY` window writeback (ff.cpp:4025 and around) |

So dirty pages, the writeback callback and fsync are all live in production, through fclose and the
implicit flush at process exit. Background idle flushing is the one still waiting on tick wiring.
Two things are not verified: dirty data surviving a power loss (that rides on the ext4 journal
crash-recovery path, which has no test), and fsync correctness when several threads fsync the same file.

## 2. OOM reclamation

| Semantics | Status | Evidence |
|---|---|---|
| FC allocation failure path | yes | `FC_CACHE_ENTRY_ALLOC` failures are counted and recovered at the allocation sites in fc.cpp; the fc regression suite injects OOM |
| dirty-page reclamation (swap analogue) | no | there are no anonymous pages and no swap: this system doesn't page out userspace. Dirty pages can be written back, not reclaimed. That's an architectural boundary, not a defect |
| eviction trigger | yes | capacity quota plus an io_congestion-driven eviction scan (fc_idle.cpp) |

## 3. Permissions

| Semantics | Status | Evidence |
|---|---|---|
| process trust bit | yes | `IsTrusted` checks at kill (task.cpp:44), exec (exec.cpp:296), cross-process mmap (task.cpp:142) and the kernel-process guard (task.cpp:51) |
| uid/gid, file permission bits, chmod, access | no | not implemented. Single-user system, no POSIX permission model. README and docs should say so rather than leave it implied |
| file-access isolation | partial | only the process-level trust bit; no per-file ACL |

## 4. ext4 consistency

Transaction commit (`ext4_trans_*`), the jbd journal (ext4_journal.cpp 941/1263 dirty blocks plus
superblock) and the block-cache dirty list are implemented. The crash-recovery path has never been
tested with a power-loss injection, though; that needs QEMU hard power-off plus a reboot and fsck.

## 5. Left open

1. fsync concurrency test: several threads on one file, as an extension of the fc regression suite.
2. Power-loss recovery test: write halfway, hard-kill QEMU, reboot, remount, check the result.
3. Document the permission model: README should state no uid/gid and that the trust bit takes its place.
4. Writeback failure leading to retry is already handled at fd.cpp:260. The first draft of this audit
   listed it as missing; corrected on review.
