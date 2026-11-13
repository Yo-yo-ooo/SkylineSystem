# GUI Stack and Parallel Compositor

Source: `programs/desktop/` (`main.cpp`, `loader.cpp`, `synthesizer/window.cpp`), `lib/graphic/`, `kernel/modules/builtin/common/drivers/framebuffer/`.

> The entire desktop runs in user space: the kernel only provides the framebuffer, shared memory, and sysinfo. This document describes the code's actual behavior, including known defects.

## 1. Data-Parallel Rendering (Actual Form)

```
online CPU count N  ← sys_sysinfo(0)

the screen is cut horizontally into N strips (disjoint Y ranges)

the main thread renders strip 0; each of the remaining N-1 strips starts one worker thread (sys_thread_launch)
```

- **Zero per-pixel locks**: each worker writes its own Y range; they never overlap;
- **Double buffering + single commit point**: workers write only the invisible back buffer; once the whole frame is done, **only the main thread** pushes it to scanout;
- **the barrier is a counting one** (`done_compose_` count + spin wait), **not** a generation barrier with sequence-number validation: a late worker incrementing for the previous frame may satisfy this frame's wait early — "absolutely no tearing" has a **residual race window that is not fully closed** (the two-phase `present_seq_/done_present_` commit claimed in the old docs does not exist);
- Fixed: the issue where the committer's second `rb_erase` caused a double delete in the red-black tree (search to confirm before removing).

## 2. Scene Traversal Complexity (Actual)

- Two levels of dynamic linked lists: the layer list + the per-layer window list;
- Each window takes one clip test in **each strip**; on a hit it blits whole scanlines;
- The per-frame cost is **O(strips × windows)**, not the O(window count) claimed in the old docs.

## 3. Visual Effects (All-Software Rendering)

- SDF anti-aliased rounded corners (8px);
- soft directional drop shadows (quadratic falloff);
- per-pixel ARGB source-over, degrading to `memcpy` over consecutive opaque spans;
- Win11/Fluent style: flat dark title bars, 1px glowing outlines;
- dirty-rectangle compare-and-blit: zero scanout writes except the cursor square when the scene is static (this mechanism really exists).

## 4. Independent Cursor Layer

- The cursor is an independent layer that writes directly to scanout, O(16²);
- Decoupled from scene composition: moving the mouse does not trigger recomposition;
- Known defect: the cursor redraw order paints the new square first and then restores the old square, so a displacement < 16px wipes part of the arrow.

## 5. Text

- `lib/base/font/ttf.c`: based on **stb_truetype** (third-party), LRU + hash-table glyph cache, CJK layout, boundary-clipped alpha blending;
- Console output is rendered by **flanterm** (third-party).

## 6. The WM / Application Boundary (Actual State)

- Window decorations are drawn by the WM; applications only handle the client area;
- The **console window** has full interaction: dragging, maximizing, minimizing (restore from the taskbar), 8-direction resizing, and the close button reclaims the entire client process via `sys_kill`;
- Generic clients (Notepad / DOOM / Settings / Installer) are now managed by the same WM path: hit testing, caption drag, edge resize, maximize/minimize/close, click-to-front;
- **Child-process exit is event-driven**: the kernel pushes `{pid, exit_code}` into the parent's child-event ring in `FinalizeProcExit`/`DeleteProc`; the desktop drains fixed syscall 31 `child_event` every loop and drops the window immediately (a 2s `sys_kill(pid,0)` sweep remains only as overflow fallback);
- **z-order / focus management**: stacking order is the compositor layer `z`; pressing any window or taskbar restore/raise brings it to the top and installs keyboard focus in the same frame; `wm_refocus()` falls back to the topmost visible window when the explicit focus target is minimized/closed; hidden windows are skipped by hit testing;
- **Minimize preserves maximize state**: generic clients record `pre_minimized_max`, console records `wm_prior` on every minimize path (caption button and taskbar pill); restore re-applies the maximized geometry and raises the window. Keyboard fallbacks: `Ctrl+Alt+C` focus console, `Ctrl+Alt+M` toggle maximize, `Ctrl+Alt+H` minimize focused window;
- "The console client is pure standard C and compiles unchanged with any host toolchain" — **inaccurate**: it depends on the in-tree libc (`-nostdlib`, the in-tree `_start`, the protocol page pinned at 0x400000) and can only be built against the libc that ships with this project;
- The client and the compositor share a single surface with no flip/fence: the compositor may sample a frame the client is still halfway through redrawing;
- `programs/desktop/loader.cpp` is responsible for loading the ELF into a new process and attaching it to a window.
