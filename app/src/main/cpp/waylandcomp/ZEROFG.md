# ZeroFG on the zero-copy path

`src/zerofg_zc.c` runs ZeroFG v1.2.1 (`../thirdparty/zerofg`, Apache-2.0) on the zero-copy main window: the frame
generation menu's "ZeroFG RZ+ 2×/3×/4×" and "ZeroFG Zero 2×/3×/4×" (`FrameGen.ENGINE_ZEROFG`, compositor engine 2;
the multiplier is `FrameGen.Mode.multiplier`, pref `zerofg_multiplier`, armed with `nativeSetFrameGenArmed`). RZ+ is
ZeroFG 1.2's one engine, `Mode::kReallyZero` (the 1.1 ReallyZero's dense flow with the old Zero's camera start,
HUD hold and wipe handling; 1.2.1 adds the Vector Trail Fill at occlusion edges); it is the default. Zero is
`Mode::kZero`, 1.1's layered engine, kept upstream as it was (`CreateInfo::zero_options` 0). Upstream turns one
optimisation of Zero's render on only where the device name says Adreno 800 or up (2c6a44bf), so on Mali that
pass runs in its general form. Selected, it does not pause zero-copy (`vkp_framegen_active()` is 0 for it), so hybris games keep presenting
zero-copy. The copy path runs no frame generation while it is selected. Off, nothing on the zero-copy path or in
LSFG / DD-FG changes.

## Per real frame B of the main window

1. Imports B (the game's AHardwareBuffer) into ZeroFG's own VkDevice.
2. A is the real frame shown before B. With an engine ready, a free job, free frame contexts and free pool
   buffers, ZeroFG records the pair's N - 1 generated frames S1 .. S(N-1) for the multiplier N in force (below):
   `Interpolate(A, B, k / N)` for k = 1 .. N - 1, one after another in the job's command buffer, each with the
   next of the engine's 8 frame contexts (`frame_context_count`, at least N + 1 as INTEGRATION.md asks for xN),
   each into a buffer of the layer's pool (RGBA8, written through ZeroFG's internal A2B10G10R10 image and a
   blit). The calls after the first record only what depends on the phase (same pair, same sequence numbers). A
   pair is submitted only while the Interpolate calls of the jobs still running and its own fit the 8 contexts
   (x4: two pairs in flight). Only the generated frames the pair shows are recorded (below: the first ones may be
   left out). With fewer free pool buffers than that, the first of them are left out too (`pool_short`); the
   others keep their targets. The pool is sized for the multiplier asked for: 3 (x2, as before), 6 (x3) or 9
   (x4) buffers, allocated as needed; a smaller size (the menu, or ZeroFG turned off, which gives the upscaler
   back its 3) frees the buffers beyond it once they are neither shown nor in use and their release fence has
   signalled, and ZeroFG's imports of them go once no job writes them (`pool_size`); the rest go when ZeroFG
   stops using the layer. The job waits for B's acquire fence, for A's when no
   earlier job waited for it, and for the pool buffers' display release fences. Timestamp queries before the
   job's calls, after its first and after its last give the GPU time of the first call (the pair recorded
   whole) and of each further one (only the picture at another phase).
3. Each S goes to the display in a transaction of its own, then B (the game's buffer itself) in another. All
   wait on the event loop for their times (below), the generated frames also until the generation is complete.
   The generation waits for B to be rendered, so it can be complete only after the next real frames arrived:
   their pairs queue behind (up to 4 pairs).
4. Each output is placed when it goes to the display, by the window's rectangle through the scene-to-output
   mapping in force then, as B always was (`sc_layer_present_vortek`). A game window crop (game fill,
   `game_fill.h`) that changes while pairs wait so moves their generated frames with their B; a generated frame
   never shows the place the screen has left.

Retention of A is "hold" (design option b): the image on the screen is the next pair's A, so nothing is
copied. The pairs waiting for their time hold their B, so a game image stays out of the game's hands from its
present until the next pair's S1 replaces it on the screen: about B's target (`b_target_ms_avg`) plus a period
/ N. A game with few swapchain images (4 on the tablet) can then wait for a free one. A game image's release
fence is merged with the end of the last generation that read it (`zfg_release_fence`, called from
`vortek_present_release`).

ZeroFG is off while a zero-copy scaling filter (SGSR, SGSR HQ, Sharpen, FSR with `DROIDDECK_ZC_FSR=1`) is
selected; the log says so once. When it takes a layer's frames again, an upscaling pass still waiting there is
dropped. Frames that are not 8-bit RGBA (HDR), and sizes outside 64x64 .. 16384x16384, are shown as they are.
B8G8R8A8 swapchains (AHardwareBuffer format 5, HAL_PIXEL_FORMAT_BGRA_8888: DXVK's B8G8R8A8 swapchains through the
hybris WSI, the Steam client's windows through the Vortek server) are imported as VK_FORMAT_B8G8R8A8_UNORM where
the driver gives their buffers no Vulkan format of its own (`vkGetAndroidHardwareBufferPropertiesANDROID`:
VK_FORMAT_UNDEFINED), as their presenters create their images on these buffers (`zfg_import_format`).
A session whose games are not shown zero-copy (another `BL_GPU_MODE`, the upstream GPU path) gets no generated
frames: the menu items are greyed out with that reason, and picking ZeroFG says so.

## Pacing

- Period P: trimmed mean of the last 8 real-frame intervals. An interval over max(4 P, 250 ms) is a cut.
- Refresh r: the panel's running rate (`FrameGen.apply`). Latch W: how long before a refresh SurfaceFlinger
  takes its buffers, one refresh unless `DROIDDECK_ZEROFG_LATCH_MS` says otherwise.
- Lead L: how long after B arrives S is ready (seen as its completion fence signalling on the event loop);
  rises at once, falls by 1/8 per frame; plus a margin (2 ms), at most 2 P; plus W and r/2.
- The outputs of a pair are N apart by Q = max(P / N, r): S1's target is P / N after the previous B's target,
  kept within [arrival + L, arrival + L + max(P/4, r)] and at least the spacing after the previous B: one
  refresh at x2 (as before x3 and x4), max(0.8 P / N, r) at x3 and x4 (the rule SkyNetEmu 0.2.4 uses: no two
  outputs closer than 80 % of their mean interval); S2 .. S(N-1) and B follow Q apart (`zfg_schedule_n`).
  When S1 would have to pass the upper bound to keep that spacing (the real frame came early against the
  rhythm; at x4 on 120 Hz and x3 on 90 Hz, one output a refresh, any early frame at the top of the window),
  x3 and x4 leave out as few of the first generated frames as they can (`early_ph`; only where N times the real
  rate is at most 5 % above the refresh rate): the next one goes at least the spacing after the previous B and
  at least m Q after arrival + L (m left out), up to m Q after the upper bound, the others and B Q apart, so B
  comes m Q earlier than the rhythm would have it. Where none fits (and always at x2), the pair has no
  generated frames (`early`) and B goes (N - 1) P / N after arrival + L, at least the spacing after the previous
  B, but never later than B of a pair with generated frames could be: where N times the real rate is above the
  refresh rate, generated frames are left out, never a real one; real frames faster than the refresh rate do
  not push the targets later and later. Every target is given to
  `ASurfaceTransaction_setDesiredPresentTime` half a refresh early (SurfaceFlinger shows a transaction on the
  first refresh whose expected present time is later), so every refresh between two real frames has an output
  aimed at it, as in upstream #2743.
- The multiplier in force N is the menu's (or `DROIDDECK_ZEROFG_MULT`), lowered one step at a time, never below
  2 (`zfg_mult_update`): at once while the engine records the middle phase only (its phase forms failed,
  below); at once while N times the real rate is more than 5 % above the peak rate (3 % to go back up); at once
  while the GPU time of a pair is more than half the real-frame period (40 % to go back up; a multiplier lowered
  for this is not tried again for 10 s): F + (N - 2) p, F the median GPU time of the pairs' first call and p of
  each further call over the last 16 pairs (p is kept while x2 runs, and counts as 0 until a pair above x2 has
  measured it, so a multiplier asked for from the menu is tried and judged on its own pairs); at once while the
  pairs in flight would not fit: with the median time from a real frame's arrival to its generation's completion
  D (last 16 pairs), floor(D / P) + 1 pairs are in flight, and their (N - 1) calls each must fit the 8 frame
  contexts and the 3 jobs (x4 up to D < 2 P, x3 up to 3 P; 15 % more than D to go back up), else every pair after
  two still running would be skipped as `late`; and after N times the real rate stayed more than 5 % above the
  running refresh rate for 2 s (3 % to go back up). A higher multiplier comes back once it has fitted for 2 s.
  Every change is logged with its reason. x2's own limits are `fast` below.
- SurfaceFlinger takes the transactions of a process in order and holds every later one behind one whose
  desired present time has not come, or whose buffer is not ready. So S and B are not handed over at once:
  S goes when it is complete and W + margin before its desired present time, B the same before its own, after
  S (a CLOCK_MONOTONIC timerfd); with N > 2 each Sk the same after S(k-1). The pairs of later real frames wait
  behind in order; they are not hurried by the next frame, so when SurfaceFlinger commits earlier than W +
  margin, the outputs slip by a refresh together instead of one being taken with the next. An Sk not complete
  when only W + margin before the refresh ahead of the next output's is left (`missed`) stays off the screen, as
  SurfaceFlinger would take it with that one; its buffer goes back with the generation's fence, and the next
  output goes on time. A frame of the layer shown at once (the skips that restart the
  rhythm) lets the waiting pairs go first, S only if complete; a fifth pair pushes out the oldest; hiding the
  layer, its swapchain or the window going drops them (their B goes back to the game without waiting).
- Without a desired present time (`DROIDDECK_ZEROFG_DPT=0`, or no `setDesiredPresentTime`) the outputs are
  handed over at the same times, so each still has a refresh of its own.
- When the generated frames are skipped for `early`, `late` or `busy`, B keeps its place in the rhythm. Other
  skips show B at once and restart the rhythm.
- `fast` (B shown as it comes, no S): 2x the real rate more than 5 % above the peak rate (below), until it is
  3 % or less above; or the real rate itself more than 2 % above the running refresh rate, until it is no
  longer above it; or 2x the real rate more than 5 % above the running refresh rate (until 3 % or less) for
  2 s, the panel having stayed below the peak after the vote (a power or thermal cap, the system's refresh
  policy, a peak the panel never reaches).
- The layer votes the lowest of the panel's rates (modes of its current size up to the peak, from
  `SessionActivity`; none known: the peak over 1 .. 8) that is a whole multiple of N times the real rate within
  3 %, so every output keeps the same number of refreshes (30 fps: x2 60 Hz, x3 90 Hz, x4 120 Hz); where none is,
  the highest rate up to the peak (the smallest unevenness). N here is the multiplier in force, or a higher one
  that only the running rate holds down, so the panel can come up for it. The rate is judged on P smoothed
  further (each frame moves it an eighth of the way). The vote stays while N times the real rate is at most 5 %
  above or 8 % below a whole fraction of it and no lower rate fits within 3 %; it changes once another rate has been chosen for 1 s without a break, so a few uneven frames do not move it
  (`zfg_vote_step_n`). It asks for seamless mode switches only. The peak rate is the requested mode, else the
  fastest mode of the panel's size, after the caps (`FrameGen.apply`). While ZeroFG is selected the session's own
  vote is that rate too, so the panel comes back up after ZeroFG's vote lowered it, and for 10 s after ZeroFG was
  turned off while the panel still runs below it; then the running rate, as without ZeroFG. Frames skipped as
  `fast`, `unsupported` or `error`, and a window that stops being the main one, drop the layer's vote.

The pure arithmetic is `src/zerofg_pace.c`, tested by `tests/zerofg_pace_test.c` (CI: "ZeroFG pacing test"),
which also drives it as `zfg_present` does against a model of SurfaceFlinger's queue (one apply token, a
commit W before each refresh, one buffer per layer and refresh), with the pairs' output times
(`zfg_pair_times`) and what becomes of each generated frame (`zfg_s_action`) from the same functions
`pend_push` and `pend_run` use, also where S is complete only 40-64 ms after B arrived (the tablet at its native
size); x2 placed as before x3 and x4 for any arrival; x3 and x4 at 30 fps on 90, 120 and 144 Hz, with arrivals
up to 5 ms uneven, with B rendered 50 and 70 ms after it arrived, the multiplier lowered for the peak, the GPU
time (and back after a spike), the pairs in flight, the engine, and a panel held below the peak; and a game fill
crop and option change (`game_fill.c`, `scene_map.c`) while pairs wait. Where a generated frame is placed
(`pend_apply_s`: the mapping in force when it goes) is modelled there, not called.

## Device and priority

ZeroFG 1.2.1 makes the pipelines of the phases off the middle (x3, x4) on the first record away from 0.5 (one for
RZ+, eight for Zero). So that the event loop does not wait for those compiles, the worker makes them: when the
engine is built for x3 or x4, or when x3 or x4 is asked for of an engine built for x2 (frames are shown as they
are meanwhile, `skip_prep`, once the jobs that used the engine are done), with one generation of a black frame
at phase 1/3 into an image of its own (two images of the engine's size for a moment), submitted and waited for;
its sequence numbers are never a game frame's, so the first real pair starts afresh. Where that fails, or a
later x3 or x4 record does, the engine records x2 only (`mult_why=engine`) and does not try again.

ZeroFG gets a VkDevice of its own on the compositor's GPU and queue family (one queue, Vulkan 1.3,
synchronization2, shaderStorageImageExtendedFormats, shaderFloat16 where offered), made the first time it
runs, so the compositor's device and the queue vku uses are unchanged. That queue asks for global priority
HIGH (`VK_KHR_global_priority`, else `VK_EXT_global_priority`); `VK_ERROR_NOT_PERMITTED` falls back to the
default priority. Device and engine are built on a worker thread; frames are shown as they are meanwhile
(`skip_prep`). After 10 s without frames the engine and imports are freed, also an engine built for nothing (the
menu changed while it was built). Nothing is freed while a generation that used it still runs (INTEGRATION.md
section 7), and the event loop never waits for one: freeing waits for the next stats tick or frame. The one
exception: a generation whose completion cannot be exported as a sync_fd is waited for, at most 250 ms, before
its frames go on (nothing else could tell the game when it stopped reading them); ZeroFG then turns off for the
session.

A size and mode that cannot be built is shown as it is and not tried again in the session; other sizes are. A
device-level failure (device creation, device lost, a sync_fd import or submission failing) turns ZeroFG off
for the session; its device, engine and imports are freed once nothing runs on them, and the next session
(`CompositorHost.newSession`) tries again. The reason shows on the first ZeroFG menu item and when ZeroFG is
picked.

## Knobs (`droiddeck-env`, read when ZeroFG turns on)

| Variable | Effect |
| --- | --- |
| `DROIDDECK_ZEROFG_MODE=zero\|reallyzero\|rz+` | this engine whatever the menu says (reallyzero and rz+: RZ+) |
| `DROIDDECK_ZEROFG_MULT=2\|3\|4` | this multiplier whatever the menu says (still lowered where it does not fit) |
| `DROIDDECK_ZEROFG_PRIORITY=0\|high\|realtime` | global priority asked for (default high; 0: none) |
| `DROIDDECK_ZEROFG_DPT=0` | no desired present time: S and B are handed over at their times all the same |
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
- `on: ZeroFG 1.2.1 rz+|zero xN WxH (...), backend auto|compat, built in N ms; ...; desired present time on|off,
  ...` once the engine is ready.
- `x3, x4: the engine's phase forms made in N ms on the worker`, or `x3, x4 unavailable with this engine: its
  phase forms could not be made (<why>); x2 only`, or later `an xN generation could not be recorded (<status>):
  x2 only with this engine`.
- `multiplier xA -> xB (xN asked for): <why>` when the multiplier in force changes: `x4 of the real rate R fps is
  F fps, above the peak refresh rate P Hz`, `x4 needs about T ms of GPU time per real frame (F ms for the first
  generated frame, p ms for each further one), more than 50 % of the real-frame period`, `generations are done D
  ms after their real frame, so K pairs are in flight ...`, `this engine records the middle phase only`, `...
  above the running refresh rate H Hz for 2 s`, or `it fits again (...)`; `multiplier xN asked for (was xM)`
  when the menu changes it.
- `[layer] <layer>: pool of N buffers (was M)` when a pool's size changes (the multiplier asked for, ZeroFG
  turned off).
- Every 5 s while active, one line of `key=value`:
  `stats 5.0s real_fps= gen= gen_fps= skip= skip_first= skip_cut= skip_fast= skip_early= skip_late= skip_busy=
  skip_missed= skip_unsupported= skip_error= skip_prep= gpu_ms_med= gpu_ms_avg= gpu_ms_max= gpu_n=
  ready_ms_avg= ready_ms_max= s_after_target= b_rendered_ms_avg= b_rendered_ms_max= s_after_b_ms_avg=
  b_target_ms_avg= queue_max= dpt= period_ms= lead_ms= refresh_hz= vote_hz= fast= mult= mult_want= mult_vote=
  mult_lo= mult_hi= mult_why= pairs_x2= pairs_x3= pairs_x4= gen_ph= missed_ph= early_ph= pool_short=
  gpu_ms_first= gpu_ms_phase= rec_ms_max= mode= size=`
  - `gen`: generated frames handed to the display. `mult`: the multiplier in force at the end of the interval,
    `mult_want` the one asked for, `mult_vote` the one the vote is for, `mult_lo`/`mult_hi` the lowest and highest
    in force during it, `mult_why` what holds it below the one asked for (`none`, `peak`, `gpu`, `refresh`, `late`,
    `engine`). `pairs_xN`: pairs generated at xN. `gen_ph=a/b/c`: generated frames shown at the first, second and
    third phase (x2: a only; x3: a, b; x4: a, b, c), `missed_ph` those left out as `missed`, `early_ph` those the
    rhythm left no room for (`early`: whole pairs, and the first frames of pairs that keep later ones).
    `pool_short`: pairs with frames left out for want of pool buffers. `gpu_ms_first`, `gpu_ms_phase`: the median
    GPU time of a pair's first call and of each further one that the multiplier is judged on (0 before 8 pairs, and
    before 4 pairs above x2). `rec_ms_max`: longest CPU time recording a pair. The line fits droiddeck_log's 1024
    bytes (`tools/tests/test_zerofg.py`). `gpu_ms_*`: GPU time of a pair's `Interpolate` calls from timestamp
    queries around them. `ready_ms_*`: from B's arrival to S's completion as the event loop saw it.
    `s_after_target`: S completed later than W before the refresh its desired present time picks. `b_rendered_ms_*`:
    from B's arrival to the signal time of B's acquire fence (the game's GPU work for B; 0 when it had signalled),
    `s_after_b_ms_avg`: from then to the signal time of S's completion (the generation plus the time it waited on
    the GPU), both from the sync_files' timestamps, for generated pairs. `b_target_ms_avg`: from B's arrival to its
    target on the screen. `queue_max`: most pairs waiting at once. `dpt=1`: desired present time used. `fast=1`:
    frames shown as they come right now.
  - Skips: `first` (no previous frame: start, new swapchain or size, after a hidden window), `cut` (frames
    more than max(4 P, 250 ms) apart), `fast` (above), `early` (no refresh of its own left for any generated
    frame), `late` (all three jobs still running, or their Interpolate calls and the pair's would not fit the 8
    frame contexts), `busy` (the display holds every pool buffer), `missed` (a generated frame not complete while a
    refresh before the next output's was left; counted per frame), `unsupported`, `error`, `prep` (engine being
    built, or its phase forms being made).
- `off: <why> (N frames generated)`; `off for this session: <why>`; `ZeroFG (<mode>) could not be built at WxH:
  <status>; frames of this size and mode are shown as they are`; `new session: ZeroFG may run again`;
  `swapchain N of presenter P (WxH, AHardwareBuffer format F) cannot be used: <why>; its frames are shown as
  they are`, once per swapchain (N and P as in the `vortek` lines `zero-copy swapchain N of presenter P`);
  `AHardwareBuffer format 0x5 has no Vulkan format of its own on this driver: imported as B8G8R8A8_UNORM`, once.

Not counted: a frame SurfaceFlinger drops for a later one on the same refresh, and ZeroFG's own scene-cut
handling (it then outputs B whole). `adb shell dumpsys SurfaceFlinger --latency <layer name>` lists actual
present times of a layer (`droiddeck_vortek_game` and the other Vortek layers).
