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
3. S goes to the display in a transaction of its own, then B (the game's buffer itself) in another.

Retention of A is "hold" (design option b): the image on the screen is the next pair's A, so the compositor
holds no game image beyond what zero-copy holds. A game image's release fence is merged with the end of the
last generation that read it (`zfg_release_fence`, called from `vortek_present_release`).

ZeroFG is off while a zero-copy scaling filter (SGSR, SGSR HQ, Sharpen, FSR with `DROIDDECK_ZC_FSR=1`) is
selected; the log says so once. Frames that are not 8-bit RGBA (HDR) are shown as they are.

## Pacing

- Period P: trimmed mean of the last 8 real-frame intervals. An interval over max(4 P, 250 ms) is a cut.
- Lead L: how long after B arrives S is ready (seen as its completion fence signalling on the event loop);
  rises at once, falls by 1/8 per frame; plus a margin (2 ms), at most 2 P.
- S's target is half a period after the previous B's target, kept within [arrival + L, arrival + L + P/4];
  B's target is S's + P/2. Both are given to `ASurfaceTransaction_setDesiredPresentTime` half a refresh
  early (SurfaceFlinger shows a transaction on the first refresh whose expected present time is later).
- When S is skipped for `late` or `busy`, B keeps its place in the rhythm. Other skips show B at once and
  restart the rhythm.
- The layer votes 2× the real rate, capped at the refresh rate (`FrameGen.apply` passes the panel rate),
  changed only by 2 Hz or more and at most once a second.

The pure arithmetic is `src/zerofg_pace.c`, tested by `tests/zerofg_pace_test.c` (CI: "ZeroFG pacing test").

## Device and priority

ZeroFG gets a VkDevice of its own on the compositor's GPU and queue family (one queue, Vulkan 1.3,
synchronization2, shaderStorageImageExtendedFormats, shaderFloat16 where offered), made the first time it
runs, so the compositor's device and the queue vku uses are unchanged. That queue asks for global priority
HIGH (`VK_KHR_global_priority`, else `VK_EXT_global_priority`); `VK_ERROR_NOT_PERMITTED` falls back to the
default priority. Device and engine are built on a worker thread; frames are shown as they are meanwhile
(`skip_prep`). After 10 s without frames the engine and imports are freed.

## Knobs (`droiddeck-env`, read when ZeroFG turns on)

| Variable | Effect |
| --- | --- |
| `DROIDDECK_ZEROFG_MODE=zero\|reallyzero` | this engine whatever the menu says |
| `DROIDDECK_ZEROFG_PRIORITY=0\|high\|realtime` | global priority asked for (default high; 0: none) |
| `DROIDDECK_ZEROFG_DPT=0` | no desired present time: S and B go to the display as soon as they are ready |
| `DROIDDECK_ZEROFG_VOTE=0` | the layer keeps the session's frame-rate vote |
| `DROIDDECK_ZEROFG_MARGIN_MS=<0..50>` | added to the measured lead (default 2) |
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
  `stats 5.0s real_fps= gen= gen_fps= skip= skip_first= skip_cut= skip_fast= skip_late= skip_busy=
  skip_unsupported= skip_error= skip_prep= gpu_ms_med= gpu_ms_avg= gpu_ms_max= gpu_n= ready_ms_avg=
  ready_ms_max= s_after_target= dpt= period_ms= lead_ms= vote_hz= mode= size=`
  - `gen`: S frames handed to the display. `gpu_ms_*`: GPU time of `Interpolate` from timestamp queries
    around it. `ready_ms_*`: from B's arrival to S's completion. `s_after_target`: S completed after its
    desired present time. `dpt=1`: desired present time used.
  - Skips: `first` (no previous frame: start, new swapchain or size, after a hidden window), `cut` (frames
    more than max(4 P, 250 ms) apart), `fast` (2× the real rate above the refresh rate), `late` (two
    generations still running, or no free job), `busy` (the display holds every pool buffer),
    `unsupported`, `error`, `prep` (engine being built).
- `off: <why> (N frames generated)`; `off for this session: <why>`.

Not counted: a frame SurfaceFlinger drops for a later one on the same refresh, and ZeroFG's own scene-cut
handling (it then outputs B whole). `adb shell dumpsys SurfaceFlinger --latency <layer name>` lists actual
present times of a layer (`droiddeck_vortek_game` and the other Vortek layers).
