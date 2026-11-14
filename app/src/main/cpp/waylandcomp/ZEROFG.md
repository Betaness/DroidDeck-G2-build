# ZeroFG on the zero-copy path

`src/zerofg_zc.c` runs ZeroFG v1.1.0 (`../thirdparty/zerofg`, Apache-2.0) on the zero-copy main window: the frame
generation menu's "ZeroFG ReallyZero 2×" and "ZeroFG Zero 2×" (`FrameGen.ENGINE_ZEROFG`, compositor engine 2).
Selected, it does not pause zero-copy (`vkp_framegen_active()` is 0 for it), so hybris games keep presenting
zero-copy. The copy path runs no frame generation while it is selected. Off, nothing on the zero-copy path or in
LSFG / DD-FG changes.

## Per real frame B of the main window

1. Imports B (the game's AHardwareBuffer) into ZeroFG's own VkDevice.
2. A is the real frame shown before B. With an engine ready, a free job and a free pool buffer, ZeroFG records
   `Interpolate(A, B, 0.5)` into a buffer of the layer's pool (RGBA8, written through ZeroFG's internal
   A2B10G10R10 image and a blit). The job waits for B's acquire fence, for A's when no earlier job waited for it,
   and for the pool buffer's display release fence.
3. S goes to the display in a transaction of its own, then B (the game's buffer itself) in another. Both wait on
   the event loop for their times (below), S also until its generation is complete.

Retention of A is "hold" (design option b): the image on the screen is the next pair's A, so the compositor
holds no game image beyond what zero-copy holds. A game image's release fence is merged with the end of the
last generation that read it (`zfg_release_fence`, called from `vortek_present_release`).

ZeroFG is off while a zero-copy scaling filter (SGSR, SGSR HQ, Sharpen, FSR with `DROIDDECK_ZC_FSR=1`) is
selected; the log says so once. When it takes a layer's frames again, an upscaling pass still waiting there is
dropped. Frames that are not 8-bit RGBA (HDR), and sizes outside 64x64 .. 16384x16384, are shown as they are.
A session whose games are not shown zero-copy (another `BL_GPU_MODE`, the upstream GPU path) gets no generated
frames: the menu items are greyed out with that reason, and picking ZeroFG says so.

## Pacing

- Period P: trimmed mean of the last 8 real-frame intervals. An interval over max(4 P, 250 ms) is a cut.
- Refresh r: the panel's running rate (`FrameGen.apply`). Latch W: how long before a refresh SurfaceFlinger
  takes its buffers, one refresh unless `DROIDDECK_ZEROFG_LATCH_MS` says otherwise.
- Lead L: how long after B arrives S is ready (seen as its completion fence signalling on the event loop);
  rises at once, falls by 1/8 per frame; plus a margin (2 ms), at most 2 P; plus W and r/2.
- S's target is half a period after the previous B's target, kept within [arrival + L, arrival + L +
  max(P/4, r)] and at least a refresh after the previous B; B's target is max(P/2, r) after S's. When S would
  have to pass the upper bound to keep that refresh, the pair has no S (`early`) and B goes half a period after
  arrival + L, at least a refresh after the previous B: where twice the real rate is above the refresh rate,
  S frames are left out, never a real one. Both targets are given to `ASurfaceTransaction_setDesiredPresentTime`
  half a refresh early (SurfaceFlinger shows a transaction on the first refresh whose expected present time is
  later).
- SurfaceFlinger takes the transactions of a process in order and holds every later one behind one whose
  desired present time has not come, or whose buffer is not ready. So S and B are not handed over at once:
  S goes when it is complete and W + margin before its desired present time, B the same before its own, after
  S. An S not complete by B's time is left out (`missed`) and its buffer goes back with the generation's fence;
  B goes on time. The next frame for the layer, hiding the layer, its swapchain or the window going settle a
  waiting pair first.
- When S is skipped for `early`, `late` or `busy`, B keeps its place in the rhythm. Other skips show B at once
  and restart the rhythm.
- 2x the real rate more than 5 % above the peak rate (below) is `fast` (until it is 3 % or less above).
- The layer votes the peak rate divided by the largest whole number that still leaves a refresh for every
  output (peak / k >= 2x the real rate), so SurfaceFlinger picks the peak or a mode that divides it; k stays
  while 2x the real rate is within 2 % above its rate, or until the next k fits with 5 % to spare. The vote
  changes at most once a second and asks for seamless mode switches only. The peak rate is the requested mode,
  else the fastest mode of the panel's size, after the caps (`FrameGen.apply`). While ZeroFG is selected the
  session's own vote is that rate too, so the panel comes back up after ZeroFG's vote lowered it. Frames
  skipped as `fast`, `unsupported` or `error`, and a window that stops being the main one, drop the layer's vote.

The pure arithmetic is `src/zerofg_pace.c`, tested by `tests/zerofg_pace_test.c` (CI: "ZeroFG pacing test"),
which also drives it as `zfg_present` does against a model of SurfaceFlinger's queue (one apply token, a
commit W before each refresh, one buffer per layer and refresh).

## Device and priority

ZeroFG gets a VkDevice of its own on the compositor's GPU and queue family (one queue, Vulkan 1.3,
synchronization2, shaderStorageImageExtendedFormats, shaderFloat16 where offered), made the first time it
runs, so the compositor's device and the queue vku uses are unchanged. That queue asks for global priority
HIGH (`VK_KHR_global_priority`, else `VK_EXT_global_priority`); `VK_ERROR_NOT_PERMITTED` falls back to the
default priority. Device and engine are built on a worker thread; frames are shown as they are meanwhile
(`skip_prep`). After 10 s without frames the engine and imports are freed, also an engine built for nothing (the
menu changed while it was built). Nothing is freed while a generation that used it still runs (INTEGRATION.md
section 7), and the event loop never waits for one: freeing waits for the next stats tick or frame.

A size and mode that cannot be built is shown as it is and not tried again in the session; other sizes are. A
device-level failure (device creation, device lost, a sync_fd import or submission failing) turns ZeroFG off
for the session; its device, engine and imports are freed once nothing runs on them, and the next session
(`CompositorHost.newSession`) tries again. The reason shows on the first ZeroFG menu item and when ZeroFG is
picked.

## Knobs (`droiddeck-env`, read when ZeroFG turns on)

| Variable | Effect |
| --- | --- |
| `DROIDDECK_ZEROFG_MODE=zero\|reallyzero` | this engine whatever the menu says |
| `DROIDDECK_ZEROFG_PRIORITY=0\|high\|realtime` | global priority asked for (default high; 0: none) |
| `DROIDDECK_ZEROFG_DPT=0` | no desired present time: S and B go to the display as soon as they are ready |
| `DROIDDECK_ZEROFG_VOTE=0` | the layer keeps the session's frame-rate vote |
| `DROIDDECK_ZEROFG_MARGIN_MS=<0..50>` | added to the measured lead, and to the latch when S and B are handed over (default 2) |
| `DROIDDECK_ZEROFG_LATCH_MS=<0..50>` | how long before a refresh SurfaceFlinger takes its buffers (default: one refresh) |
| `DROIDDECK_ZEROFG_BACKEND=compat` | ZeroFG's portable backend only (default: Auto, then Compat) |
| `DROIDDECK_ZEROFG_TRACE=1` | one line per real frame |

## Log (logcat tag `DroidDeckWayland`, also in the session log; lines start with `[zerofg]`)

- `available on the compositor's GPU ...` or `unavailable: <reason>` when the compositor's device comes up.
- `device: <GPU>, Vulkan 1.x, queue family N, subgroup N, fp16 on|off, GPU timestamps on|off; global priority
  HIGH accepted by vkCreateDevice (VK_KHR_global_priority)` | `HIGH refused (VK_ERROR_NOT_PERMITTED): default
  priority` | `not requested (...)` | `unavailable: ...`, once, when ZeroFG first runs.
- `on: <mode> WxH (...), backend auto|compat, built in N ms; ...; desired present time on|off, ...` once the
  engine is ready.
- Every 5 s while active, one line of `key=value`:
  `stats 5.0s real_fps= gen= gen_fps= skip= skip_first= skip_cut= skip_fast= skip_early= skip_late= skip_busy=
  skip_missed= skip_unsupported= skip_error= skip_prep= gpu_ms_med= gpu_ms_avg= gpu_ms_max= gpu_n=
  ready_ms_avg= ready_ms_max= s_after_target= dpt= period_ms= lead_ms= vote_hz= mode= size=`
  - `gen`: S frames handed to the display. `gpu_ms_*`: GPU time of `Interpolate` from timestamp queries
    around it. `ready_ms_*`: from B's arrival to S's completion. `s_after_target`: S completed later than W
    before the refresh its desired present time picks. `dpt=1`: desired present time used.
  - Skips: `first` (no previous frame: start, new swapchain or size, after a hidden window), `cut` (frames
    more than max(4 P, 250 ms) apart), `fast` (2× the real rate above the peak rate), `early` (no refresh of
    its own left for S), `late` (two generations still running, or no free job), `busy` (the display holds
    every pool buffer), `missed` (S generated but not complete by B's time), `unsupported`, `error`, `prep`
    (engine being built).
- `off: <why> (N frames generated)`; `off for this session: <why>`; `ZeroFG (<mode>) could not be built at WxH:
  <status>; frames of this size and mode are shown as they are`; `new session: ZeroFG may run again`.

Not counted: a frame SurfaceFlinger drops for a later one on the same refresh, and ZeroFG's own scene-cut
handling (it then outputs B whole). `adb shell dumpsys SurfaceFlinger --latency <layer name>` lists actual
present times of a layer (`droiddeck_vortek_game` and the other Vortek layers).
