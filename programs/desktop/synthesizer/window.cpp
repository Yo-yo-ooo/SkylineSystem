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
    frame_seq_  = present_seq_ = 0;
    started_cnt_ = done_compose_ = done_present_ = 0;
    cur_x_ = cur_y_ = 0;
    cur_visible_ = 0;
    save_x_ = save_y_ = 0;
    save_valid_ = 0;
    cursor_save_ = nullptr;

    /* Off-screen compose target, same layout as the scanout (pitch == PSL).
       All clear/stack work happens here so the visible front buffer is only
       ever touched by the final, whole-frame present pass. */
    back_bytes_ = screen_.PixelsPerScanLine * screen_.Height * COMP_BPP;
    back_ = (uint32_t*)malloc((size_t)back_bytes_);
    if (!back_) return false;

    /* save-under scratch for the independent cursor overlay (16x16 px) */
    cursor_save_ = (uint32_t*)malloc(16u * 16u * COMP_BPP);
    if (!cursor_save_) { free(back_); back_ = nullptr; return false; }

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
/*  Phase 2: copy one finished strip from back buffer to visible scanout.     */
/*  Source is a complete frame, so the front is overwritten with final pixels */
/*  only — never a cleared/black intermediate state.                          */
/* ========================================================================== */
void Compositor::PresentStrip(uint32_t id) {
    const Strip&    s = strips_[id];
    const uint64_t  pitch = (uint64_t)screen_.PixelsPerScanLine;
    const uint8_t*  src = (const uint8_t*)back_;
    uint8_t*        dst = (uint8_t*)screen_.BaseAddress;

    uint64_t offset = (uint64_t)s.y0 * pitch * COMP_BPP;
    uint64_t bytes  = (uint64_t)(s.y1 - s.y0) * pitch * COMP_BPP;
    memcpy(dst + offset, src + offset, bytes);
}

void Compositor::ComposeSingleThreaded() {
    for (uint32_t i = 0; i < ncpus_; i++) ComposeStripToBack(i);
    /* present the SCENE ONLY -- the cursor is never part of the back buffer */
    memcpy(screen_.BaseAddress, back_, (size_t)back_bytes_);
    /* then overlay the independent cursor layer directly on the scanout */
    save_valid_ = 0;
    cursorStamp(cur_x_, cur_y_);
}

void Compositor::SetCursor(int32_t x, int32_t y, bool visible) {
    cur_x_ = x;
    cur_y_ = y;
    cur_visible_ = visible ? 1u : 0u;
}

/* ---- independent save-under cursor overlay, straight on the scanout ----
 * Composition (wallpaper+windows) renders the cursor-less scene into back_,
 * while the pointer is a separate layer that only ever touches the visible
 * framebuffer. cursorStamp() saves the 16x16 backdrop it is about to cover,
 * then draws the arrow; cursorRestore() writes that backdrop back to erase
 * it. A pointer move is therefore a 16x16 erase+stamp = O(16^2), with no
 * full-screen recompose/present. Clips at every edge. */
void Compositor::cursorRestore() {
    if (!save_valid_ || !cursor_save_) return;
    uint32_t* fb = (uint32_t*)screen_.BaseAddress;
    const int32_t pitch = (int32_t)screen_.PixelsPerScanLine;
    const int32_t W = (int32_t)screen_.Width;
    const int32_t H = (int32_t)screen_.Height;
    for (int cy = 0; cy < 16; ++cy) {
        int32_t py = save_y_ + cy;
        if (py < 0 || py >= H) continue;
        uint32_t* d = fb + (uint64_t)py * pitch;
        const uint32_t* s = cursor_save_ + cy * 16;
        for (int cx = 0; cx < 16; ++cx) {
            int32_t px = save_x_ + cx;
            if (px < 0 || px >= W) continue;
            d[px] = s[cx];
        }
    }
    save_valid_ = 0;
}

void Compositor::cursorStamp(int32_t x, int32_t y) {
    if (!cur_visible_ || !screen_.BaseAddress || !cursor_save_) return;
    uint32_t* fb = (uint32_t*)screen_.BaseAddress;
    const int32_t pitch = (int32_t)screen_.PixelsPerScanLine;
    const int32_t W = (int32_t)screen_.Width;
    const int32_t H = (int32_t)screen_.Height;
    cur_x_ = x; cur_y_ = y;

    /* 1) save the backdrop under the new 16x16 square */
    for (int cy = 0; cy < 16; ++cy) {
        int32_t py = y + cy;
        uint32_t* srow = cursor_save_ + cy * 16;
        if (py < 0 || py >= H) continue;
        const uint32_t* src = fb + (uint64_t)py * pitch;
        for (int cx = 0; cx < 16; ++cx) {
            int32_t px = x + cx;
            srow[cx] = (px >= 0 && px < W) ? src[px] : 0u;
        }
    }

    /* 2) stamp the arrow glyph over the saved region */
    for (int cy = 0; cy < 16; ++cy) {
        int32_t py = y + cy;
        if (py < 0 || py >= H) continue;
        const char* row = kCursorArrow[cy];
        uint32_t* d = fb + (uint64_t)py * pitch;
        for (int cx = 0; cx < 16; ++cx) {
            int32_t px = x + cx;
            if (px < 0 || px >= W) continue;
            char g = row[cx];
            if (g == '*')      d[px] = 0xFF000000u;   /* black outline */
            else if (g == 'O') d[px] = 0xFFFFFFFFu;   /* white fill    */
            /* '.' transparent: the saved backdrop already sits there */
        }
    }
    save_x_ = x; save_y_ = y; save_valid_ = 1;
}

/* Fast pointer path used by the main loop: erase the old square from its
   saved backdrop and stamp the new one, entirely on the scanout. */
void Compositor::CursorMoveTo(int32_t x, int32_t y) {
    if (x == cur_x_ && y == cur_y_ && save_valid_) return;
    cursorRestore();
    cursorStamp(x, y);
}

/* ---- worker: compose to back, barrier, then present ---------------------- */
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

        ComposeStripToBack(id);                                   /* phase 1 */
        __atomic_add_fetch(&done_compose_, 1, __ATOMIC_RELEASE);

        /* hold present until the main thread confirms the WHOLE back frame */
        uint32_t pwait = 0;
        while (__atomic_load_n(&present_seq_, __ATOMIC_ACQUIRE) < seq)
            comp_backoff(pwait);

        PresentStrip(id);                                         /* phase 2 */
        __atomic_add_fetch(&done_present_, 1, __ATOMIC_RELEASE);
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

    /* reset barrier and launch one pinned worker per CPU (hint == cpu id) */
    g_strip_ticket = 0;
    __atomic_store_n(&started_cnt_,  0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&done_compose_, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&done_present_, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&frame_seq_,    0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&present_seq_,  0, __ATOMIC_SEQ_CST);
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

    /* phase 1: publish frame; peers render strips 1..N-1, main does strip 0 */
    __atomic_store_n(&done_compose_, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&done_present_, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);
    ComposeStripToBack(0);
    /* Peers run truly in parallel and finish in tens of us: spin (PAUSE)
       instead of paying a full scheduler round-trip; comp_backoff only
       yields after a bounded budget if a peer was ever preempted. */
    uint32_t cw = 0;
    while (__atomic_load_n(&done_compose_, __ATOMIC_ACQUIRE) < peers)
        comp_backoff(cw);

    /* Release peers to present the cursor-less scene. */
    __atomic_store_n(&present_seq_, frame_seq_, __ATOMIC_RELEASE);

    /* phase 2: main presents its own strip 0, then wait only for the peers */
    PresentStrip(0);
    uint32_t pw = 0;
    while (__atomic_load_n(&done_present_, __ATOMIC_ACQUIRE) < peers)
        comp_backoff(pw);

    /* Every scene strip is now on the scanout; overlay the independent
       cursor layer LAST, directly on the framebuffer, so no present strip
       can overwrite it. The wholesale present invalidated the save-under. */
    save_valid_ = 0;
    cursorStamp(cur_x_, cur_y_);
}

void Compositor::Shutdown() {
    __atomic_store_n(&shutdown_, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&frame_seq_, 1, __ATOMIC_RELEASE);   /* wake waiters */
    if (strips_)      { free(strips_);      strips_ = nullptr; }
    if (cursor_save_) { free(cursor_save_); cursor_save_ = nullptr; }
    if (back_)        { free(back_);       back_ = nullptr; }
}
