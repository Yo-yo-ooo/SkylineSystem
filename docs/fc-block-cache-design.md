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
