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

    void StartWorkers();
    void Compose();
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

    /* ---- worker entry (called from the C trampoline) ---- */
    void WorkerEntry(uint32_t id);

private:
    void LockList();
    void UnlockList();
    void InsertLayerOrdered(CompLayer* layer);
    CompWinNode* FindNode(Window* w);

    void ComposeStripToBack(uint32_t id);
    void ComposeRangeToBack(uint32_t ry0, uint32_t ry1);
    void ComposeSingleThreaded();
    void blitSceneAvoidCursor(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                              int32_t ox, int32_t oy, int32_t nx, int32_t ny);
    void paintSquareFromBack(int32_t x, int32_t y);
    void blendCursorSquare(int32_t x, int32_t y);
    void overlayCursorFinal(int32_t ox, int32_t oy, int32_t nx, int32_t ny);
    void commitScene(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int32_t ox, int32_t oy, int32_t nx, int32_t ny);

    FrameBuffer      screen_;
    const uint32_t*  background_;
    uint32_t         ncpus_;
    uint32_t         launched_;
    int              shutdown_;
    Strip*           strips_;
    CompLayer*       layer_head_;
    CompLayer*       layer_tail_;
    unsigned char    list_lock_;
    uint64_t         frame_seq_;
    uint32_t         started_cnt_;
    uint32_t         done_compose_;
    uint64_t         worker_done_seq_[COMP_CPUS_SANITY];
    int32_t          cur_x_, cur_y_;
    uint8_t          cur_visible_;
    int32_t          committed_x_, committed_y_;
    int32_t          dx0_, dy0_, dx1_, dy1_;
    uint8_t          dirty_;
    uint32_t         back_bytes_;
    uint32_t*        back_;
};

#endif /* SYNTHESIZER_WINDOW_H */