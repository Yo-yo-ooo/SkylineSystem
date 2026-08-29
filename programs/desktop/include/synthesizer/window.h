//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <graphic/fb.h>

/*
 * ============================================================================
 *  Desktop Parallel Compositor  (double-buffered)
 * ----------------------------------------------------------------------------
 *  The screen is split horizontally into N equal-height strips where
 *  N == online CPU count obtained dynamically from sys_sysinfo(). One pinned
 *  worker thread owns one strip, so every frame is rendered with N-way data
 *  parallelism (disjoint Y ranges, zero write sharing -> no per-pixel lock).
 *
 *  Tearing fix (black horizontal bands):
 *  Workers NEVER paint the visible scanout directly. Every frame runs in two
 *  barrier-separated phases:
 *
 *    phase 1  COMPOSE : each worker renders its strip into an invisible
 *                       back buffer (clear + layer stack happens off-screen);
 *    barrier          : main thread waits until the WHOLE back frame is done;
 *    phase 2  PRESENT : each worker memcpy's its finished strip back->front.
 *
 *  Because the front buffer is only ever overwritten with rows from a
 *  complete frame, it never shows the transient "cleared to black" state, so
 *  no black band can flash even if a worker is preempted mid-compose.
 *
 *  Everything is DYNAMIC: layers are malloc'd nodes on a standalone linked
 *  list, the strip table and the back buffer are malloc'd at start. The
 *  application Window struct is left UNTOUCHED — each registered window is
 *  wrapped by a standalone CompWinNode.
 *
 *  Scene graph (two INDEPENDENT linked lists):
 *
 *    [Layer list]  bottom -> top  (dynamically allocated CompLayer nodes)
 *        +-- layer 0: [window-node list] -> Window* -> Window* -> ...
 *        +-- layer 1: [window-node list] -> Window* -> ...
 *
 *  Traversal per strip: walk the Layer list (bottom -> top), then each
 *  layer's window-node list. Every window is visited once per strip with an
 *  O(1) clip test and blitted with whole-scanline memcpy (never a per-pixel
 *  topmost search). Scene *traversal* is O(total window count); the only
 *  other cost is the unavoidable O(screen pixels) block copy.
 * ============================================================================
 */

#define COMP_BPP            4    /* 32-bit ARGB, uint32 / pixel              */
#define COMP_CPUS_SANITY  256    /* defensive cap only; storage is dynamic   */

/* ---------------------------------------------------------------------------
 *  Application window — kept exactly as originally defined, ZERO changes.
 * ------------------------------------------------------------------------- */
typedef struct Window {
    uint32_t SizeX, SizeY;
    uint32_t PosX, PosY;
    // Must in Window Size
    // Frame means (Title bar: Close/Maximize/Minimize)
    uint32_t FrameStartX, FrameStartY;
    uint32_t FrameEndX, FrameEndY;
    // Frame Buffer Base Address (window-local ARGB surface, pitch == SizeX)
    uint64_t FbAddr;
} Window;

/* ---------------------------------------------------------------------------
 *  In-layer window node — compositor-owned wrapper, does not modify Window.
 * ------------------------------------------------------------------------- */
typedef struct CompWinNode {
    Window            *win;         /* borrowed application window          */
    CompWinNode       *next, *prev; /* in-layer window list                  */
    struct CompLayer  *layer;       /* back pointer to owning layer          */
    uint8_t            visible;     /* 0 = skipped during compose            */
} CompWinNode;

/* ---------------------------------------------------------------------------
 *  Layer — a DYNAMICALLY ALLOCATED standalone linked-list node; each layer
 *  owns its own window-node list.
 * ------------------------------------------------------------------------- */
typedef struct CompLayer {
    CompLayer   *l_next, *l_prev;       /* standalone layer list (bottom->top) */
    CompWinNode *win_head, *win_tail;   /* this layer's window-node list       */
    uint32_t     window_count;
    uint32_t     z;                     /* stacking order, smaller = further   */
} CompLayer;

class Compositor {
public:
    static Compositor& Get();

    /* Bind the target scanout framebuffer and allocate the off-screen back
       buffer. Safe to call once before StartWorkers(). The fb is copied. */
    bool            Init(FrameBuffer* screen);

    /* Optional static backdrop (ARGB, screen-sized). Borrowed pointer. */
    void            SetBackground(const uint32_t* bg);

    /* Dynamically query ncpus via sys_sysinfo(), malloc the strip table,
       and launch one pinned worker per CPU (hint == cpu id). Blocks until
       every worker reports ready. */
    void            StartWorkers();

    /* Dynamic layer list management (sorted bottom -> top by z). */
    CompLayer*      CreateLayer(uint32_t z);
    bool            DestroyLayer(CompLayer* layer);

    /* Window registry: wraps `w` in a standalone CompWinNode, does NOT write
       into the Window struct. */
    bool            RegisterWindow(Window* w, CompLayer* layer);
    bool            UnregisterWindow(Window* w);
    void            SetVisible(Window* w, bool visible);
    void            MoveWindow(Window* w, uint32_t x, uint32_t y);

    /* Run one double-buffered parallel frame; returns after every strip has
       been presented to the scanout. Single-threaded fallback if workers
       are not running or the back buffer could not be allocated. */
    void            Compose();

    void            Shutdown();

    uint32_t             WorkerCount() const { return ncpus_; }
    const CompLayer*     LayerHead()  const { return layer_head_; }

    /* Bare C entry for sys_thread_launch (the kernel forces rdi = 0, so the
       strip id is handed out through an atomic ticket pool). */
    void            WorkerEntry(uint32_t id);

private:
    struct Strip {
        uint32_t y0, y1;           /* half-open row range [y0, y1) */
    };

    FrameBuffer     screen_;
    const uint32_t* background_;
    uint32_t*       back_;             /* off-screen compose target, scanout-sized */
    uint64_t        back_bytes_;       /* total byte size of back_ / scanout        */
    uint32_t        ncpus_;
    uint32_t        launched_;
    uint32_t        shutdown_;
    Strip*          strips_;             /* malloc'd [ncpus_], dynamic */

    /* standalone dynamic layer list */
    CompLayer*      layer_head_;
    CompLayer*      layer_tail_;
    int32_t         list_lock_;          /* registry mutation spinlock */

    /* ---- two-phase frame barrier (GCC __atomic builtins) ----
       compose phase: frame_seq_ publishes a frame, done_compose_ counts
                      workers that finished rendering into back_;
       present phase: present_seq_ releases workers to copy back_->front,
                      done_present_ counts workers that finished presenting.  */
    uint64_t        frame_seq_;
    uint64_t        present_seq_;
    uint32_t        started_cnt_;
    uint32_t        done_compose_;
    uint32_t        done_present_;

    CompWinNode*    FindNode(Window* w);
    void            ComposeStripToBack(uint32_t id); /* clear+stack -> back_ */
    void            PresentStrip(uint32_t id);       /* back_ -> scanout      */
    void            ComposeSingleThreaded();
    void            InsertLayerOrdered(CompLayer* layer);
    void            LockList();
    void            UnlockList();
};
