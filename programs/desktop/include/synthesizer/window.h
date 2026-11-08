//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// synthesizer/window.h — double-buffered horizontal-strip compositor
#ifndef SYNTHESIZER_WINDOW_H
#define SYNTHESIZER_WINDOW_H

#include <stdint.h>
#include <graphic/fb.h>

/* bytes per compositor pixel (ARGB8888) */
#define COMP_BPP 4u
/* hard sanity bound on strip/CPU count (defensive clamp in StartWorkers) */
#define COMP_CPUS_SANITY 64u

/* Application-side window description. The compositor treats it read-only
   (except PosX/PosY via MoveWindow's release stores). */
struct Window {
    uint32_t PosX, PosY;               /* surface top-left on screen       */
    uint32_t SizeX, SizeY;             /* surface pitch / height (pixels)  */
    uint32_t FrameStartX, FrameStartY; /* client-content frame inside it   */
    uint32_t FrameEndX, FrameEndY;
    uint64_t FbAddr;                   /* ARGB8888 surface base address    */
    int      HasAlpha;                 /* 1 = blend per-pixel alpha        */
};

/* One horizontal band of the screen, rendered by one worker. */
struct Strip { uint32_t y0, y1; };

/* Pointer shapes (see Compositor::SetCursorShape). */
#define SKY_CURSOR_ARROW     0u   /* default arrow                          */
#define SKY_CURSOR_SIZE_DIAG 1u   /* corner resize: diagonal double-arrow    */
#define SKY_CURSOR_SIZE_HORZ 2u   /* left/right edge: horizontal double-arrow*/
#define SKY_CURSOR_SIZE_VERT 3u   /* top/bottom edge: vertical double-arrow  */

struct CompLayer;

/* Window-node wrapper inside a layer's list (registry bookkeeping). */
struct CompWinNode {
    Window*      win;
    CompLayer*   layer;
    int          visible;
    CompWinNode* next;
    CompWinNode* prev;
};

/* Standalone layer: an ordered stack slot holding any number of windows. */
struct CompLayer {
    CompWinNode* win_head;
    CompWinNode* win_tail;
    uint32_t     window_count;
    uint32_t     z;                    /* insert key; list order rules     */
    CompLayer*   l_next;
    CompLayer*   l_prev;
};

class Compositor {
public:
    static Compositor& Get();

    bool Init(FrameBuffer* screen);
    void SetBackground(const uint32_t* bg);
    /* 壁纸/主题被就地改写（内容指针不变）时，强制下一帧整屏重组，
     * 避免增量合成跳过未标记为 dirty 的壁纸行。 */
    void ForceFullCompose(void);

    /* 永远要重画的屏幕行带 (任务栏): 壁纸缓冲被 wm_draw_taskbar 就地改写的
       区域。其它行在"没有内容窗口覆盖"时可以被跳过 —— 见 ComposeRangeToBack。 */
    void SetDynamicRows(int32_t y0, int32_t y1);

    /* 扫描出锁: 光标线程与主线程(合成/注册表变更) 互斥。
       只有"会写扫描出缓冲或遍历图层/窗口链表"的入口需要拿它。 */
    void LockScan();
    void UnlockScan();

    void StartWorkers();
    void Compose();
    /* 只重绘 [y0,y1) 行带 (损伤区域合成)。窗口内容变化普遍只涉及
       几百行, 整屏 1280x800 全量重绘要 10~19ms, 局部带只要 1~6ms。 */
    void ComposeRows(int32_t y0, int32_t y1);
    void Shutdown();

    /* ---- layer/window registry ---- */
    CompLayer* CreateLayer(uint32_t z);
    bool       DestroyLayer(CompLayer* layer);
    bool       RegisterWindow(Window* w, CompLayer* layer);

    /* AUTO layer management: register w on a freshly created layer stacked
       ABOVE every existing one (first registration = z 0 = bottom). Returns
       the owning layer, or nullptr on failure/duplicate. The layer is
       compositor-owned: UnregisterWindow retires and frees it automatically
       once its last window is gone — null your handle then. Stacking order
       == registration order; reorder with RaiseWindow. */
    CompLayer* RegisterWindowAuto(Window* w);

    bool       UnregisterWindow(Window* w);
    void       SetVisible(Window* w, bool visible);
    void       MoveWindow(Window* w, uint32_t x, uint32_t y);
    void       RaiseWindow(Window* w);

    /* ---- cursor ---- */
    void SetCursor(int32_t x, int32_t y, bool visible);
    void CursorMoveTo(int32_t x, int32_t y);
    /* 光标线程专用: 抢不到扫描出锁就立刻返回 false (丢掉这次采样),
       绝不在原地自旋 —— 自旋占核会让持锁的主线程拿不到 CPU, 形成活锁
       (DOOM 重负载时"鼠标卡死"的成因)。 */
    bool CursorMoveToTry(int32_t x, int32_t y);
    /* Shape the pointer takes: arrow by default, resize glyphs while the
       pointer is over (or dragging) a window edge/corner band. */
    void SetCursorShape(uint32_t shape);

    /* ---- worker entry (called from the C trampoline) ---- */
    void WorkerEntry(uint32_t id);

private:
    void LockList();
    void UnlockList();
    /* 有界尝试取扫描出锁 (成功返回 true); LockScan 则是"自旋+让出"的阻塞版 */
    bool TryLockScan(uint32_t budget);
    /* 已持锁时的光标重绘实现 */
    void CursorMoveLocked(int32_t x, int32_t y);
    void InsertLayerOrdered(CompLayer* layer);
    CompWinNode* FindNode(Window* w);

    void ComposeStripToBack(uint32_t id);
    /*        allow_skip: 允许跳过"两帧都没有窗口、也没有光标"的静态背景行 ——
       那些行在帧缓冲里本来就是壁纸, 重写一遍纯属浪费。
       光标行带必须传 false: 光标擦除时 cur_visible_ 已置 0, 一旦被当成
       "静态背景行"跳过, 旧箭头就留在屏上了。 */
    void ComposeRangeToBack(uint32_t ry0, uint32_t ry1, uint32_t* stage,
                            bool allow_skip);
    /* 当前所有可见窗口覆盖的屏幕行带 (无窗口时 y0>=y1) */
    void ComputeCoverSpan(uint32_t* y0, uint32_t* y1) const;
    /* 只重组覆盖光标的那几行 (光标是图层, 移动 = 局部重绘) */
    void ComposeCursorBand(int32_t y0, int32_t y1);
    void ComposeSingleThreaded();

    FrameBuffer      screen_;
    const uint32_t*  background_;
    uint32_t         bg_pitch_;       /* 背景缓冲每行像素数 (壁纸是紧凑排布) */
    uint32_t         ncpus_;
    uint32_t         launched_;
    int              shutdown_;
    Strip*           strips_;
    /* 每个 worker 一份"单行暂存": 合成一行完整像素后再整行写回扫描出缓冲,
       屏幕上不会出现"先清成壁纸、再叠窗口"的中间态 (桌面闪烁的来源)。
       一行 = 屏幕宽 x 4B, 1280 宽时约 5 KiB. */
    uint32_t*        stage_[COMP_CPUS_SANITY];
    CompLayer*       layer_head_;
    CompLayer*       layer_tail_;
    unsigned char    list_lock_;
    volatile int     scan_lock_;   /* 扫描出/图层链表互斥 (光标线程) */
    uint64_t         frame_seq_;
    uint32_t         started_cnt_;
    uint32_t         done_compose_;
    uint64_t         worker_done_seq_[COMP_CPUS_SANITY];
    int32_t          cur_x_, cur_y_;
    uint8_t          cur_visible_;
    uint32_t         cur_shape_;      /* SKY_CURSOR_*                       */
    int32_t          committed_x_, committed_y_;
    int32_t          dx0_, dy0_, dx1_, dy1_;
    uint8_t          dirty_;
    uint32_t         back_bytes_;
    uint32_t*        back_;           /* == 帧缓冲首地址 (直写, 不再单独分配) */
    /* 静态背景行跳过 (见 ComposeRangeToBack): 上一帧被"内容窗口"覆盖的行带,
       以及"必须整帧重画"的一次性标志 (初始化 / 换壁纸 / 降级)。 */
    uint32_t         cov_y0_, cov_y1_;
    int32_t          dyn_y0_, dyn_y1_;   /* 永远重画的行带 (任务栏) */
    uint8_t          force_full_;

};

#endif /* SYNTHESIZER_WINDOW_H */