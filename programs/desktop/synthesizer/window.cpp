//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// synthesizer/window.cpp — double-buffered horizontal-strip compositor
#include <synthesizer/window.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>

/*
 * Worker threads are pinned 1:1 to CPUs through sys_thread_launch's hint.
 * The kernel forces the new thread's rdi = 0 (no argument passing), so a
 * strip id cannot be handed in directly; an atomic ticket pool assigns one.
 */
static uint32_t g_strip_ticket = 0;

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
static inline void comp_backoff(uint32_t &spin) {
    if (spin < COMP_SPIN_BUDGET) { cpu_relax(); ++spin; }
    else { sys_yield(); spin = 0; }
}


static inline int64_t max_i64(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t min_i64(int64_t a, int64_t b) { return a < b ? a : b; }

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
    started_cnt_ = done_compose_ = 0;
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

    /* free every window-node wrapper owned by this layer */
    CompWinNode* n = layer->win_head;
    while (n) {
        CompWinNode* dead = n;
        n = n->next;
        free(dead);
    }

    if (layer->l_prev) layer->l_prev->l_next = layer->l_next;
    else               layer_head_ = layer->l_next;
    if (layer->l_next) layer->l_next->l_prev = layer->l_prev;
    else               layer_tail_ = layer->l_prev;

    UnlockList();
    free(layer);                 /* layer node itself is dynamic */
    return true;
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

    UnlockList();
    free(node);
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

/* ========================================================================== */
/*  Phase 1: render one strip into the INVISIBLE back buffer.                 */
/*  Traversal = layer list -> in-layer window list = O(window count).         */
/*  Because dst is off-screen, the clear-to-black transient is never visible. */
/* ========================================================================== */
void Compositor::ComposeStripToBack(uint32_t id) {
    const Strip& s = strips_[id];

    uint32_t*      dst   = back_;                 /* off-screen target        */
    const uint32_t pitch = (uint32_t)screen_.PixelsPerScanLine;
    const uint32_t sw    = (uint32_t)screen_.Width;
    const uint32_t row_bytes = sw * COMP_BPP;

    /* 1) repaint this strip's backdrop in the back buffer */
    for (uint32_t y = s.y0; y < s.y1; y++) {
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

            /* clip window rect against this strip and the screen width */
            int64_t cy0 = max_i64((int64_t)w->PosY, (int64_t)s.y0);
            int64_t cy1 = min_i64((int64_t)w->PosY + w->SizeY, (int64_t)s.y1);
            if (cy0 >= cy1) continue;                          /* misses strip */

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
                memcpy(dline, sline, copy_b);                  /* whole rows   */
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
            /* dirty-rect compare-and-blit: write a span ONLY if the scene
               actually changed. On a static frame every span compares equal
               and the scanout is left byte-for-byte untouched, so there is no
               full-screen rewrite window a scanline/snapshot could catch. */
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

/* Cursor is the LAST scanout write of a frame. Draw the NEW finished square
   FIRST, then restore the OLD square to the plain scene. At every instant at
   least one arrow is present (two coexist only for nanoseconds), so the
   pointer can never vanish -> no flicker. Tracks the on-screen square in
   committed_ so an in-flight scene blit knows which arrow to preserve. */
void Compositor::overlayCursorFinal(int32_t ox,int32_t oy,int32_t nx,int32_t ny) {
    blendCursorSquare(nx, ny);
    if ((ox != nx || oy != ny) && ox >= 0 && oy >= 0)
        paintSquareFromBack(ox, oy);
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

void Compositor::ComposeSingleThreaded() {
    for (uint32_t i = 0; i < ncpus_; i++) ComposeStripToBack(i);
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
void Compositor::Invalidate(int32_t x0,int32_t y0,int32_t x1,int32_t y1) {
    if (x0 >= x1 || y0 >= y1) return;
    if (!dirty_) { dx0_=x0; dy0_=y0; dx1_=x1; dy1_=y1; dirty_=1; }
    else {
        if (x0 < dx0_) dx0_ = x0; if (y0 < dy0_) dy0_ = y0;
        if (x1 > dx1_) dx1_ = x1; if (y1 > dy1_) dy1_ = y1;
    }
}

/* Push only the accumulated scene dirty rectangle to the scanout, keeping
   the cursor on top; clears the dirty union afterwards. */
void Compositor::Present() {
    if (!back_ || !dirty_) return;
    commitScene(dx0_,dy0_,dx1_,dy1_, committed_x_,committed_y_,cur_x_,cur_y_);
    dirty_ = 0;
}

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
    while (!__atomic_load_n(&shutdown_, __ATOMIC_ACQUIRE)) {
        uint64_t seq;
        uint32_t wait = 0;
        do {                                   /* spin/yield until a frame */
            seq = __atomic_load_n(&frame_seq_, __ATOMIC_ACQUIRE);
            if (seq != last_seq) break;
            if (__atomic_load_n(&shutdown_, __ATOMIC_ACQUIRE)) return;
            comp_backoff(wait);
        } while (true);
        last_seq = seq;

        ComposeStripToBack(id);                 /* scene -> back_ only */
        __atomic_add_fetch(&done_compose_, 1, __ATOMIC_RELEASE);
    }
}

/* C trampoline: kernel passes rdi = 0, so obtain the strip id from a ticket */
extern "C" void CompWorkerTrampoline() {
    /* id 0 is reserved for the main thread (help-the-work); workers own 1..N-1 */
    uint32_t id = 1u + __atomic_fetch_add(&g_strip_ticket, 1, __ATOMIC_SEQ_CST);
    Compositor::Get().WorkerEntry(id);
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
    if (!strips_) { ncpus_ = 1; strips_ = (Strip*)malloc(sizeof(Strip)); }

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
    __atomic_store_n(&done_compose_, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&frame_seq_,    0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shutdown_,     0, __ATOMIC_SEQ_CST);

    /* Strip 0 is rendered by the main thread itself; launch workers ONLY
       for the other cores so each frame barrier never has to hand this core
       to a same-core worker. */
    uint32_t peers = (n > 1) ? n - 1 : 0;
    for (uint32_t i = 0; i < peers; i++) {
        sys_thread_launch((uint64_t)CompWorkerTrampoline, i + 1);
    }
    uint32_t sw = 0;
    while (__atomic_load_n(&started_cnt_, __ATOMIC_ACQUIRE) < peers)
        comp_backoff(sw);
    launched_ = 1;
}

void Compositor::Compose() {
    if (!back_) { ComposeSingleThreaded(); return; }
    if (!launched_ || ncpus_ <= 1) { ComposeSingleThreaded(); return; }

    /* Peers are workers pinned to the OTHER cores; the main thread renders
       strip 0 itself (help-the-work). The old design launched a worker on
       EVERY core including this one and sys_yield()'d at each barrier: on a
       shared core that is main<->worker ping-pong through the scheduler, and
       frame gaps jittered by whole scheduling quanta (stuttery pointer). */
    const uint32_t peers = ncpus_ - 1;

    /* phase 1 (off-screen): peers render strips 1..N-1 to back_, main strip 0 */
    __atomic_store_n(&done_compose_, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);
    ComposeStripToBack(0);
    uint32_t cw = 0;
    while (__atomic_load_n(&done_compose_, __ATOMIC_ACQUIRE) < peers)
        comp_backoff(cw);

    /* phase 2 (single commit point, main thread only): push the whole scene
       to the scanout while preserving the cursor square, then draw the
       cursor LAST at its freshest position. Workers never touch the fb, so
       the pointer is never erased by an in-flight present -> no flicker. */
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
