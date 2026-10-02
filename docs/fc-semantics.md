# Full file-cache semantics audit (fc + lwext4 + FatFs)

> The goal list's "full semantics: file-cache writeback, dirty pages, fsync, OOM reclamation, permissions, etc."
> is checked item by item against the code and the verified tests, and marked honestly: ✅ implemented + tested / 🟡 implemented but not specifically tested / ❌ missing.

## 1. Writeback / dirty pages / fsync

| Semantics                        | Status | Evidence                                                                                                                                                                                                                                                                                           |
| -------------------------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| dirty-page tracking              | ✅      | `fc.cpp` per-entry `is_dirty` + global `dirty_cache_bytes` counter (L576/1048/1086)                                                                                                                                                                                                                |
| background writeback (idle)      | 🟡     | `fc_idle.cpp` idle_handler batch flushes, batch size dynamic (total entries/4, 16..64) — **mechanism exists but not activated in production** (idle flushing depends on tick wiring)                                                                                                               |
| writeback callback               | ✅      | `writeback_cb(key, key_len, data, data_len)` → `fd.cpp file_cache_writeback_callback` — callback complete; round 75 added a writeback content verification test (test_flush_writeback_content)                                                                                                     |
| fsync                            | ✅      | `file_cache_fsync` collects dirty entries by file_id → callback → clears the dirty flag on success — **activated in production in round 94**: both sys_fclose and process exit (fd_manager_destroy) flush dirty pages before closing (file_id = filedesc pointer, consistent with the promote key) |
| writeback retry/failure state    | ✅      | `writeback_retries` counter, ≥5 → `FC_STATE_WRITEBACK_FAILED`; failed entries retry after a 30 s cooldown (round 61 P1-35)                                                                                                                                                                         |
| I/O congestion adaptation        | ✅      | io_congestion ≥90 stops flushing / ≥70 batch=1 / ≥30 batch/4 (fc_idle.cpp:159-161); dynamic dirty-ratio upper bound 80-congestion/2, lower bound 20 (fc.cpp:1103-1105)                                                                                                                             |
| eviction never drops dirty pages | ✅      | eviction only selects `!is_dirty` (fc_idle.cpp:175/229/284); entries whose writeback failed are not evicted; broadcast invalidation writes back before removal (round 61 P1-34)                                                                                                                    |
| ext4-layer dirty blocks          | ✅      | `ext4_blockdev` dirty_list + `ext4_block_cache_flush` (ext4_blockdev.cpp:469); jbd dirty superblock writeback (ext4_journal.cpp:442)                                                                                                                                                               |
| FatFs dirty sectors              | ✅      | `FA_DIRTY` window writeback (ff.cpp:4025 etc.)                                                                                                                                                                                                                                                     |

Conclusion: dirty pages / writeback callback / fsync **are activated in production** (round 94: fclose + implicit flush on process exit);
background idle flushing still depends on tick wiring (🟡). Audit wording corrected, unified with filesystems.md.
Not specifically verified: dirty-data consistency across power loss (depends on the untested ext4 journal crash-recovery path),
concurrent correctness of fsync (multiple threads fsyncing the same file).

## 2. OOM reclamation

| Semantics                                 | Status | Evidence                                                                                                                                                                              |
| ----------------------------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| FC allocation-failure path                | ✅      | `FC_CACHE_ENTRY_ALLOC` failure counting and recovery (allocation sites in fc.cpp); the fc regression suite includes OOM injection                                                     |
| dirty-page OOM reclamation (swap analogy) | ❌      | no anonymous pages / no swap — this OS has no userspace paging; dirty pages can only be written back, not reclaimed. **Honest note: this is an architectural boundary, not a defect** |
| eviction trigger                          | ✅      | capacity quota + io_congestion-driven eviction scan (fc_idle.cpp)                                                                                                                     |

## 3. Permissions

| Semantics                                       | Status | Evidence                                                                                                                                                 |
| ----------------------------------------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| process trust bit                               | ✅      | `IsTrusted` checks: kill (task.cpp:44), exec (exec.cpp:296), cross-process mmap (task.cpp:142), kernel-process protection (task.cpp:51)                  |
| uid/gid / file permission bits / chmod / access | ❌      | **not implemented** — single-user OS, no POSIX permission model. This is a known architectural boundary and must be stated explicitly in the README/docs |
| file-access isolation                           | 🟡     | relies on the process-level trust bit, no per-file ACL                                                                                                   |

## 4. Consistency supplement (ext4)

- Transaction commit (`ext4_trans_*`), jbd journal (ext4_journal.cpp 941/1263 dirty blocks + superblock), block-cache dirty list — the crash-recovery path code exists but has no power-loss injection test (needs QEMU sudden power-off + reboot fsck verification).

## 5. Gaps → follow-up tasks

1. 🟡 fsync concurrency-correctness test (multiple threads on the same file; fc regression suite extension)
2. 🟡 power-loss crash-recovery test (ext4: write halfway → hard-kill QEMU → reboot and remount to verify)
3. ❌ document the permission model (README explicitly states no uid/gid, trust-bit replacement)
4. ✅ incomplete-writeback→retry-on-failure is already implemented at fd.cpp:260 (mislisted in the audit first draft; corrected on review)
