import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'app/src/main/cpp/waylandcomp/src'
SOURCE = SRC / 'present_cadence.c'
HARNESS = ROOT / 'tools/tests/present_cadence_harness.c'


@unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
class PresentCadenceTest(unittest.TestCase):
    def test_game_layer_frames_keep_their_cadence_through_copy_jitter(self):
        with tempfile.TemporaryDirectory(prefix='droiddeck-cadence-') as folder:
            binary = Path(folder) / 'cadence-test'
            build = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-I', str(SOURCE.parent),
                                    str(HARNESS), str(SOURCE), '-o', str(binary), '-lpthread'],
                                   capture_output=True, text=True)
            self.assertEqual(0, build.returncode, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual('PASS', result.stdout.strip())


def function_body(source, signature):
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]


class PresentCadenceWiringTest(unittest.TestCase):
    """[mali-g2] The Vortek zero-copy path: only the main game window's direct frames are planned, and while a
    frame is held no layer transaction is applied around the presenter's queue."""

    def test_every_layer_transaction_goes_through_the_presenter_queue(self):
        source = (SRC / 'sc_layer.c').read_text()
        allowed = ('pacer_apply_head_locked', 'filler_retire_locked', 'filler_ensure_locked', 'filler_locked',
                   'pacer_submit')
        bodies = ''.join(function_body(source, f'static {kind} {name}(')
                         for kind, name in (('void', 'pacer_apply_head_locked'), ('void', 'filler_retire_locked'),
                                            ('void', 'filler_ensure_locked'), ('void', 'filler_locked'),
                                            ('int', 'pacer_submit')))
        outside = source
        for name in allowed:
            for kind in ('void', 'int'):
                signature = f'static {kind} {name}('
                if signature in outside:
                    outside = outside.replace(function_body(outside, signature), '')
        calls = [m.start() for m in re.finditer(r'api\.txApply\(', outside)]
        self.assertEqual([], calls, 'api.txApply outside the presenter queue')
        self.assertGreaterEqual(bodies.count('api.txApply('), 5)

    def test_only_the_main_windows_direct_frames_are_paced(self):
        source = (SRC / 'vortek_present.c').read_text()
        on_present = function_body(source, 'static void on_present(')
        self.assertIn('const int paced = up == VKU_DIRECT && vf.main_window && sc_layer_present_cadence_on();', on_present)
        self.assertIn('g_batch_frame = 1;', on_present)
        batch_apply = function_body(source, 'static void batch_apply(')
        self.assertIn('sc_layer_tx_apply_frame(g_batch, g_batch_arrival_ns, g_batch_ready_fd)', batch_apply)
        self.assertIn('g_batch_frame = 0;', batch_apply)

    def test_pacing_off_applies_at_once_and_forgets_the_stream(self):
        source = (SRC / 'sc_layer.c').read_text()
        frame = function_body(source, 'void sc_layer_tx_apply_frame(')
        off = frame.index('if (!atomic_load(&g_cadence_on))')
        self.assertLess(off, frame.index('present_cadence_plan('))
        self.assertLess(frame.index('cadence_forget_if_off();'), off)
        self.assertIn('cadence_forget();', frame[off:frame.index('present_cadence_plan(')])
        setter = function_body(source, 'void sc_layer_set_present_cadence(')
        self.assertIn('atomic_store(&g_cadence_forget, 1);', setter)
        self.assertIn('pacer_flush_locked();', setter)
        self.assertIn('g_pacer_fill_until_ns = 0;', setter)
        self.assertIn('cadence_forget_if_off();', function_body(source, 'void sc_layer_cadence_log('))

    def test_frames_pacing_cannot_plan_are_counted_with_their_reason(self):
        source = (SRC / 'vortek_present.c').read_text()
        on_present = function_body(source, 'static void on_present(')
        self.assertIn('if (up != VKU_DIRECT && vf.main_window && sc_layer_present_cadence_on())', on_present)
        self.assertIn('sc_layer_cadence_note_unpaced(zfg != ZFG_DIRECT ? SC_CADENCE_ZEROFG : SC_CADENCE_FILTER);',
                      on_present)


ANDROID_HEADERS = {
    'rect.h': '''#pragma once
#include <stdint.h>
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
''',
    'native_window.h': '''#pragma once
typedef struct ANativeWindow ANativeWindow;
''',
    'hardware_buffer.h': '''#pragma once
#include <stdint.h>
#include "rect.h"
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct { uint32_t width,height,layers,format; uint64_t usage;
    uint32_t stride,rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM 0x2b
#define AHARDWAREBUFFER_USAGE_CPU_READ_RARELY 2ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY 32ULL
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE 256ULL
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT 512ULL
#define AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY 2048ULL
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *, AHardwareBuffer **);
void AHardwareBuffer_describe(const AHardwareBuffer *, AHardwareBuffer_Desc *);
int AHardwareBuffer_lock(AHardwareBuffer *, uint64_t, int32_t, const ARect *, void **);
int AHardwareBuffer_unlock(AHardwareBuffer *, int32_t *);
void AHardwareBuffer_release(AHardwareBuffer *);
''',
}


@unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
class PresentCadenceLayerTest(unittest.TestCase):
    """[mali-g2] sc_layer.c's pacing with SurfaceControl stubbed: toggling off forgets the stream on the
    compositor thread, the 10 s line gives the reason nothing was paced and SurfaceFlinger's latches."""

    def test_toggle_off_forgets_and_the_line_explains(self):
        with tempfile.TemporaryDirectory(prefix='droiddeck-cadence-layer-') as folder:
            work = Path(folder)
            (work / 'android').mkdir()
            for name, text in ANDROID_HEADERS.items():
                (work / 'android' / name).write_text(text)
            binary = work / 'cadence-layer'
            build = subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-parameter', '-Wno-unused-function',
                                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                    '-fsanitize=address,undefined', f'-I{work}',
                                    f'-I{ROOT}/app/src/main/cpp/waylandcomp/prebuilt/include',
                                    str(ROOT / 'tools/tests/sc_layer_cadence_harness.c'), '-ldl', '-pthread',
                                    '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(0, build.returncode, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertEqual('PASS', result.stdout.strip())

    def test_the_tick_carries_the_frame_timeline_only_while_pacing(self):
        host = (ROOT / 'app/src/main/java/com/droiddeck/launcher/wayland/CompositorHost.kt').read_text()
        tick = host[host.index('private fun postTick('):host.index('private fun vsyncCallback(')]
        self.assertIn('presentCadence && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU', tick)
        self.assertIn('postVsyncCallback(timelineCallback(generation))', tick)
        self.assertIn('postFrameCallback(vsyncCallback(generation))', tick)
        prefs = (ROOT / 'app/src/main/java/com/droiddeck/launcher/session/SessionPrefs.kt').read_text()
        self.assertIn('getBoolean("presentCadence", false)', prefs)

    def test_pacing_waits_while_the_session_is_off_the_built_in_screen(self):
        activity = (ROOT / 'app/src/main/java/com/droiddeck/launcher/SessionActivity.kt').read_text()
        refresh = activity[activity.index('private fun refreshActivityDisplayId('):
                           activity.index('private fun notifyActivityDisplayStateChanged(')]
        self.assertIn('applyPresentCadence()', refresh)
        apply = activity[activity.index('private fun applyPresentCadence('):activity.index('private var presentCadenceWaits')]
        self.assertIn('PresentCadenceGate.waitsFor(', apply)
        self.assertIn('sessionActivityDisplayId)', apply)
        self.assertIn('CompositorHost.setPresentCadence(presentCadence && waits == null)', apply)

    def test_the_games_cap_picks_the_mode_only_while_their_frames_hold_it(self):
        activity = (ROOT / 'app/src/main/java/com/droiddeck/launcher/SessionActivity.kt').read_text()
        pacing = activity[activity.index('private fun applyOutputPacing('):]
        pacing = pacing[:pacing.index('\n    }\n')]
        self.assertIn('val gameCap = if (gameCapHold.held) gameFpsCap else 0', pacing)
        self.assertIn('GameFpsCap.forMode(outputFpsLimit(false), gameCap)', pacing)
        self.assertNotIn('forMode(outputFpsLimit(false), gameFpsCap)', activity)
        self.assertIn('gameCapSampler.start()', activity)
        self.assertIn('gameCapSampler.stop()', activity)


if __name__ == '__main__':
    unittest.main()
