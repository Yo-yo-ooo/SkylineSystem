# fc block-level cache design (audit design debt #5)

> Status: **implementation complete (round 26-30)**. Steps 1-4 landed; Step 5 decided as
> keeping the path whole-file API as the small-file fast path (dual-track coexistence, block-level primary).

## Implementation log

| Step | Content | round |
|---|---|---|
| 1 | Key API skeleton (get/promote/invalidate_block, 16B composite key) | 26 |
| 2 | fops read path: per-block hit for non-zero offset + per-block promote | 27 |
| 3 | fops write path: per-block promote + block-range invalidation after hit (prevent stale block entries) | 28 |
| 4 | invalidate_file (whole-file block invalidation, local explicit delete + cross-core broadcast; host regression proved the broadcast-skips-local gap) | 29-30 |
| 5 | whole-file path API kept as small-file fast path (dual-track) | 30 |
| Tests | fc-reg added test_block_cache_basics (promote/get/isolation/invalidate_file) | 30 |

## 1. Target semantics

| Current | Target |
|---|---|
| Key = path (one entry per whole file) | Key = (file_id, block#) (independent entry per 4KB block) |
| promote limited to whole-file reads with offset==0 | reads at any offset/length can promote |
| Large file read entirely in one go | only accessed blocks are cached (sparse cache) |
| Write hit requires `offset+wcnt <= out_len` | write hits aligned per block, auto-split across blocks |

## 2. Key design

```
key = file_id(8B) + block_no(8B, 4KB-aligned sector offset >> 3)
key_len = 16
```

- file_id keeps using `(uint64_t)filedesc` (round 5 writeback already wrote through this way)
- ART index needs no change (variable-length keys already supported); path kept as auxiliary (for diagnostics/invalidation broadcast)
- Compatibility with fc's existing API: `file_cache_get/put/promote/fsync` gain
  `offset/block` parameter overloads; old path signatures kept as whole-file aliases
  (internally expanded to a special file_id + block 0 entry or a compatibility shim)

## 3. Read/write paths

- **Read**: `sys_read` first computes the block list covered by [offset, offset+count) →
  per-block get → memory-copy hits, per-block disk read + promote misses
  (small reads ≤ 2KB still go direct-read to avoid cache thrash, threshold reuses FC_TINY_FILE_THRESHOLD)
- **Write**: write-through keeps current semantics (re-verified in round 24,
  fops.cpp:110-144 flow unchanged), only the hit-decision key changes from path to
  (file_id, block)
- **fsync/writeback**: round 5's file_id write-through unchanged; the entry-level
  dirty bit drops from whole-file granularity to block granularity

## 4. Consistency

- Cross-CPU invalidation: `file_cache_invalidate` changes from path broadcast to
  (file_id, block range) broadcast (ART prefix delete)
- O_TRUNC/file deletion: invalidate the whole file_id range (prefix delete supports this naturally)
- Concurrency: per-entry pin/refcount and the FC_STATE machine carried over; block entries are smaller,
  LRU eviction granularity is finer (eviction no longer drops the whole file's prefix)

## 5. Migration steps (suggested order)

1. fc.h/fc.cpp add block-key overloads + file_id parsing (compatibility layer, golden unbroken)
2. fops read path per-block promote (fc-reg host regression adds cross-block read cases)
3. fops write path block hit (fc-reg adds cross-block writeback cases)
4. Invalidation broadcast switches to range prefix delete (fc-mt concurrency cases extended)
5. whole-file path demoted to a compatibility shim or removed

## 6. Verification gating

- tests/fc/regression.cpp adds: cross-block read hit/miss, cross-block writeback,
  block-level invalidation broadcast, large-file sparse cache (memory-usage assertion)
- Full golden + 30-minute soak (ping 169 baseline)

## 7. Invariant: cached blocks are block prefixes (bug found 2026-10-07)

A block entry is keyed by `(file_id, block#)` and the hit path indexes it with
the caller's *within-block* offset:

```c
void *bdata = file_cache_get_block(..., first_block + b, ...);   /* bdata = ? */
size_t from = (cur_offset + b * BLOCK_SZ) % BLOCK_SZ;
CopyToUser(buf, bdata + from, chunk);        /* assumes bdata starts at the block */
```

That only works if the promoted buffer really starts at the block boundary. The
original promote loop sliced a hardware read into per-block chunks and promoted
**every** chunk, including the first one when the read started mid-block — so a
54-byte read at within-block offset 2644 was cached as "block N", and any later
read in block N whose offset satisfied `from < 54` got those 54 bytes back at
the wrong address. Silent data corruption, not a crash.

It stayed hidden because reads used to be few and large. The per-glyph TTF
loader is what exposed it: glyph slices are ~500 B, so a 4 KB block holds ~8 of
them and consecutive loads poisoned each other — outlines came back garbage,
`stbtt` rasterized absurd bounding boxes, and both vCPUs were found spinning in
`stbtt__v_prefilter` while the console client hung at boot.

Fixes:

- **promote**: only chunks with `(cur_offset + off) % BLOCK_SZ == 0` are cached
  (read path and write path — the write path had the same loop).
- **file_id**: the block key used `(uint64_t)FD->filedesc`, which is a pointer
  that `kmalloc` hands straight back to the next `open()` after a close. Two
  different files could therefore share block keys. `fd_t` now carries a
  monotonic `file_uid` (assigned in `sys_fopen`, preserved by `fd_manager_dup`)
  and every cache call uses that instead.
- **user side**: the TTF loader reads glyph slices from the block start and
  copies the slice out, so it is correct even against an unpatched cache — and
  aligned reads are also the shape the cache promotes best.
