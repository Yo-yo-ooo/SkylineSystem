//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// synthesizer/window.cpp — double-buffered horizontal-strip compositor
#include <synthesizer/window.h>
#include <syscall.h>
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
/* P1-53: 卸载窗口的退役节点链 (LockList 保护写入), 由 Compose 的
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
#define COMP_FRAME_YIELD_BUDGET 512u
static inline void comp_backoff(uint32_t &spin) {
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

/* ========================================================================== */
/*  Compositor                                                                */
/* ========================================================================== */
Compositor& Compositor::Get() { return g_compositor; }

bool Compositor::Init(FrameBuffer* screen) {
    if (!screen || !screen->BaseAddress || screen->Width == 0 || screen->Height == 0)
        return false;

    screen_     = *screen;              /* flat copy; BaseAddress stays valid */
    background_ = nullptr;
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
    committed_x_ = committed_y_ = -1;
    dx0_ = dy0_ = dx1_ = dy1_ = 0;
    dirty_ = 0;

    /* Off-screen compose target, same layout as the scanout (pitch == PSL).
       All clear/stack work happens here so the visible front buffer is only
       ever touched by the final, whole-frame present pass. */
    back_bytes_ = screen_.PixelsPerScanLine * screen_.Height * COMP_BPP;
    back_ = (uint32_t*)malloc((size_t)back_bytes_);
    if (!back_) return false;

    return true;
}

void Compositor::SetBackground(const uint32_t* bg) { background_ = bg; }

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

    /* P1-53: 节点延迟回收 —— 立即 free(node) 若与在途 worker 的
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
    LockList();
    CompWinNode* n = FindNode(w);
    if (n) n->visible = visible ? 1 : 0;
    UnlockList();
}

void Compositor::MoveWindow(Window* w, uint32_t x, uint32_t y) {
    if (!w) return;
    /* Position lives in the application Window; a release store publishes
       it before the next Compose() frame_seq release. */
    __atomic_store_n(&w->PosX, x, __ATOMIC_RELEASE);
    __atomic_store_n(&w->PosY, y, __ATOMIC_RELEASE);
}

/* ---- click-to-front: raise a window's layer above every other layer ------ */
void Compositor::RaiseWindow(Window* w) {
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
    ComposeRangeToBack(strips_[id].y0, strips_[id].y1);
}

void Compositor::ComposeRangeToBack(uint32_t ry0, uint32_t ry1) {
    uint32_t*      dst   = back_;                 /* off-screen target        */
    const uint32_t pitch = (uint32_t)screen_.PixelsPerScanLine;
    const uint32_t sw    = (uint32_t)screen_.Width;
    const uint32_t row_bytes = sw * COMP_BPP;

    /* 1) repaint this range's backdrop in the back buffer */
    for (uint32_t y = ry0; y < ry1; y++) {
        uint32_t* dline = dst + (uint64_t)y * pitch;
        if (background_)
            memcpy(dline, background_ + (uint64_t)y * pitch, row_bytes);
        else
            memset(dline, 0, row_bytes);
    }

    /* 2) stack windows: standalone layer list (bottom -> top), then the
          window-node list inside each layer. O(window count) visits. */
    for (const CompLayer* L = layer_head_; L; L = L->l_next) {
        for (const CompWinNode* node = L->win_head; node; node = node->next) {
            const Window* w = node->win;
            if (!node->visible || !w || w->FbAddr == 0) continue;

            /* clip window rect against this range and the screen width */
            int64_t cy0 = max_i64((int64_t)w->PosY, (int64_t)ry0);
            int64_t cy1 = min_i64((int64_t)w->PosY + w->SizeY, (int64_t)ry1);
            if (cy0 >= cy1) continue;                          /* misses range */

            int64_t cx0 = max_i64((int64_t)w->PosX, (int64_t)0);
            int64_t cx1 = min_i64((int64_t)w->PosX + w->SizeX, (int64_t)sw);
            if (cx0 >= cx1) continue;                          /* off-screen   */

            const uint32_t  cw      = (uint32_t)(cx1 - cx0);
            const uint32_t  wpitch  = w->SizeX;                /* window pitch */
            const uint32_t* src     = (const uint32_t*)w->FbAddr;
            const uint32_t  copy_b  = cw * COMP_BPP;

            for (int64_t y = cy0; y < cy1; y++) {
                uint32_t sy = (uint32_t)(y  - w->PosY);
                uint32_t sx = (uint32_t)(cx0 - w->PosX);
                const uint32_t* sline = src + (uint64_t)sy * wpitch + sx;
                uint32_t*       dline = dst + (uint64_t)y * pitch + cx0;
                if (w->HasAlpha)
                    blend_row_argb(dline, sline, cw);   /* rounded/shadowed    */
                else
                    memcpy(dline, sline, copy_b);       /* opaque, whole rows  */
            }
        }
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

/* Dirty-rectangle commit: push back_->fb over [x0,x1)x[y0,y1), leaving the  */
/* up-to-two cursor squares (old ox,oy / new nx,ny) untouched, and writing a  */
/* scene span ONLY where its pixels differ (compare-and-blit). Static frames */
/* therefore perform zero scanout writes outside the cursor squares.         */
void Compositor::blitSceneAvoidCursor(int32_t x0,int32_t y0,int32_t x1,int32_t y1,
                                      int32_t ox,int32_t oy,int32_t nx,int32_t ny) {
    uint32_t* fb = (uint32_t*)screen_.BaseAddress;
    const int32_t pitch = (int32_t)screen_.PixelsPerScanLine;
    const int32_t W = (int32_t)screen_.Width, H = (int32_t)screen_.Height;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;
    const int32_t cyA[2] = { oy, ny }, cxA[2] = { ox, nx };
    for (int32_t y = y0; y < y1; ++y) {
        const uint32_t* s = back_ + (uint64_t)y * pitch;
        uint32_t*       d = fb   + (uint64_t)y * pitch;
        int32_t exL[2], exR[2]; int ne = 0;
        for (int r = 0; r < 2; ++r) {
            if (y >= cyA[r] && y < cyA[r] + SKY_CURS) {
                exL[ne] = cxA[r]; exR[ne] = cxA[r] + SKY_CURS; ++ne;
            }
        }
        for (int a = 1; a < ne; ++a)          /* sort <=2 spans by left edge */
            if (exL[a] < exL[a-1]) {
                int32_t t; t=exL[a];exL[a]=exL[a-1];exL[a-1]=t;
                t=exR[a];exR[a]=exR[a-1];exR[a-1]=t;
            }
        int32_t cur = x0;
        for (int e = 0; e < ne; ++e) {
            int32_t a = exL[e], b = exR[e];
            if (a < 0) a = 0; if (b > W) b = W;
            if (a > cur && !span_eq_u32(d + cur, s + cur, (uint32_t)(a - cur)))
                memcpy(d + cur, s + cur, (size_t)(a - cur) * COMP_BPP);
            if (b > cur) cur = b;
        }
        if (cur < x1 && !span_eq_u32(d + cur, s + cur, (uint32_t)(x1 - cur)))
            memcpy(d + cur, s + cur, (size_t)(x1 - cur) * COMP_BPP);
    }
}

/* Copy the 16x16 scene at (x,y) from the authoritative back_ onto the fb. */
void Compositor::paintSquareFromBack(int32_t x, int32_t y) {
    uint32_t* fb = (uint32_t*)screen_.BaseAddress;
    const int32_t pitch = (int32_t)screen_.PixelsPerScanLine;
    const int32_t W = (int32_t)screen_.Width, H = (int32_t)screen_.Height;
    for (int cy = 0; cy < SKY_CURS; ++cy) {
        int32_t py = y + cy; if (py < 0 || py >= H) continue;
        const uint32_t* s = back_ + (uint64_t)py * pitch;
        uint32_t*       d = fb   + (uint64_t)py * pitch;
        for (int cx = 0; cx < SKY_CURS; ++cx) {
            int32_t px = x + cx; if (px < 0 || px >= W) continue;
            d[px] = s[px];
        }
    }
}

/* Build the FINAL 16x16 square (backdrop from back_ + arrow merged) in a local
   cell, then publish it to the fb in one pass. The cursor square therefore
   never passes through a cursor-less state (no 'clear then redraw' window a
   scanline or snapshot could catch -> no flicker). */
void Compositor::blendCursorSquare(int32_t x, int32_t y) {
    uint32_t cell[SKY_CURS * SKY_CURS];
    uint32_t* fb = (uint32_t*)screen_.BaseAddress;
    const int32_t pitch = (int32_t)screen_.PixelsPerScanLine;
    const int32_t W = (int32_t)screen_.Width, H = (int32_t)screen_.Height;

    /* 1) backdrop from the authoritative cursor-less scene in back_ */
    for (int cy = 0; cy < SKY_CURS; ++cy) {
        int32_t py = y + cy;
        uint32_t* crow = cell + cy * SKY_CURS;
        if (py < 0 || py >= H) { for (int cx=0;cx<SKY_CURS;++cx) crow[cx]=0; continue; }
        const uint32_t* srow = back_ + (uint64_t)py * pitch;
        for (int cx = 0; cx < SKY_CURS; ++cx) {
            int32_t px = x + cx;
            crow[cx] = (px >= 0 && px < W) ? srow[px] : 0u;
        }
    }
    /* 2) merge the arrow glyph into the finished cell */
    if (cur_visible_)
        for (int cy = 0; cy < SKY_CURS; ++cy) {
            const char* row = kCursorArrow[cy];
            uint32_t* crow = cell + cy * SKY_CURS;
            for (int cx = 0; cx < SKY_CURS; ++cx) {
                char g = row[cx];
                if (g == '*')      crow[cx] = 0xFF000000u;  /* black outline */
                else if (g == 'O') crow[cx] = 0xFFFFFFFFu;  /* white fill    */
            }
        }
    /* 3) publish the finished square; compare-and-blit each row so a static
          cursor writes NOTHING to the scanout (fb stays byte-identical, hence
          no transient a scanline/snapshot could ever catch -> no flicker). */
    int32_t xs = (x < 0) ? 0 : x;
    int32_t xe = x + SKY_CURS; if (xe > W) xe = W;
    for (int cy = 0; cy < SKY_CURS; ++cy) {
        int32_t py = y + cy; if (py < 0 || py >= H) continue;
        if (xs >= xe) continue;
        uint32_t*       d = fb + (uint64_t)py * pitch + xs;
        const uint32_t* s = cell + cy * SKY_CURS + (xs - x);
        size_t bytes = (size_t)(xe - xs) * COMP_BPP;
        if (!span_eq_u32(d, s, (uint32_t)(xe - xs))) memcpy(d, s, bytes);
    }
}

/* Cursor is the LAST scanout write of a frame. P1-44: 先恢复旧方块再画新
   箭头。Tracks the on-screen square in committed_ so an in-flight scene blit
   knows which arrow to preserve. */
void Compositor::overlayCursorFinal(int32_t ox,int32_t oy,int32_t nx,int32_t ny) {
    if ((ox != nx || oy != ny) && ox >= 0 && oy >= 0)
        paintSquareFromBack(ox, oy);
    blendCursorSquare(nx, ny);
    cur_x_ = committed_x_ = nx;
    cur_y_ = committed_y_ = ny;
}

/* Commit one scene rectangle, then finalize the cursor: compose the scene
   FIRST and draw the pointer LAST, at its freshest position. */
void Compositor::commitScene(int32_t x0,int32_t y0,int32_t x1,int32_t y1,
                             int32_t ox,int32_t oy,int32_t nx,int32_t ny) {
    blitSceneAvoidCursor(x0,y0,x1,y1,ox,oy,nx,ny);
    overlayCursorFinal(ox,oy,nx,ny);
}

/* Frame-barrier retire free (P1-53 semantics, extended to layers). Runs ONLY
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
    if (strips_ && ncpus_ > 1) {
        for (uint32_t i = 0; i < ncpus_; i++)
            ComposeRangeToBack(strips_[i].y0, strips_[i].y1);
    } else {
        ComposeRangeToBack(0, (uint32_t)screen_.Height);
    }
    /* P1-53: 单线程模式无并发遍历, 退役链在此立即回收 (共享同一路径) */
    comp_reclaim_retired();
    /* scene finished off-screen; single commit preserves then redraws cursor */
    commitScene(0, 0, (int32_t)screen_.Width, (int32_t)screen_.Height,
                committed_x_, committed_y_, cur_x_, cur_y_);
    dirty_ = 0;
}

void Compositor::SetCursor(int32_t x, int32_t y, bool visible) {
    uint8_t v = visible ? 1u : 0u;
    if (v != cur_visible_) { committed_x_ = committed_y_ = -1; } /* force repaint */
    cur_x_ = x;
    cur_y_ = y;
    cur_visible_ = v;
}

/* ---- dirty-rectangle scene API ----------------------------------------- */
/* P5-96: Invalidate()/Present() 为无调用者的死代码 —— Compose 已走
   全帧 commitScene 路径; 已删除 (若未来要按脏矩形增量合成, 从
   git 历史恢复)。 */

/* Fast pointer path: the scene is untouched - only the old and new 16x16
   cursor dirty squares are refreshed straight from back_, so tracking costs
   O(16^2) with no recompose/full-screen present. */
void Compositor::CursorMoveTo(int32_t x, int32_t y) {
    if (x == committed_x_ && y == committed_y_) return;
    overlayCursorFinal(committed_x_, committed_y_, x, y);
}

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
    /* P0-18: 线程退出钩子 —— TLS 分配器队列归还 (原 cleanup 无调用点) */
    extern void allocator_thread_exit_cleanup(void);
    allocator_thread_exit_cleanup();
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
       for the other cores. P1-48: 统计实际成功的启动数 —— 原实现忽略
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
    if (!back_) return;          /* Init failed / Shutdown: nothing to compose */
    if (!launched_ || ncpus_ <= 1) { ComposeSingleThreaded(); return; }

    /* Peers are workers pinned to the OTHER cores; the main thread renders
       strip 0 itself (help-the-work). */
    const uint32_t peers = ncpus_ - 1;

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

    /* P1-53 (扩展到图层): barrier point — 所有 worker 已完成本帧的列表
       遍历 (或已判定降级), 退役节点与退役图层不再被引用, 可安全释放。 */
    comp_reclaim_retired();

    /* phase 2 (single commit point, main thread only): push the whole scene
       to the scanout while preserving the cursor square, then draw the
       cursor LAST at its freshest position. */
    commitScene(0, 0, (int32_t)screen_.Width, (int32_t)screen_.Height,
                committed_x_, committed_y_, cur_x_, cur_y_);
    dirty_ = 0;
}

void Compositor::Shutdown() {
    __atomic_store_n(&shutdown_, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);   /* wake waiters */
    if (strips_) { free(strips_); strips_ = nullptr; }
    if (back_)   { free(back_);   back_ = nullptr; }
}