# 文件缓存完整语义审计（fc + lwext4 + FatFs）

> 目标清单中的"完整语义：文件缓存 writeback、脏页、fsync、OOM 回收、权限等"
> 逐项对照代码与已验证测试，如实标注：✅ 已实现+已测 / 🟡 已实现未专项测 / ❌ 缺失。

## 1. Writeback / 脏页 / fsync

| 语义 | 状态 | 证据 |
|---|---|---|
| 脏页追踪 | ✅ | `fc.cpp` 每条目 `is_dirty` + `dirty_cache_bytes` 全局计数 (L576/1048/1086) |
| 后台写回 (idle) | 🟡 | `fc_idle.cpp` idle_handler 批量冲刷, 批大小动态 (总条目/4, 16..64) —— **机制存在但生产未激活** (idle 冲刷依赖 tick 接线) |
| 写回回调 | ✅ | `writeback_cb(key, key_len, data, data_len)` → `fd.cpp file_cache_writeback_callback` —— 回调完整; round 75 补写回内容校验测试 (test_flush_writeback_content) |
| fsync | ✅ | `file_cache_fsync` 按 file_id 收集脏条目 → 回调 → 成功后清脏 —— **round 94 生产激活**: sys_fclose 与进程退出 (fd_manager_destroy) 均先冲刷脏页再关闭 (file_id = filedesc 指针, 与 promote 键一致) |
| 写回重试/失败状态 | ✅ | `writeback_retries` 计数, ≥5 → `FC_STATE_WRITEBACK_FAILED`; 失败条目 30s 冷却重试 (round 61 P1-35) |
| I/O 拥塞自适应 | ✅ | io_congestion ≥90 停冲刷 / ≥70 批=1 / ≥30 批/4 (fc_idle.cpp:159-161); 脏比例动态上限 80-拥塞/2, 下限 20 (fc.cpp:1103-1105) |
| 淘汰不丢脏 | ✅ | 淘汰只选 `!is_dirty` (fc_idle.cpp:175/229/284); 写回失败条目不淘汰; 广播失效先写回再摘除 (round 61 P1-34) |
| ext4 层脏块 | ✅ | `ext4_blockdev` dirty_list + `ext4_block_cache_flush` (ext4_blockdev.cpp:469); jbd 脏超块写回 (ext4_journal.cpp:442) |
| FatFs 脏扇区 | ✅ | `FA_DIRTY` 窗口写回 (ff.cpp:4025 等) |

结论: 脏页/写回回调/fsync **已生产激活** (round 94: fclose + 进程退出隐式冲刷);
后台 idle 冲刷仍依赖 tick 接线 (🟡)。审计口径修正, 与 filesystems.md 统一。
未专项验证项: 断电场景的脏数据一致性 (依赖 ext4 journal 的崩溃恢复路径未测)、
fsync 的并发正确性 (多线程同文件 fsync)。

## 2. OOM 回收

| 语义 | 状态 | 证据 |
|---|---|---|
| FC 分配失败路径 | ✅ | `FC_CACHE_ENTRY_ALLOC` 失败计数与恢复 (fc.cpp 分配处); fc 回归套件含 OOM 注入 |
| 脏页 OOM 回收 (swap 类比) | ❌ | 无匿名页/无 swap —— 本 OS 无用户态换页; 脏页只能写回不能回收。**如实说明: 这是架构边界, 不是缺陷** |
| 淘汰触发 | ✅ | 容量配额 + io_congestion 驱动的淘汰扫描 (fc_idle.cpp) |

## 3. 权限

| 语义 | 状态 | 证据 |
|---|---|---|
| 进程信任位 | ✅ | `IsTrusted` 检查: kill (task.cpp:44), exec (exec.cpp:296), 跨进程 mmap (task.cpp:142), 内核进程保护 (task.cpp:51) |
| uid/gid / 文件权限位 / chmod / access | ❌ | **未实现** —— 单用户 OS, 无 POSIX 权限模型。这是已知架构边界, 需在 README/文档明示 |
| 文件访问隔离 | 🟡 | 依赖进程级信任位, 无 per-file ACL |

## 4. 一致性补充 (ext4)

- 事务提交 (`ext4_trans_*`)、jbd 日志 (ext4_journal.cpp 941/1263 脏块+超块)、块缓存脏链表 —— 崩溃恢复路径代码存在, 未做断电注入测试 (需 QEMU 突然断电 + 重启 fsck 验证)。

## 5. 差距 → 后续任务

1. 🟡 fsync 并发正确性测试 (多线程同文件, fc 回归套件扩展)
2. 🟡 断电崩溃恢复测试 (ext4: 写一半 → QEMU 强杀 → 重启挂载验证)
3. ❌ 权限模型文档化 (README 明示无 uid/gid, 信任位替代)
4. ✅ 不完整写回→失败重试 已在 fd.cpp:260 实现 (audit 初稿误列, 复核修正)
