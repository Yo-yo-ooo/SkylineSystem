# GUI stack and the compositor

Source: `programs/desktop/` (`main.cpp`, `loader.cpp`, `synthesizer/window.cpp`), `lib/graphic/`,
`kernel/modules/common/drivers/framebuffer/`.

The whole desktop runs in userspace. The kernel hands over a framebuffer, shared memory and sysinfo,
nothing more. What follows is what the code does, defects included.

## 1. Rendering model (what the code actually does)

`Compositor::StartWorkers()` splits the screen into **up to 3 horizontal strips**: the main thread
renders strip 0, and one worker thread per stripe (pinned to another CPU) renders the rest. The
worker count is capped at 2 on purpose: the 2026-10 attempt with 3 workers starved DOOM on 4 vCPUs
(`R_InitData +18.2s` vs `+8.6s`), but that version used a **spin/yield wait**. The current workers
still cannot `Sleep` (see below) but two of them leave at least one core for clients, and the
measured compose time is 10.1 ms median vs 26.0 ms single-threaded on the same workload (start-menu
toggle, 4 vCPU QEMU) — a 2.6x improvement.

- `Schedule::Sleep()` now works on every CPU: `Switch()` advances that CPU's timer wheel from its
  own SCHED_VEC (`TimerTickWheels()` in `timer.cpp`) and idle re-arms a 1 ms tick when a sleeper is
  pending. Before this fix the wheel only advanced from the PIT IRQ on the BSP, so a thread that was
  stolen to an AP slept forever — the observed symptom was the pointer freezing while clicks still
  worked. Workers still wait with spin → `sys_yield` (`comp_backoff`); switching them to real 1 ms
  sleeps is the next step (it should remove the idle CPU cost without changing frame latency).
- There is no off-screen back buffer. `back_ = screen_.BaseAddress`; composing writes the scanout
  buffer directly. This saves 4 MiB (needed for the 64 MiB target) at the cost of visible partial
  updates instead of an atomic frame.
- Each worker owns one staging row so a strip is composed row-by-row instead of clearing the screen
  first (the clear→compose flash fix).
- The barrier is per-strip and generation-based: `frame_seq_` is published, each worker stores
  `worker_done_seq_[id] = seq`, and the main thread waits for every strip to reach `seq`. A late ack
  from frame N−1 can no longer satisfy frame N. `worker_busy_[id]` tells the timeout path which
  strips it may re-render itself; it never re-renders a strip a worker is currently writing, and it
  waits (bounded) for in-flight workers to leave before the desktop mutates layer lists again.
- `StartWorkers()` checks the launch result with a signed cast: `sys_thread_launch` returns
  `uint64_t`, so the old `>= 0` test was always true and a failed worker launch was counted as
  success (the barrier then waited for a thread that did not exist).
- The desktop binary lives in `disk.img:/desktop.elf`, not in the ISO: after changing
  `programs/desktop`, run `make -C programs` (or `make cm`) so the image gets the new ELF. A stale
  `desktop.elf` from an experimental sleep-wait build is what made the pointer appear frozen while
  clicks still worked.

## 2. Traversal cost

- Two levels of dynamic lists: layers, and windows within a layer.
- Each window is clip-tested once per strip; on a hit it blits whole scanlines.
- Cost per frame is O(strips × windows), not O(window count) as an older doc said.

## 3. Visual effects

- SDF anti-aliased corners, 8px.
- Directional soft shadows with quadratic falloff.
- Per-pixel ARGB source-over, degrading to `memcpy` across runs of opaque pixels.
- Win11/Fluent look: flat dark title bars, 1px light outline.
- No dirty-rectangle compare: the compare-and-blit path was removed together with the back buffer.
  Every compose pass redraws its range; the old "zero scanout writes when static" claim no longer
  holds.

## 4. Cursor

- The cursor is not a separate layer and does not write straight to scanout outside the composer.
  A move recomposes the two 16-row bands the cursor covers, restoring the pixels underneath from the
  scene row (see `ComposeRangeToBack` step 3).
- A dedicated cursor thread samples the pointer every ~2 ms and redraws only those two bands; the
  main loop publishes the clamped position into a normal global (`g_mouse_pos`, one 64-bit atomic
  store) instead of letting the thread read the kernel-mapped mouse page. With two threads in the
  same process we measured one thread reading all-zero mouse state at the same VA the other thread
  saw updating (stale cross-CPU translation); the shared global avoids that entirely.
- This keeps mouse movement from triggering a full-frame recompose, at the cost of two band redraws
  per move.
- The old "paint the new square before restoring the old one" defect is gone with this approach.

## 5. Text

- `lib/base/font/ttf.c` builds on stb_truetype (third-party), with an LRU + hash glyph cache, CJK
  layout and boundary-clipped alpha blending.
- Console output is drawn by flanterm, also third-party.

## 6. WM / application boundary

- Window decorations belong to the WM; applications only draw their client area.
- The console window has the full set: drag, maximize, minimize with taskbar restore, resize in 8
  directions, and a close button that reclaims the whole client process through `sys_kill`.
- Generic clients (Notepad, DOOM, Settings, Installer) go through the same WM path now: hit testing,
  caption drag, edge resize, maximize/minimize/close, click to front.
- Child exit is event-driven. The kernel pushes `{pid, exit_code}` into the parent's child-event ring
  in `FinalizeProcExit`/`DeleteProc`, and the desktop drains fixed syscall 31 `child_event` each loop
  and drops the window immediately. A 2s `sys_kill(pid,0)` sweep remains as overflow fallback only.
- z-order and focus: stacking order is the compositor layer `z`. Clicking a window, or restoring and
  raising one from the taskbar, brings it to the top and installs keyboard focus in the same frame.
  `wm_refocus()` falls back to the topmost visible window when the focused one is minimized or
  closed, and hidden windows are skipped by hit testing.
- Minimize keeps the maximize state: generic clients store `pre_minimized_max`, the console stores
  `wm_prior`, on both the caption button and the taskbar pill. Restore re-applies the maximized
  geometry and raises the window. Keyboard fallbacks: `Ctrl+Alt+C` focuses the console, `Ctrl+Alt+M`
  toggles maximize, `Ctrl+Alt+H` minimizes the focused window.
- The old claim that the console client is "pure standard C and compiles unchanged with any host
  toolchain" is wrong. It needs the in-tree libc: `-nostdlib`, the in-tree `_start`, and the protocol
  page pinned at 0x400000. It builds against this project's libc and nothing else.
- Client and compositor share one surface with no flip or fence, so the compositor can sample a frame
  the client is still drawing.
- `programs/desktop/loader.cpp` loads the ELF into a new process and attaches it to a window.
