# GUI 栈与并行合成器

源码：`programs/desktop/`（`main.cpp`、`loader.cpp`、`synthesizer/window.cpp`）、`lib/graphic/`、`kernel/src/drivers/framebuffer/`。

> 整个桌面跑在
>
> **用户态**
>
> ：内核只给 framebuffer、共享内存和 sysinfo，窗口装饰、合成、事件全由 
>
> `desktop.elf`
>
>  自己做。

## 1. 数据并行渲染



```
在线 CPU 数 N  ← sys\_sysinfo(16)

屏幕水平切成 N 条带（disjoint Y range）

每条带 pin 一个 worker 渲染
```



* **零逐像素锁**：每个 worker 写自己的 Y 区间，永不重叠；

* **两阶段 barrier**：所有 worker 先画进 back buffer，整帧结束后才整体切到 scanout—— 被抢占的 worker 不可能在屏幕上闪一条黑带（无撕裂）；

* **脏矩形比较 blit**：场景没动时，除光标方块外零 scanout 写。

## 2. 场景遍历 O (窗口数)



* 两层动态链表：**layer 列表** + 每个 layer 内的**窗口列表**；

* 每个窗口一次 O (1) clip 测试，按整根 scanline blit；

* **不做逐像素的 "最上层搜索"**—— 复杂度是 O (窗口数)，和屏幕分辨率无关。

## 3. 视觉效果（全软件渲染）



* **SDF 抗锯齿圆角**（8 px）；

* **软方向投影阴影**（二次衰减）；

* **逐像素 ARGB source-over 混合**；遇到连续不透明段直接退化成 `memcpy`，几乎不花钱；

* Win11/Fluent 风格：扁平深色标题栏、1 px 发光描边。

## 4. 独立光标层

这是 "鼠标永不卡" 的关键：



* 光标是**独立一层**，直接写 scanout，复杂度 O (16²)；

* 和场景合成**完全解耦**—— 移动鼠标不会唤醒 worker、不会触发重合成；

* 即便用户态在 `for(;;)` 里死循环，光标照样顺滑（配合 3EVDF 的交互优待）。

## 5. 文本：内嵌 TTF 光栅

`lib/base/font/ttf.c`：



* LRU + 哈希表字形缓存；

* CJK 排版规则、真实排版行高；

* 边界裁剪后的 alpha 混合到线性 framebuffer。

## 6. WM 与应用的边界



* **窗口装饰由 WM 画**，应用只管自己的客户区；

* 一个控制台客户端就是朴素 C：`printf` + `return 0`，零 Skyline 专有样板，能在任意宿主工具链上原样编译；

* `programs/desktop/loader.cpp` 负责把 ELF 装进新进程并接入窗口。