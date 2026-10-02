# fc 块级缓存设计（审计设计债 #5）

> 状态：**实施完成（round 26-30）**。Step 1-4 落地，Step 5 决策为
> 保留 path 整文件 API 作为小文件快路径（双轨并存，块级为主）。

## 实施记录

| Step | 内容 | round |
|---|---|---|
| 1 | 键 API 骨架（get/promote/invalidate_block，16B 复合键） | 26 |
| 2 | fops 读路径：非零 offset 逐块命中 + 逐块 promote | 27 |
| 3 | fops 写路径：逐块 promote + 命中后块区间失效（防块条目陈旧） | 28 |
| 4 | invalidate_file（整文件块失效，本机显式删除 + 跨核广播；宿主回归实锤广播跳过本机的缺口） | 29-30 |
| 5 | whole-file path API 保留为小文件快路径（双轨） | 30 |
| 测试 | fc-reg 新增 test_block_cache_basics（promote/get/隔离/invalidate_file） | 30 |

## 1. 目标语义

| 现状 | 目标 |
|---|---|
| 键 = path（整文件一个条目） | 键 = (file_id, block#)（每 4KB 一块独立条目） |
| promote 限 offset==0 的整文件读 | 任意 offset/length 的读都可 promote |
| 大文件一次读入全部 | 只缓存被访问的块（稀疏缓存） |
| 写命中要求 `offset+wcnt <= out_len` | 写命中按块对齐，跨块自动拆条 |

## 2. 键设计

```
key = file_id(8B) + block_no(8B, 4KB 对齐的扇区偏移 >> 3)
key_len = 16
```

- file_id 沿用 `(uint64_t)filedesc`（round 5 写回已按此直写）
- ART 索引无需改（变长键已支持）；path 保留为辅助（诊断/失效广播用）
- 与 fc 现有 API 的兼容：`file_cache_get/put/promote/fsync` 增加
  `offset/block` 参数的重载，旧 path 签名保留为 whole-file 的别名
  （内部展开为 file_id + block 0 的特殊条目或兼容 shim）

## 3. 读写路径

- **读**：`sys_read` 先算 [offset, offset+count) 覆盖的块列表 →
  逐块 get → 命中者内存拷贝，未命中者逐块磁盘读 + promote
  （小块读 ≤ 2KB 仍走直读避免 cache 抖动，阈值沿用 FC_TINY_FILE_THRESHOLD）
- **写**：write-through 保持现状语义（round 24 复核的
  fops.cpp:110-144 流程不变），仅把命中判定的键从 path 换成
  (file_id, block)
- **fsync/写回**：round 5 的 file_id 直写不变；条目级的
  dirty 位从整文件粒度降为块粒度

## 4. 一致性

- 跨 CPU 失效：`file_cache_invalidate` 从 path 广播改为
  (file_id, block 区间) 广播（ART 前缀删除）
- O_TRUNC/文件删除：invalidate 全 file_id 区间（前缀删除天然支持）
- 并发：每条目 pin/refcount 与 FC_STATE 机沿用；块条目更小，
  LRU 淘汰粒度更细（淘汰不再丢弃整文件的前缀）

## 5. 迁移步骤（建议顺序）

1. fc.h/fc.cpp 增加块键重载 + file_id 解析（兼容层，golden 不破）
2. fops 读路径逐块 promote（fc-reg 宿主回归新增跨块读用例）
3. fops 写路径块命中（fc-reg 新增跨块写回用例）
4. 失效广播改区间前缀删除（fc-mt 并发用例扩展）
5. whole-file 路径降级为兼容 shim 或删除

## 6. 验证门禁

- tests/fc/regression.cpp 新增：跨块读命中/未命中、跨块写回、
  块级失效广播、大文件稀疏缓存（内存占用断言）
- 全量 golden + 30 分钟 soak（ping 169 基线）
