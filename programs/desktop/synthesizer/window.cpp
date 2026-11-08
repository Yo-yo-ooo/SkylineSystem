//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// synthesizer/window.cpp — double-buffered horizontal-strip compositor
#include <synthesizer/window.h>
#include <syscall.h>
#include <syscalln.h>   /* SYSCALL_SLEEP_MS */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/*
 * Worker threads are pinned 1:1 to CPUs through sys_thread_launch's hint.
 * The kernel forces the new thread's rdi = 0 (no argument passing), so a
 * strip id cannot be handed in directly; an atomic ticket pool assigns one.
 */
static uint32_t g_strip_ticket = 0;
/* 卸载窗口的退役节点链 (LockList 保护写入), 由 Compose 的
   帧屏障点统一释放 —— 防在途遍历 UAF。链走 prev: worker 只读 next,
   被退役节点保持 next 指向原后继, 在途遍历可无缝继续。 */
static CompWinNode* g_retire_list = nullptr;
/* Auto-layer retire chain: RegisterWindowAuto 创建的图层随其最后一个
   窗口的注销而退役, 同样在帧屏障点释放。链走 l_prev (worker 只读
   l_next); 被退役图层的 l_next 保持指向原后继, 在途遍历会"穿过"这个
   空图层继续走进仍注册的栈段。 */
static CompLayer* g_retire_layers = nullptr;

/* Freestanding 32-bit span equality (no libc memcmp declaration needed);
   -O2 lowers a word loop like this to a fast vectorised compare. Used by the
   dirty-rectangle compare-and-blit path to skip unchanged scanout spans. */
static inline bool span_eq_u32(const uint32_t* a, const uint32_t* b, uint32_t words) {
    for (uint32_t i = 0; i < words; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

/* One global compositor — all members are trivial/POD, so BSS zero-init
   needs no dynamic constructor (matches -fno-threadsafe-statics). */
static Compositor g_compositor;

/* ---- 临时性能埋点: 供桌面主循环统计合成/光标开销 ------------------------- */
static inline uint64_t comp_rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t g_pf_compose_cyc = 0, g_pf_compose_cnt = 0;
static uint64_t g_pf_band_cyc    = 0, g_pf_band_cnt    = 0;
static uint64_t g_pf_lock_miss   = 0;   /* 光标线程抢锁失败次数 */
static uint64_t g_compositor_lock_miss(void) { return g_pf_lock_miss; }

extern "C" void comp_perf_take(uint64_t* compose_cyc, uint64_t* compose_cnt,
                               uint64_t* band_cyc, uint64_t* band_cnt,
                               uint64_t* lock_miss) {
    if (lock_miss) *lock_miss = g_compositor_lock_miss();
    if (compose_cyc) *compose_cyc = g_pf_compose_cyc;
    if (compose_cnt) *compose_cnt = g_pf_compose_cnt;
    if (band_cyc)    *band_cyc    = g_pf_band_cyc;
    if (band_cnt)    *band_cnt    = g_pf_band_cnt;
    g_pf_compose_cyc = g_pf_compose_cnt = 0;
    g_pf_band_cyc    = g_pf_band_cnt    = 0;
}

static inline void cpu_relax() {
    __asm__ __volatile__("pause" ::: "memory");
}

/* Adaptive wait: spin with PAUSE for a bounded budget first (a peer CPU
   usually makes progress within a few hundred spins, at zero scheduling
   cost); only drop into sys_yield() once the budget is spent. Pure
   sys_yield() polling is pathological here: when this CPU's runqueue holds
   only the waiter, every yield takes a full scheduling interrupt only to
   re-pick the same thread, drowning the whole system in context switches. */
#define COMP_SPIN_BUDGET 2048u
/* 帧屏障耐心值 (任务栏卡死修复): sys_yield 这么多次还凑不齐 peers, 判定
   有 worker 已丢失 (其核心被不让出的客户端占死 / 线程意外退出), 主线程
   自补缺失 strip 并永久降级单线程 —— 宁可变慢也不锁死桌面。嫌自愈慢可
   调小; 负载重导致误降级则调大。 */
/* yield 阶段的迭代次数上限: 约相当于几次调度器时间片 (见 comp_backoff) */
#ifndef COMP_YIELD_BUDGET
#define COMP_YIELD_BUDGET 4000u
#endif

#define COMP_FRAME_YIELD_BUDGET 512u
static inline void comp_backoff(uint32_t &spin) {
    /* 自旋一小段 (帧内栅栏要低延迟), 之后**真睡眠**而不是 sys_yield()。
       原因: yield 之后调度器立刻又把这个线程放回运行队列, 等于换一种方式
       空转 —— 合成器 worker 在帧间等待时就是这样一直占着 CPU, 实测把
       DOOM 的启动拖慢一倍 (R_InitData 18.2s vs 8.6s)。
       SYSCALL_SLEEP_MS 走内核 Schedule::Sleep (定时器轮), 真正让出 CPU。 */
    /* 自旋一小段后 sys_yield()。**不能**换成 sleep:
       主循环的 compose 与输入处理是同一个线程串行的, worker 一旦睡过整帧,
       主线程就卡在栅栏上读不到鼠标 —— 实测 sleep 版本光标完全不动。
       (启动加速改由"限制 worker 数量"实现, 见 StartWorkers 里的说明。) */
    if (spin < COMP_SPIN_BUDGET) { cpu_relax(); ++spin; }
    else { sys_yield(); spin = 0; }
}


static inline int64_t max_i64(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t min_i64(int64_t a, int64_t b) { return a < b ? a : b; }

/* Source-over blend one ARGB pixel over an already-flattened (opaque) scene. */
static inline uint32_t src_over_argb(uint32_t s, uint32_t d) {
    uint32_t a = s >> 24;
    if (a == 255u) return s;
    if (a == 0u)   return d;
    uint32_t ia = 255u - a;
    uint32_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
    uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    uint32_t r = (sr * a + dr * ia + 128u) >> 8;
    uint32_t g = (sg * a + dg * ia + 128u) >> 8;
    uint32_t b = (sb * a + db * ia + 128u) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Blend one window row that may carry per-pixel alpha (anti-aliased rounded
   corners, soft shadow). Runs of fully-opaque pixels are bulk-copied with
   memcpy, fully-transparent pixels keep the destination, and only the sparse
   edge/shadow pixels pay for a blend — so an opaque body stays one memcpy. */
static inline void blend_row_argb(uint32_t* dst, const uint32_t* src, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t a = src[i] >> 24;
        if (a == 255u) {
            uint32_t j = i + 1;
            while (j < n && (src[j] >> 24) == 255u) ++j;
            memcpy(dst + i, src + i, (size_t)(j - i) * sizeof(uint32_t));
            i = j;
        } else if (a == 0u) {
            ++i;                                   /* keep what is underneath */
        } else {
            dst[i] = src_over_argb(src[i], dst[i]);
            ++i;
        }
    }
}

/* Classic 16x16 arrow pointer. '*' = black outline, 'O' = white fill,
   '.' = transparent (keeps whatever was composited underneath). */
static const char* const kCursorArrow[16] = {
    "*...............",
    "**..............",
    "*O*.............",
    "*OO*............",
    "*OOO*...........",
    "*OOOO*..........",
    "*OOOOO*.........",
    "*OOOOOO*........",
    "*OOOOOOO*.......",
    "*OOOO*****......",
    "*OO*O*..........",
    "*O*.*O*.........",
    "**..*O*.........",
    "*....*O*........",
    ".....*O*........",
    "......*........."
};

/* Resize cursors, shown while the pointer sits on a window edge/corner band
   (and for the whole duration of an edge drag). Diagonal is used for the four
   corners, the double-arrows for the four edges. */
static const char* const kCursorSizeDiag[16] = {
    ".....*..........",
    "....**..........",
    "...*O*..........",
    "..*OO*..........",
    ".*OOO*..........",
    "*OOOO*...*......",
    "*OOOO*.***......",
    "*OOOO*O*O*......",
    ".*OOO**O*O*.....",
    "..*OO*.*O*O*....",
    "...*O*..*O*O*...",
    "....*....*O*O*..",
    "..........*O*...",
    "...........*....",
    "................",
    "................"
};

static const char* const kCursorSizeHorz[16] = {
    "................",
    "................",
    "................",
    ".......*........",
    "......*O*.......",
    "..*..*OO*..*....",
    ".*O**OOOO**O*...",
    "*OOOOOO*OOOOO*..",
    ".*O**OOOO**O*...",
    "..*..*OO*..*....",
    "......*O*.......",
    ".......*........",
    "................",
    "................",
    "................",
    "................"
};

static const char* const kCursorSizeVert[16] = {
    ".......*........",
    "......*O*.......",
    ".....*OO*.......",
    "....*OOO*.......",
    ".....*OO*.......",
    "......*O*.......",
    "......*O*.......",
    "......*O*.......",
    "......*O*.......",
    "......*O*.......",
    "......*O*.......",
    "......*O*.......",
    ".....*OO*.......",
    "....*OOO*.......",
    ".....*OO*.......",
    "......*........."
};

static const char* const* cursor_glyph(uint32_t shape) {
    switch (shape) {
    case SKY_CURSOR_SIZE_DIAG: return kCursorSizeDiag;
    case SKY_CURSOR_SIZE_HORZ: return kCursorSizeHorz;
    case SKY_CURSOR_SIZE_VERT: return kCursorSizeVert;
    default:                   return kCursorArrow;
    }
}

/* ========================================================================== */
/*  Compositor                                                                */
/* ========================================================================== */
/* 扫描出锁 (自旋锁)。光标线程会频繁地重绘 16 行带, 而主线程在整屏合成
   或增删窗口节点。两者必须互斥, 否则光标线程可能遍历到正在被释放的节点,
   或在主线程刚写好的行上盖一块旧内容。
   临界区都很短 (一次带合成 <1ms, 一次整屏合成 10~19ms), 自旋足够。 */
/* 自旋有上限, 之后必须 sys_yield() 让出 CPU。
   原先是无界 pause 自旋: 光标线程(2ms 一轮)一旦碰上主线程正在做 10~30ms 的
   整屏合成, 就会在自己的核上原地转圈。DOOM 这类重客户端把核吃满时,
   "光标线程自旋占核 -> 持锁的主线程拿不到核 -> 锁更不可能被释放" 形成活锁,
   外部表现就是 DOOM 还在跑、鼠标和桌面却冻住。 */
#define COMP_LOCK_SPIN 2048u
void Compositor::LockScan() {
    uint32_t spin = 0;
    while (__atomic_exchange_n(&scan_lock_, 1, __ATOMIC_ACQUIRE)) {
        if (spin < COMP_LOCK_SPIN) {
            __asm__ __volatile__("pause" ::: "memory");
            ++spin;
        } else {
            sys_yield();          /* 真让出, 给持锁者运行机会 */
            spin = 0;
        }
    }
}

/* 有界尝试: 抢不到立刻返回 false, 绝不阻塞。
   光标线程用它 —— 丢一帧采样(2ms 后再来)远好过自旋把 CPU 占死。 */
bool Compositor::TryLockScan(uint32_t budget) {
    for (uint32_t i = 0; i < budget; ++i) {
        if (!__atomic_exchange_n(&scan_lock_, 1, __ATOMIC_ACQUIRE)) return true;
        __asm__ __volatile__("pause" ::: "memory");
    }
    return false;
}
void Compositor::UnlockScan() {
    __atomic_store_n(&scan_lock_, 0, __ATOMIC_RELEASE);
}
struct CompScanGuard {
    Compositor* c;
    explicit CompScanGuard(Compositor* pm) : c(pm) { c->LockScan(); }
    ~CompScanGuard() { c->UnlockScan(); }
    CompScanGuard(const CompScanGuard&) = delete;
    CompScanGuard& operator=(const CompScanGuard&) = delete;
};

Compositor& Compositor::Get() { return g_compositor; }

bool Compositor::Init(FrameBuffer* screen) {
    if (!screen || !screen->BaseAddress || screen->Width == 0 || screen->Height == 0)
        return false;

    screen_     = *screen;              /* flat copy; BaseAddress stays valid */
    background_ = nullptr;
    bg_pitch_   = 0;
    ncpus_      = 0;
    launched_   = 0;
    shutdown_   = 0;
    strips_     = nullptr;
    layer_head_ = layer_tail_ = nullptr;
    list_lock_  = 0;
    frame_seq_  = 0;
    started_cnt_ = 0;
    done_compose_ = 0;
    for (uint32_t i = 0; i < COMP_CPUS_SANITY; i++) worker_done_seq_[i] = 0;
    cur_x_ = cur_y_ = 0;
    cur_visible_ = 0;
    cur_shape_ = SKY_CURSOR_ARROW;
    committed_x_ = committed_y_ = -1;
    dx0_ = dy0_ = dx1_ = dy1_ = 0;
    dirty_ = 0;
    cov_y0_ = cov_y1_ = 0;
    dyn_y0_ = dyn_y1_ = 0;
    force_full_ = 1;             /* 第一帧必须整屏铺一次壁纸 */

    /* Off-screen compose target, same layout as the scanout (pitch == PSL).
       All clear/stack work happens here so the visible front buffer is only
       ever touched by the final, whole-frame present pass. */
    /* 直写帧缓冲: 不再单独分配 4 MiB 的离屏副本, back_ 直接指向扫描出缓冲。
       代价是合成过程中屏幕上会看到"部分更新"的一帧 (不再有整帧原子呈现),
       收益是省下 4 MiB —— 对 64 MiB 目标这是关键的一块。
       光标的"干净背景"改由 16x16 快照提供 (见 blendCursorSquare/eraseCursor)。 */
    back_bytes_ = screen_.PixelsPerScanLine * screen_.Height * COMP_BPP;
    back_ = (uint32_t*)screen_.BaseAddress;
    if (!back_) return false;

    return true;
}

void Compositor::SetBackground(const uint32_t* bg) {
    CompScanGuard _sg(this);
    background_ = bg;
    /* 壁纸缓冲是紧凑排布 (RenderWallpaper: pitch == Width), 与扫描出的
       stride 未必相同, 不能共用 pitch 去索引。 */
    bg_pitch_ = (uint32_t)screen_.Width;
    force_full_ = 1;
}

void Compositor::ForceFullCompose(void) {
    CompScanGuard _sg(this);
    force_full_ = 1;
}

void Compositor::SetDynamicRows(int32_t y0, int32_t y1) {
    CompScanGuard _sg(this);
    dyn_y0_ = y0; dyn_y1_ = y1;
    force_full_ = 1;             /* 行带刚变更, 让它画一次再进入增量态 */
}

/* ---- registry spinlock --------------------------------------------------- */
void Compositor::LockList() {
    while (__atomic_test_and_set(&list_lock_, __ATOMIC_ACQUIRE)) cpu_relax();
}
void Compositor::UnlockList() {
    __atomic_clear(&list_lock_, __ATOMIC_RELEASE);
}

/* ---- dynamic standalone layer list, kept sorted bottom -> top by z ------- */
void Compositor::InsertLayerOrdered(CompLayer* layer) {
    CompLayer* at = layer_head_;
    while (at && at->z <= layer->z) at = at->l_next;

    layer->l_next = at;
    layer->l_prev = at ? at->l_prev : layer_tail_;

    if (at) {
        if (at->l_prev) at->l_prev->l_next = layer;
        else            layer_head_ = layer;        /* new bottom */
        at->l_prev = layer;
    } else {
        if (layer_tail_) layer_tail_->l_next = layer;
        else             layer_head_ = layer;       /* first layer */
        layer_tail_ = layer;                        /* new top */
    }
}

CompLayer* Compositor::CreateLayer(uint32_t z) {
    CompScanGuard _sg(this);
    CompLayer* layer = (CompLayer*)malloc(sizeof(CompLayer));
    if (!layer) return nullptr;

    layer->win_head     = layer->win_tail = nullptr;
    layer->window_count = 0;
    layer->z            = z;
    layer->l_next = layer->l_prev = nullptr;

    LockList();
    InsertLayerOrdered(layer);
    UnlockList();
    return layer;
}

bool Compositor::DestroyLayer(CompLayer* layer) {
    CompScanGuard _sg(this);
    if (!layer) return false;
    LockList();

    /* Retire every window-node wrapper owned by this layer (deferred free at
       the frame barrier, same as UnregisterWindow). The retire chain links
       through prev, so an in-flight worker following next is never derailed
       into the retire chain. */
    CompWinNode* n = layer->win_head;
    while (n) {
        CompWinNode* dead = n;
        n = n->next;
        dead->prev = g_retire_list;
        g_retire_list = dead;
    }

    if (layer->l_prev) layer->l_prev->l_next = layer->l_next;
    else               layer_head_ = layer->l_next;
    if (layer->l_next) layer->l_next->l_prev = layer->l_prev;
    else               layer_tail_ = layer->l_prev;

    /* ...and the layer itself joins the layer retire chain (its l_next keeps
       pointing at the old successor — see the comment on g_retire_layers). */
    layer->l_prev = g_retire_layers;
    g_retire_layers = layer;

    UnlockList();
    return true;                 /* actual free happens at the frame barrier */
}

/* ---- window registry (Window struct itself is never written) ------------- */
CompWinNode* Compositor::FindNode(Window* w) {
    for (CompLayer* L = layer_head_; L; L = L->l_next)
        for (CompWinNode* n = L->win_head; n; n = n->next)
            if (n->win == w) return n;
    return nullptr;
}

bool Compositor::RegisterWindow(Window* w, CompLayer* layer) {
    CompScanGuard _sg(this);
    if (!w || !layer) return false;

    LockList();
    if (FindNode(w)) { UnlockList(); return false; }   /* already registered */

    CompWinNode* node = (CompWinNode*)malloc(sizeof(CompWinNode));
    if (!node) { UnlockList(); return false; }

    node->win     = w;
    node->layer   = layer;
    node->visible = 1;
    node->next    = nullptr;
    node->prev    = layer->win_tail;

    if (layer->win_tail) layer->win_tail->next = node;
    else                 layer->win_head = node;
    layer->win_tail = node;
    layer->window_count++;

    UnlockList();
    return true;
}

CompLayer* Compositor::RegisterWindowAuto(Window* w) {
    CompScanGuard _sg(this);
    if (!w) return nullptr;

    LockList();
    if (FindNode(w)) { UnlockList(); return nullptr; }   /* already registered */

    CompLayer*   layer = (CompLayer*)malloc(sizeof(CompLayer));
    CompWinNode* node  = (CompWinNode*)malloc(sizeof(CompWinNode));
    if (!layer || !node) {
        free(layer);
        free(node);
        UnlockList();
        return nullptr;
    }

    /* z strictly above the current topmost layer (0 for the very first). */
    layer->win_head     = layer->win_tail = nullptr;
    layer->window_count = 0;
    layer->z            = layer_tail_ ? layer_tail_->z + 1u : 0u;
    layer->l_next = layer->l_prev = nullptr;
    InsertLayerOrdered(layer);                 /* by construction: new tail */

    node->win     = w;
    node->layer   = layer;
    node->visible = 1;
    node->next    = nullptr;
    node->prev    = nullptr;
    layer->win_head = layer->win_tail = node;
    layer->window_count = 1;

    UnlockList();
    return layer;
}

bool Compositor::UnregisterWindow(Window* w) {
    CompScanGuard _sg(this);
    if (!w) return false;
    LockList();

    CompWinNode* node = FindNode(w);
    if (!node) { UnlockList(); return false; }

    CompLayer* L = node->layer;
    if (node->prev) node->prev->next = node->next;
    else            L->win_head = node->next;
    if (node->next) node->next->prev = node->prev;
    else            L->win_tail = node->prev;
    L->window_count--;

    /* 节点延迟回收 —— 立即 free(node) 若与在途 worker 的
       列表遍历失步即 UAF; 改为退役链, 由 Compose 的屏障点统一释放。
       退役链走 prev (worker 只读 next), 被退役节点保持 next 指向原
       后继 —— 在途遍历无缝继续。 */
    node->prev = g_retire_list;
    g_retire_list = node;

    /* Auto layer management: a layer that just lost its LAST window dies
       with it — RegisterWindowAuto callers never destroy layers by hand.
       Like the node, the layer is only RETIRED here and freed at the frame
       barrier (a worker may be walking layer_head_ right now); its l_next
       keeps pointing at the old successor, so the walk "passes through"
       this now-empty layer into the still-registered stack. */
    if (L->window_count == 0) {
        if (L->l_prev) L->l_prev->l_next = L->l_next;
        else           layer_head_ = L->l_next;
        if (L->l_next) L->l_next->l_prev = L->l_prev;
        else           layer_tail_ = L->l_prev;

        L->l_prev = g_retire_layers;      /* retire chain via l_prev */
        g_retire_layers = L;
    }

    UnlockList();
    return true;
}

void Compositor::SetVisible(Window* w, bool visible) {
    CompScanGuard _sg(this);
    LockList();
    CompWinNode* n = FindNode(w);
    if (n) n->visible = visible ? 1 : 0;
    UnlockList();
}

void Compositor::MoveWindow(Window* w, uint32_t x, uint32_t y) {
    CompScanGuard _sg(this);
    if (!w) return;
    /* Position lives in the application Window; a release store publishes
       it before the next Compose() frame_seq release. */
    __atomic_store_n(&w->PosX, x, __ATOMIC_RELEASE);
    __atomic_store_n(&w->PosY, y, __ATOMIC_RELEASE);
}

/* ---- click-to-front: raise a window's layer above every other layer ------ */
void Compositor::RaiseWindow(Window* w) {
    CompScanGuard _sg(this);
    if (!w) return;
    LockList();

    CompWinNode* node = FindNode(w);
    if (!node || !node->layer) { UnlockList(); return; }
    CompLayer* L = node->layer;

    /* Already the topmost layer: nothing to reorder. */
    if (L == layer_tail_) { UnlockList(); return; }

    /* Detach L from the layer list at its current position. */
    if (L->l_prev) L->l_prev->l_next = L->l_next;
    else           layer_head_ = L->l_next;       /* L was the bottom */
    if (L->l_next) L->l_next->l_prev = L->l_prev;
    else           layer_tail_ = L->l_prev;       /* L was the top (handled above) */

    /* Give it a z strictly above the current top, then reinsert -> new tail. */
    L->z = layer_tail_->z + 1;
    InsertLayerOrdered(L);

    /* Auto layers mint a fresh z on every registration and every raise;
       relabel the stack bottom->top as 0..N-1 afterwards so the z field
       stays a dense rank and can never creep toward overflow in a long
       session. (List order is authoritative; z is only the insert key and
       the WM's topmost-hit comparison. Workers never read z.) */
    uint32_t zrel = 0;
    for (CompLayer* p = layer_head_; p; p = p->l_next) p->z = zrel++;

    UnlockList();
}

/* ========================================================================== */
/*  Phase 1: render one Y-range into the INVISIBLE back buffer.               */
/*  Traversal = layer list -> in-layer window list = O(window count).         */
/*  Because dst is off-screen, the clear-to-black transient is never visible. */
/* ========================================================================== */
void Compositor::ComposeStripToBack(uint32_t id) {
    ComposeRangeToBack(strips_[id].y0, strips_[id].y1,
                       (id < COMP_CPUS_SANITY) ? stage_[id] : nullptr, true);
}

/* 所有可见"内容窗口"覆盖的屏幕行带 (无内容窗口时 *y0 >= *y1)。
   壁纸窗口本身不算内容: 它就是 background_ 那份缓冲, 由背景 memcpy 负责。 */
static inline bool comp_is_bg_window(const Window* w, const uint32_t* bg,
                                     uint32_t sw, uint32_t H) {
    return bg && w->FbAddr == (uint64_t)bg &&
           w->PosX == 0 && w->PosY == 0 &&
           w->SizeX >= sw && w->SizeY >= H;
}

void Compositor::ComputeCoverSpan(uint32_t* y0, uint32_t* y1) const {
    const uint32_t H = (uint32_t)screen_.Height;
    const uint32_t sw = (uint32_t)screen_.Width;
    uint32_t lo = H, hi = 0;
    for (const CompLayer* L = layer_head_; L; L = L->l_next) {
        for (const CompWinNode* n = L->win_head; n; n = n->next) {
            const Window* w = n->win;
            if (!n->visible || !w || w->FbAddr == 0 || w->SizeY == 0) continue;
            if (comp_is_bg_window(w, background_, sw, H)) continue;
            uint32_t a = w->PosY, b = w->PosY + w->SizeY;
            if (b > H) b = H;
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        }
    }
    if (hi <= lo) { lo = hi = 0; }
    *y0 = lo; *y1 = hi;
}

/* 逐行合成: 先把这一行的**最终**像素做进暂存行, 再整行写回扫描出缓冲。
 *
 * 为什么不再是"先铺背景、再叠窗口"的两遍:
 * 直写帧缓冲之后, 那两遍之间的中间态是**可见的** —— 屏幕会先闪一下壁纸,
 * 再被窗口盖住, 这就是桌面上看到的闪烁。改成逐行之后, 每一行只对扫描出
 * 缓冲写一次, 而且写进去的已经是合成完的最终像素, 中间态永远不会上屏。
 *
 * 代价: 每个 worker 一份暂存行 (屏幕宽 x 4B, 1280 宽约 5 KiB)。
 * stage == nullptr 时退化为直写 (会闪, 但功能正常)。
 */
/* 光标方块边长。合成函数现在也要用到它, 而原来的定义在文件后面, 这里提前。 */
#ifndef SKY_CURS
#define SKY_CURS 16
#endif

void Compositor::ComposeRangeToBack(uint32_t ry0, uint32_t ry1, uint32_t* stage,
                                    bool allow_skip) {
    uint32_t*      dst   = back_;                 /* 扫描出缓冲 (或合成目标) */
    const uint32_t pitch = (uint32_t)screen_.PixelsPerScanLine;
    const uint32_t sw    = (uint32_t)screen_.Width;
    const uint32_t row_bytes = sw * COMP_BPP;
    const bool     force = force_full_ != 0;
    /* 背景可能是紧凑排布 (bg_pitch_ == Width); 未设置时退回扫描出 pitch */
    const uint32_t bgp = bg_pitch_ ? bg_pitch_ : pitch;

    for (uint32_t y = ry0; y < ry1; y++) {
        /* 先扫一遍本行: 最上层盖住它的窗口是谁, 以及有没有"内容窗口"
           (壁纸窗口不算内容 —— 它就是 background_ 那份缓冲)。
           (窗口数很少, 这一趟比一次整行 memcpy 便宜得多) */
        const Window* top = nullptr;
        bool content = false;
        for (const CompLayer* L = layer_head_; L; L = L->l_next) {
            for (const CompWinNode* node = L->win_head; node; node = node->next) {
                const Window* w = node->win;
                if (!node->visible || !w || w->FbAddr == 0) continue;
                if ((int64_t)y <  (int64_t)w->PosY) continue;
                if ((int64_t)y >= (int64_t)w->PosY + (int64_t)w->SizeY) continue;
                const int64_t cx0 = max_i64((int64_t)w->PosX, 0);
                const int64_t cx1 = min_i64((int64_t)w->PosX + (int64_t)w->SizeX, (int64_t)sw);
                if (cx0 >= cx1) continue;
                if (!comp_is_bg_window(w, background_, sw,
                                       (uint32_t)screen_.Height)) content = true;
                top = w;                 /* 自底向上遍历, 最后一次命中即最上层 */
            }
        }
        const bool cur_row = (cur_visible_ != 0) &&
                             ((int32_t)y >= cur_y_) && ((int32_t)y < cur_y_ + SKY_CURS);
        const bool dyn_row = ((int32_t)y >= dyn_y0_) && ((int32_t)y < dyn_y1_);

        /* ---- 快路 1: 静态背景行整行跳过 -------------------------------
           本帧没有内容窗口盖住、光标也不在本行, 上一帧同样没有 ——
           帧缓冲里那一行早就是壁纸了, 重新搬一遍壁纸再搬回去毫无意义。
           桌面常态 (620x400 的窗口 + 1280x800 屏) 能省掉约 400 行,
           约等于整帧 4 MB 的无用搬运。任务栏行带 (dyn) 永远不跳。 */
        if (allow_skip && !force && !content && !cur_row && !dyn_row &&
            !((int64_t)y >= (int64_t)cov_y0_ && (int64_t)y < (int64_t)cov_y1_))
            continue;

        /* ---- 快路 2: 整行被一个不透明窗口盖满, 且光标不在本行 ---------
           一次 memcpy 直出。省掉"壁纸行 + 窗口叠加 + 暂存行回写"三遍,
           最大化窗口 (最常见的桌面状态) 就靠这条路径把整屏成本降到 1/3。 */
        if (top && !cur_row && !top->HasAlpha &&
            (int64_t)top->PosX <= 0 &&
            (int64_t)top->PosX + (int64_t)top->SizeX >= (int64_t)sw) {
            const uint32_t sy = (uint32_t)((int64_t)y - (int64_t)top->PosY);
            const uint32_t sx = (uint32_t)(0 - (int64_t)top->PosX);
            const uint32_t* s = (const uint32_t*)top->FbAddr
                              + (uint64_t)sy * top->SizeX + sx;
            memcpy(dst + (uint64_t)y * pitch, s, row_bytes);
            continue;
        }

        /* 本行的合成目标: 优先用暂存行, 没有就直写扫描出缓冲 */
        uint32_t* row = stage ? stage : (dst + (uint64_t)y * pitch);

        /* 1) 背景 */
        if (background_)
            memcpy(row, background_ + (uint64_t)y * bgp, row_bytes);
        else
            memset(row, 0, row_bytes);

        /* 2) 叠加覆盖本行的窗口 (自底向上) */
        const uint32_t H = (uint32_t)screen_.Height;
        for (const CompLayer* L = layer_head_; L; L = L->l_next) {
            for (const CompWinNode* node = L->win_head; node; node = node->next) {
                const Window* w = node->win;
                if (!node->visible || !w || w->FbAddr == 0) continue;

                /* 壁纸窗口 == 上面第 1) 步刚 memcpy 的那块缓冲, 再叠一遍
                   纯属重复搬运 (整屏 4 MB)。 */
                if (comp_is_bg_window(w, background_, sw, H)) continue;

                if ((int64_t)y <  (int64_t)w->PosY) continue;
                if ((int64_t)y >= (int64_t)w->PosY + (int64_t)w->SizeY) continue;

                int64_t cx0 = max_i64((int64_t)w->PosX, 0);
                int64_t cx1 = min_i64((int64_t)w->PosX + (int64_t)w->SizeX, (int64_t)sw);
                if (cx0 >= cx1) continue;

                const uint32_t cw = (uint32_t)(cx1 - cx0);
                const uint32_t sy = (uint32_t)((int64_t)y - (int64_t)w->PosY);
                const uint32_t sx = (uint32_t)(cx0 - (int64_t)w->PosX);
                const uint32_t* sline = (const uint32_t*)w->FbAddr
                                      + (uint64_t)sy * w->SizeX + sx;
                uint32_t* dline = row + cx0;
                if (w->HasAlpha)
                    blend_row_argb(dline, sline, cw);
                else
                    memcpy(dline, sline, (size_t)cw * COMP_BPP);
            }
        }

        /* 3) 光标直接画进这一行。
           以前是"合成前先 eraseCursor、合成完再重画", 而 erase 到重画之间
           屏幕上**没有光标** —— 每一次 Compose 都会闪一下, 这就是鼠标闪烁。
           现在光标成为场景的一部分, 随行一起写回, 中间态不存在。 */
        if (cur_visible_) {
            const int32_t cy0 = cur_y_, cx0 = cur_x_;
            if ((int32_t)y >= cy0 && (int32_t)y < cy0 + SKY_CURS) {
                const int32_t gy = (int32_t)y - cy0;
                const char* const* gl = cursor_glyph(cur_shape_);
                if (gl && gy >= 0 && gy < SKY_CURS) {
                    const char* g = gl[gy];
                    for (int32_t cx = 0; cx < SKY_CURS; ++cx) {
                        const int32_t px = cx0 + cx;
                        if (px < 0 || px >= (int32_t)sw) continue;
                        if (g[cx] == '*')      row[px] = 0xFF000000u;
                        else if (g[cx] == 'O') row[px] = 0xFFFFFFFFu;
                    }
                }
            }
        }

        /* 4) 整行一次写回 —— 暂存模式下这是对扫描出缓冲唯一的写 */
        if (stage)
            memcpy(dst + (uint64_t)y * pitch, row, row_bytes);
    }
}

/* ========================================================================== */
/*  SINGLE-POINT SCANOUT COMMIT - dirty-rect blit + cursor-last overlay       */
/*  Scene work always lands in the invisible back_ first; the main thread is  */
/*  the ONLY writer of the visible framebuffer. It blits scene rows from back_*/
/*  while SKIPPING the (old/new) cursor squares, then paints the cursor last  */
/*  from the authoritative back_ backdrop, so a scene present never blanks    */
/*  the pointer (no flicker), and a pure move touches only two 16x16 boxes.   */
/* ========================================================================== */
#define SKY_CURS 16

#define SKY_CURS 16

/* Frame-barrier retire free (extended to layers). Runs ONLY
   on the main thread (both Compose paths are main-thread-only); the retire
   chains are mutated only by main-thread registry calls, and the barrier
   above guarantees no worker still walks the layer/window lists — so no lock
   is needed here. Node chain links through `prev`, layer chain through
   `l_prev` (walkers only ever read `next` / `l_next`). */
static void comp_reclaim_retired() {
    CompWinNode* rn = g_retire_list;
    CompLayer*   rl = g_retire_layers;
    g_retire_list   = nullptr;
    g_retire_layers = nullptr;
    while (rn) { CompWinNode* d = rn->prev; free(rn); rn = d; }
    while (rl) { CompLayer*   d = rl->l_prev; free(rl); rl = d; }
}

void Compositor::ComposeSingleThreaded() {
    /* Full-screen fallback when the strip table is absent or degraded. The
       old `for (i < ncpus_)` loop, after ANY degrade to ncpus_==1 that kept
       the N-strip table, rendered ONLY the top band and left the lower
       screen (taskbar included) permanently stale; with strips_ == nullptr
       it rendered nothing at all (or faulted). */
    uint32_t ny0 = 0, ny1 = 0;
    ComputeCoverSpan(&ny0, &ny1);          /* 本帧的窗口覆盖行带 */
    if (strips_ && ncpus_ > 1) {
        for (uint32_t i = 0; i < ncpus_; i++)
            ComposeRangeToBack(strips_[i].y0, strips_[i].y1, stage_[0], true);
    } else {
        ComposeRangeToBack(0, (uint32_t)screen_.Height, stage_[0], true);
    }
    cov_y0_ = ny0; cov_y1_ = ny1;          /* 下一帧据此补"窗口挪走"的行 */
    force_full_ = 0;
    /* 单线程模式无并发遍历, 退役链在此立即回收 (共享同一路径) */
    comp_reclaim_retired();
    /* 整屏行里已经包含光标 (ComposeRangeToBack 第 3 步), 这里只同步记账 */
    if (cur_visible_) { committed_x_ = cur_x_; committed_y_ = cur_y_; }
    dirty_ = 0;
}

/* ---------------------------------------------------------------------------
   光标作为一个"图层"参与合成。

   光标本来就不是"在显存上另画一笔": ComposeRangeToBack 的第 3 步会把它画进
   正在合成的每一行里 (见那里的注释: 这样整行一次写回, 中间态不存在, 不闪)。
   所以"移动光标"天然就是一次**局部重绘**: 旧位置所在的行重绘一次 (此时把
   cur_visible_ 临时清掉 => 旧箭头被场景内容覆盖), 新位置所在的行再重绘一次
   (场景 + 光标一起写回)。

   相比旧的旁路实现 (在扫描出缓冲上直接擦/贴 + 16x16 快照还原 + 避免区的
   特殊处理):
     * 唯一真相来源是"场景行", 不会出现快照失效导致的残影/双光标;
     * 每次移动只重画 2 x 16 行 x 1280 列 (合计约 0.5~1ms), 而整屏合成要
       10~19ms —— 鼠标响应不再被整屏合成拖住;
     * 与窗口/壁纸的重绘走同一条路径, 没有叠放顺序的特例。
   --------------------------------------------------------------------------- */
/* 损伤区域合成: 只重画 [y0,y1) 这些行 (整行宽)。
   场景行里包含光标 (ComposeRangeToBack 第 3 步), 所以光标/窗口内容/任务栏
   都走这一条路径 —— 这就是"鼠标是图层"的直接结果: 谁变了就只重画哪几行。 */
void Compositor::ComposeRows(int32_t y0, int32_t y1) {
    CompScanGuard _sg(this);
    if (!back_) return;
    const int32_t H = (int32_t)screen_.Height;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y0 >= y1) return;
    ComposeRangeToBack((uint32_t)y0, (uint32_t)y1, stage_[0], false);
    if (cur_visible_) { committed_x_ = cur_x_; committed_y_ = cur_y_; }
}

/* 光标移动用的就是上面那条"损伤行带"路径。 */
void Compositor::ComposeCursorBand(int32_t y0, int32_t y1) {
    /* 由 CursorMoveTo / SetCursorShape 在已持锁的前提下调用:
       这里直接走内部实现, 不能再进 ComposeRows (自旋锁不可重入)。 */
    if (!back_) return;
    const int32_t H = (int32_t)screen_.Height;
    int32_t y0c = y0 < 0 ? 0 : y0;
    int32_t y1c = y1 > H ? H : y1;
    if (y0c >= y1c) return;
    ComposeRangeToBack((uint32_t)y0c, (uint32_t)y1c, stage_[0], false);
    if (cur_visible_) { committed_x_ = cur_x_; committed_y_ = cur_y_; }
}

void Compositor::SetCursor(int32_t x, int32_t y, bool visible) {
    CompScanGuard _sg(this);
    const uint8_t v = visible ? 1u : 0u;
    /* 只改状态。调用点在初始化阶段 (场景还没铺开), 真正的绘制交给下一次
       Compose / CursorMoveTo。 */
    if (v != cur_visible_) { committed_x_ = committed_y_ = -1; dirty_ = 1; }
    cur_x_ = x; cur_y_ = y; cur_visible_ = v;
}

/* 已持扫描出锁时执行的光标重绘 (CursorMoveTo / CursorMoveToTry 共用) */
void Compositor::CursorMoveLocked(int32_t x, int32_t y) {
    if (x == cur_x_ && y == cur_y_) return;
    const uint64_t _t0 = comp_rdtsc();
    const int32_t ox = committed_x_, oy = committed_y_;
    const uint8_t vis = cur_visible_;

    /* 1) 旧位置: 临时藏起光标, 重绘那几行 -> 旧箭头被场景覆盖 (= 擦除) */
    if (vis && ox >= 0 && oy >= 0) {
        cur_visible_ = 0;
        ComposeCursorBand(oy, oy + SKY_CURS);
        cur_visible_ = vis;
    }
    /* 2) 新位置: 场景 + 光标一起写回 */
    cur_x_ = x; cur_y_ = y;
    if (cur_visible_) {
        ComposeCursorBand(y, y + SKY_CURS);
    } else {
        committed_x_ = committed_y_ = -1;
    }
    g_pf_band_cyc += comp_rdtsc() - _t0; g_pf_band_cnt++;
}

/* 阻塞版: 主线程/初始化路径用 (可接受等待) */
void Compositor::CursorMoveTo(int32_t x, int32_t y) {
    CompScanGuard _sg(this);
    CursorMoveLocked(x, y);
}

/* 非阻塞版: 光标线程专用。抢不到锁就丢掉这次采样, 绝不原地自旋。 */
bool Compositor::CursorMoveToTry(int32_t x, int32_t y) {
    if (x == cur_x_ && y == cur_y_) return true;      /* 没动, 无需重绘 */
    if (!TryLockScan(2048u)) { g_pf_lock_miss++; return false; }
    CursorMoveLocked(x, y);
    UnlockScan();
    return true;
}

/* 形状切换: 同位置先按"无光标"重绘一次擦掉旧字形, 再按新形状画一次。 */
void Compositor::SetCursorShape(uint32_t shape) {
    CompScanGuard _sg(this);
    if (shape == cur_shape_) return;
    cur_shape_ = shape;
    if (!cur_visible_ || committed_y_ < 0) return;
    const uint8_t vis = cur_visible_;
    cur_visible_ = 0;
    ComposeCursorBand(cur_y_, cur_y_ + SKY_CURS);
    cur_visible_ = vis;
    ComposeCursorBand(cur_y_, cur_y_ + SKY_CURS);
}


/* ---- dirty-rectangle scene API ----------------------------------------- */
/* Invalidate()/Present() 为无调用者的死代码 —— Compose 已走
   全帧 commitScene 路径; 已删除 (若未来要按脏矩形增量合成, 从
   git 历史恢复)。 */

/* Fast pointer path: the scene is untouched - only the old and new 16x16
   cursor dirty squares are refreshed straight from back_, so tracking costs
   O(16^2) with no recompose/full-screen present. */

/* Shape swap forces a repaint of the 16x16 cell, otherwise the old glyph
   would be left under the new one (the fast path skips identical cells). */

/* ---- worker: render its strip into the OFF-SCREEN back_ only ------------
 * Workers never touch the visible framebuffer; the main thread is the sole
 * scanout writer (single commit point), which keeps the cursor on top and
 * removes present/cursor flicker. */
void Compositor::WorkerEntry(uint32_t id) {
    __atomic_add_fetch(&started_cnt_, 1, __ATOMIC_RELEASE);

    uint64_t last_seq = 0;
    for (;;) {
        uint64_t seq;
        uint32_t wait = 0;
        do {                                   /* spin/yield until a frame */
            if (__atomic_load_n(&shutdown_, __ATOMIC_ACQUIRE)) return;
            seq = __atomic_load_n(&frame_seq_, __ATOMIC_ACQUIRE);
            if (seq != last_seq) break;
            comp_backoff(wait);
        } while (true);

        /* Shutdown check BEFORE rendering: once Compose()'s barrier timeout
           has degraded the compositor, the main thread finished this strip
           itself — a late-waking worker must exit WITHOUT writing back_
           again (a straggler render would tear the frame the main thread is
           committing). */
        if (__atomic_load_n(&shutdown_, __ATOMIC_ACQUIRE)) return;
        last_seq = seq;

        ComposeStripToBack(id);                 /* scene -> back_ only */
        /* Publish per-strip completion for THIS frame seq: Compose()'s
           timeout path reads it to learn which strips it must finish. */
        __atomic_store_n(&worker_done_seq_[id], seq, __ATOMIC_RELEASE);
        __atomic_add_fetch(&done_compose_, 1, __ATOMIC_RELEASE);
    }
}

/* C trampoline: kernel passes rdi = 0, so obtain the strip id from a ticket */
extern "C" void CompWorkerTrampoline() {
    /* id 0 is reserved for the main thread (help-the-work); workers own 1..N-1 */
    uint32_t id = 1u + __atomic_fetch_add(&g_strip_ticket, 1, __ATOMIC_SEQ_CST);
    Compositor::Get().WorkerEntry(id);
    /* 线程退出钩子 —— TLS 分配器队列归还 (原 cleanup 无调用点) */
    extern void allocator_thread_exit_cleanup(void);
    allocator_thread_exit_cleanup();

    /* 关键: 本函数既不能 return, 也不能调 sys_exit。
       1) 不能 return: 线程由 sys_thread_launch 创建, 栈上没有返回地址 ——
          末尾那条 `ret` 会从 [rsp] 弹出垃圾或直接读未映射页。
          实测崩溃与之吻合: CR2 == RSP(页对齐)、ERROR_CODE=4(读)、
          RIP 落在紧邻本行之前的 flush_tls_scb_all、其余寄存器全 0。
       2) 不能 sys_exit: 它走 Schedule::Exit, 退的是**整个进程** ——
          实测 worker 退出后桌面直接 GONE (没有任何段错误)。
       内核没有"只退出当前线程"的系统调用, 所以唯一安全做法是**原地停驻**:
       让出 CPU 永不返回。降级后这些 worker 本来也无事可做。 */
    /* 关键: 本函数既不能 return, 也不能调 sys_exit。
       1) 不能 return —— 已实测复现: 线程由 sys_thread_launch 创建, 栈上没有
          返回地址, 末尾的 `ret` 从 [rsp] 弹出垃圾。用户日志里的崩溃
          (CR2 == RSP 页对齐 / ERROR_CODE=4 读 / RIP 落在紧邻的
          flush_tls_scb_all) 就是这条 `ret` 在读未映射的栈顶页。
       2) 不能 sys_exit —— 它走 Schedule::Exit, 退的是**整个进程**:
          实测 worker 一退出桌面直接 GONE (且没有段错误)。
       内核没有"只退当前线程"的系统调用, 所以安全做法是**原地停驻**:
       让出 CPU 永不返回。降级后这些 worker 本来也无事可做。 */
    /* 停驻必须**睡眠**, 不能自旋。sys_yield() 只是让出后立刻被调度回来,
       等于一直占着一个 CPU —— 合成器一旦降级, 这些 worker 就变成 1~3 个
       永久空转的核, 正好在 DOOM 初始化这种重负载阶段抢走算力, 这也是
       "启动耗时忽快忽慢" 的一个来源。
       SYSCALL_SLEEP_MS 走内核定时器轮, 真正不占 CPU。 */
    for (;;) syscall(SYSCALL_SLEEP_MS, 1000, 0, 0, 0, 0, 0);
}

void Compositor::StartWorkers() {
    if (launched_) return;

    /* CPU count comes dynamically from sys_sysinfo(); release mapping after */
    SysInfo* si = (SysInfo*)sys_sysinfo(0);
    uint32_t n = 1;
    if ((int64_t)si >= 0) {
        n = si->ncpus ? si->ncpus : 1;
        sys_sysinfo((uint64_t)si);            /* hand SysInfo back to kernel */
    }
    if (n < 1) n = 1;
    if (n > COMP_CPUS_SANITY) n = COMP_CPUS_SANITY;   /* defensive only */
    /* worker 数量上限 (实测调优, 不是权宜之计)。
       worker 在屏障上是**自旋等待**的, 一直占着 CPU。4 个 vCPU 上
       "3 worker + DOOM + 桌面主线程 + 控制台" 属于超订, 重负载客户端
       (DOOM) 抢不到核。实测把 worker 从 3 降到 1:
           R_InitData  +18.2s -> +8.6s   (2.1x)
           第一帧       30.7s -> 19.6s
       而 DOOM 稳态帧率不变 (19.2 fps), 界面渲染/光标/时钟全部正常 ——
       说明满屏合成的并行度并不是瓶颈, 让出 CPU 给客户端更划算。
       机器核更多时可上调此上限; 但先要解决"worker 空转" (需要内核提供
       真正的 sleep, 现在只有 yield, 让出后立刻又会被调度回来)。 */
    /* 实测结论 (2026-10, 4 vCPU / 96 MiB, DOOM 运行中):
       并行分条 + 有界栅栏是**负优化** —— 栅栏等待占掉合成耗时的 47~84%,
       单帧最坏 125ms (平均 15~68ms); 而纯单线程 平均 13~25ms, 最坏 27ms,
       等待 0%。响应速度看的是最坏延迟, 所以这里固定 1: 只有主线程合成,
       worker 线程不再启动 (下面的代码保留, 需要时把 n 调大即可)。 */
    n = 1;
    ncpus_ = n;

    /* dynamically allocate the exact strip table for this CPU count */
    strips_ = (Strip*)malloc(n * sizeof(Strip));
    if (!strips_) {
        /* Alloc-fail fallback: ONE full-screen strip. (The old code only set
           ncpus_ = 1 but left n at the CPU count, so the carve loop below
           wrote n entries into a 1-entry buffer — a heap overflow.) */
        n = 1;
        ncpus_ = 1;
        strips_ = (Strip*)malloc(sizeof(Strip));
    }
    if (!strips_) {
        /* Total alloc failure: Compose()'s single-thread path renders a
           full-screen range with strips_ == nullptr; nothing to launch. */
        ncpus_ = 1;
        launched_ = 1;
        return;
    }

    /* one staging row per worker (flicker fix, see ComposeRangeToBack) */
    {
        const uint32_t row_bytes = (uint32_t)screen_.Width * COMP_BPP;
        for (uint32_t i = 0; i < COMP_CPUS_SANITY; i++) stage_[i] = nullptr;
        for (uint32_t i = 0; i < (uint32_t)n; i++) {
            stage_[i] = (uint32_t*)malloc(row_bytes);
            if (!stage_[i]) {          /* 退化为直写: 会闪, 但不会崩 */
                for (uint32_t j = 0; j < i; j++) { free(stage_[j]); stage_[j] = nullptr; }
                break;
            }
        }
    }

    /* carve N horizontal strips: equal width (== screen width), equal
       height (H/N); the last strip absorbs the remainder rows. */
    uint32_t H    = (uint32_t)screen_.Height;
    uint32_t band = H / n;
    if (band == 0) band = H;
    for (uint32_t i = 0; i < n; i++) {
        strips_[i].y0 = i * band;
        strips_[i].y1 = (i == n - 1) ? H : (i + 1) * band;
        if (strips_[i].y1 > H) strips_[i].y1 = H;
    }

    /* reset render barrier; workers render to off-screen back_ only */
    g_strip_ticket = 0;
    __atomic_store_n(&started_cnt_,  0, __ATOMIC_SEQ_CST);
    for (uint32_t i = 0; i < COMP_CPUS_SANITY; i++)
        __atomic_store_n(&worker_done_seq_[i], 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&frame_seq_,    0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shutdown_,     0, __ATOMIC_SEQ_CST);

    /* Strip 0 is rendered by the main thread itself; launch workers ONLY
       for the other cores. 统计实际成功的启动数 —— 原实现忽略
       返回值, 启动失败时下面的等待循环永转。 */
    uint32_t peers = (n > 1) ? n - 1 : 0;
    uint32_t launched_ok = 0;
    for (uint32_t i = 0; i < peers; i++) {
        if (sys_thread_launch((uint64_t)CompWorkerTrampoline, i + 1) >= 0)
            launched_ok++;
    }
    /* Wait for every launched worker to run its entry ack. The high safety
       bound only guards a wedged system and, if it is ever hit, shuts the
       workers down BEFORE degrading so no thread is left spinning on a
       frame_seq that single-thread mode would never bump. */
    uint32_t spin = 0, yields = 0;
    while (__atomic_load_n(&started_cnt_, __ATOMIC_ACQUIRE) < launched_ok) {
        if (spin < 512u) { cpu_relax(); ++spin; }
        else {
            sys_yield();
            spin = 0;
            if (++yields > 2000u) {
                __atomic_store_n(&shutdown_, 1, __ATOMIC_RELEASE);
                __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);
                /* Degrade to ONE full-screen strip. ncpus_ = 1 alone would
                   keep the N-strip table, so ComposeSingleThreaded would
                   render only strip 0 and the lower screen (taskbar) would
                   stay unrendered forever. */
                if (strips_) {
                    strips_[0].y0 = 0;
                    strips_[0].y1 = (uint32_t)screen_.Height;
                }
                ncpus_ = 1;
                launched_ = 1;
                return;
            }
        }
    }
    launched_ = 1;
}

void Compositor::Compose() {
    CompScanGuard _sg(this);
    if (!back_) return;          /* Init failed / Shutdown: nothing to compose */
    const uint64_t _t0 = comp_rdtsc();
    if (!launched_ || ncpus_ <= 1) {
        ComposeSingleThreaded();
        g_pf_compose_cyc += comp_rdtsc() - _t0; g_pf_compose_cnt++;
        return;
    }

    /* Peers are workers pinned to the OTHER cores; the main thread renders
       strip 0 itself (help-the-work). */
    const uint32_t peers = ncpus_ - 1;
    uint32_t ny0 = 0, ny1 = 0;
    ComputeCoverSpan(&ny0, &ny1);

    /* phase 1 (off-screen): peers render strips 1..N-1 to back_, main strip 0.
       Bump the frame sequence, then wait on the per-frame completion counter. */
    __atomic_store_n(&done_compose_, 0, __ATOMIC_RELEASE);
    const uint64_t seq = __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);
    ComposeStripToBack(0);

    /* BOUNDED barrier — the actual taskbar-click freeze fix. The old
       unbounded spin was the one true hard-freeze point reachable from a
       click: any input action that sets wmDirty (e.g. the taskbar toggle)
       funnels into Compose(); if a single pinned worker misses the frame
       (its core lost to a client that never yields, or the thread died),
       the wait never ended and the WHOLE desktop — cursor included, it
       lives in this same main loop — locked up. Now: after
       COMP_FRAME_YIELD_BUDGET yields the main thread finishes every strip
       that has not published completion for `seq` itself, then degrades
       permanently to single-thread. Worst case is one possibly torn frame
       (a straggler worker mid-render sees shutdown_ == 1 and exits without
       writing back_ again); never a hang. */
    uint32_t spin = 0, yields = 0;
    for (;;) {
        if (__atomic_load_n(&done_compose_, __ATOMIC_ACQUIRE) >= peers) break;
        if (spin < COMP_SPIN_BUDGET) { cpu_relax(); ++spin; continue; }
        sys_yield();
        spin = 0;
        if (++yields > COMP_FRAME_YIELD_BUDGET) {
            for (uint32_t i = 1; i < ncpus_; i++)
                if (__atomic_load_n(&worker_done_seq_[i], __ATOMIC_ACQUIRE) < seq)
                    ComposeStripToBack(i);      /* finish missing strips here */
            __atomic_store_n(&shutdown_, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE); /* wake */
            /* ONE full-screen strip from now on (see StartWorkers). */
            if (strips_) {
                strips_[0].y0 = 0;
                strips_[0].y1 = (uint32_t)screen_.Height;
            }
            ncpus_ = 1;
            break;
        }
    }

    /* barrier point (图层同样适用) — 所有 worker 已完成本帧的列表
       遍历 (或已判定降级), 退役节点与退役图层不再被引用, 可安全释放。 */
    comp_reclaim_retired();
    cov_y0_ = ny0; cov_y1_ = ny1;
    force_full_ = 0;

    /* 整屏行里已经包含光标, 只同步记账 (与 ComposeSingleThreaded 一致) */
    if (cur_visible_) { committed_x_ = cur_x_; committed_y_ = cur_y_; }
    dirty_ = 0;
    g_pf_compose_cyc += comp_rdtsc() - _t0; g_pf_compose_cnt++;
}

void Compositor::Shutdown() {
    __atomic_store_n(&shutdown_, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);   /* wake waiters */
    if (strips_) { free(strips_); strips_ = nullptr; }
    for (uint32_t i = 0; i < COMP_CPUS_SANITY; i++) {
        if (stage_[i]) { free(stage_[i]); stage_[i] = nullptr; }
    }
    back_ = nullptr;             /* 指向帧缓冲, 绝不能 free */
}