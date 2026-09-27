# GUI 栈与并行合成器

源码：`programs/desktop/`（`main.cpp`、`loader.cpp`、`synthesizer/window.cpp`）、`lib/graphic/`、`kernel/src/drivers/framebuffer/`。

> 整个桌面跑在用户态：内核只给 framebuffer、共享内存和 sysinfo。本文档描述代码实际行为，含已知缺陷。

## 1. 数据并行渲染（实际形态）

```
在线 CPU 数 N  ← sys_sysinfo(0)

屏幕水平切成 N 条带（disjoint Y range）

主线程渲染条带 0；其余 N-1 条带各起一个 worker 线程（sys_thread_launch）
```

- **零逐像素锁**：每个 worker 写自己的 Y 区间，永不重叠；
- **双缓冲 + 单一提交点**：worker 只写不可见 back buffer，整帧结束后**只有主线程**把它推到 scanout；
- **barrier 是计数式的**（`done_compose_` 计数 + 自旋等待），**不是**带序列号校验的生成屏障：迟到的 worker 为上一帧自增可能提前满足本帧等待——"绝无撕裂"存在**残余竞态窗口，未完全关闭**（旧文档宣称的 `present_seq_/done_present_` 两阶段提交并不存在）；
- 已修复：提交方二次 `rb_erase` 导致红黑树双删的问题（先搜索确认再摘除）。

## 2. 场景遍历复杂度（实际）

- 两层动态链表：layer 列表 + 每层窗口列表；
- 每个窗口在**每个条带**做一次 clip 测试，命中则按整根 scanline blit；
- 单帧代价是 **O(strips × windows)**，并非旧文档宣称的 O(窗口数)。

## 3. 视觉效果（全软件渲染）

- SDF 抗锯齿圆角（8px）；
- 软方向投影阴影（二次衰减）；
- 逐像素 ARGB source-over，连续不透明段退化为 `memcpy`；
- Win11/Fluent 风格：扁平深色标题栏、1px 发光描边；
- 脏矩形比较 blit：场景静止时除光标方块外零 scanout 写（该机制真实存在）。

## 4. 独立光标层

- 光标是独立一层，直接写 scanout，O(16²)；
- 与场景合成解耦：移动鼠标不触发重合成；
- 已知缺陷：光标重绘顺序先画新方块再恢复旧方块，位移 <16px 时会把箭头抹掉一部分。

## 5. 文本

- `lib/base/font/ttf.c`：基于 **stb_truetype**（第三方），LRU + 哈希表字形缓存、CJK 排版、边界裁剪 alpha 混合；
- 控制台输出由 **flanterm**（第三方）渲染。

## 6. WM 与应用的边界（实际状态）

- 窗口装饰由 WM 画，应用只管客户区；
- **控制台窗口**具备完整交互：拖拽、最大化、最小化（任务栏恢复）、8 向缩放、关闭按钮走 `sys_kill` 回收整个客户进程；
- **notepad 的窗口只注册未管理**：不参与命中测试/拖拽/缩放/最大化/关闭；
- **无子进程退出通知**（无 waitpid/SIGCHLD）：客户端 `return 0` 后屏幕留下死窗口；
- 无 z-order raise / 焦点管理；最小化会丢失最大化状态；
- "控制台客户端是纯标准 C、可在任意宿主工具链原样编译"——**不准确**：它依赖树内 libc（`-nostdlib`、树内 `_start`、钉在 0x400000 的协议页），只能在随本项目构建的 libc 上编译；
- 客户端与合成器共用单个 surface，无 flip/fence：合成器可能采样到客户端半重绘的画面；
- `programs/desktop/loader.cpp` 负责把 ELF 装进新进程并接入窗口。
