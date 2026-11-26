import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class GameTopMappingTest(unittest.TestCase):
    def test_letterbox_alignment_and_touch_inverse(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('A host C compiler is required')
        source = (REPO / 'app/src/main/cpp/waylandcomp/src/vk_present.c').read_text()
        header = (REPO / 'app/src/main/cpp/waylandcomp/src/vk_present.h').read_text()
        enums = header[header.index('enum vkp_scale_mode'):header.index('void vk_present_set_scale_mode')]
        state = source[source.index('static volatile int g_mode'):source.index('static int64_t g_swap_retry_at_ns')]
        def function(signature):
            start = source.index(signature)
            return source[start:source.index('\n}\n', start) + 3]
        funcs = '\n'.join(function(name) for name in ['static const char *mode_name(',
            'static const char *align_name(', 'static void update_map(', 'void vkp_set_scene_crop(',
            'int vkp_game_picture_rect(', 'int vkp_output_to_scene(', 'int vkp_scene_to_output('])
        scene_map = (REPO / 'app/src/main/cpp/waylandcomp/src/scene_map.c').read_text()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            code = '#include <assert.h>\n#include <math.h>\n#include <stdio.h>\n#include <string.h>\n#include <pthread.h>\n'
            code += '#include "scene_map.h"\n'
            code += '#define droiddeck_log(...) ((void)0)\nstatic struct { unsigned width, height; } g_extent;\n'
            code += 'static int cardboard;\nstatic int vkp_cardboard_active(void) { return cardboard; }\n'
            code += enums + state + funcs + scene_map
            code += r'''
static void near(double a, double b) { assert(fabs(a - b) < 0.01); }
static void check(int sw, int sh, int ow, int oh, int alignment) {
    g_extent.width = ow; g_extent.height = oh;
    g_mode = VKP_MODE_FIT; g_align = alignment;
    update_map(sw, sh);
    float rect[4];
    assert(vkp_game_picture_rect(rect, NULL, NULL));
    double top = alignment == VKP_ALIGN_PICTURE_TOP ? 0 : oh - sh * g_map.ky;
    near(rect[0], g_map.off_x); near(rect[1], top);
    near(rect[2], g_map.off_x + sw * g_map.kx);
    near(rect[3], top + sh * g_map.ky);
    near(g_map.off_y, top);
    double sx, sy;
    assert(vkp_output_to_scene(g_map.off_x + sw * g_map.kx / 2,
                              top + sh * g_map.ky / 2, &sx, &sy));
    near(sx, sw / 2.0); near(sy, sh / 2.0);
    near(g_map.kx, g_map.ky);
    assert(sh * g_map.ky <= oh + 0.01);
    g_align = VKP_ALIGN_CENTER; update_map(sw, sh);
    assert(!vkp_game_picture_rect(rect, NULL, NULL));
    near(g_map.off_y, (int)((oh - sh * g_map.ky) * 0.5));
}
/* Game window fill: Richman 11's 3408x1917 window on a 3408x2272 X screen and output. */
static void game_window(void) {
    g_extent.width = 3408; g_extent.height = 2272;
    g_mode = VKP_MODE_FIT; g_align = VKP_ALIGN_CENTER;
    update_map(3408, 2272);
    const int win[4] = {0, 0, 3408, 1917};
    float rect[4], shown[2];
    int cropped = 0;
    double sx, sy, ox, oy;
    vkp_set_scene_crop(1, win, 3408, 2272, VKP_MODE_STRETCH);
    assert(vkp_game_picture_rect(rect, shown, &cropped) && cropped);
    near(rect[0], 0); near(rect[1], 0); near(rect[2], 3408); near(rect[3], 2272);
    near(shown[0], 3408); near(shown[1], 1917);
    assert(vkp_output_to_scene(1704, 2272, &sx, &sy)); near(sx, 1704); near(sy, 1917);
    assert(vkp_scene_to_output(100, 1000, &ox, &oy) && vkp_output_to_scene(ox, oy, &sx, &sy));
    near(sx, 100); near(sy, 1000);
    vkp_set_scene_crop(1, win, 3408, 2272, VKP_MODE_FIT);
    assert(vkp_game_picture_rect(rect, shown, &cropped) && cropped);
    near(rect[1], 177); near(rect[3], 2094);
    assert(vkp_output_to_scene(1704, 177, &sx, &sy)); near(sy, 0);
    /* A half-size scene: the window's X px are scaled onto it. */
    update_map(1704, 1136);
    assert(vkp_game_picture_rect(rect, shown, &cropped) && cropped);
    near(shown[0], 1704); assert(fabs(shown[1] - 958.5) <= 0.5);
    cardboard = 1; update_map(3408, 2272);
    assert(!vkp_game_picture_rect(rect, NULL, NULL));
    cardboard = 0; update_map(3408, 2272);
    assert(vkp_game_picture_rect(rect, NULL, NULL));
    vkp_set_scene_crop(0, win, 0, 0, 0);
    assert(!vkp_game_picture_rect(rect, NULL, NULL));
    assert(vkp_output_to_scene(1704, 2272, &sx, &sy)); near(sy, 2272);
}
int main(void) {
    game_window();
    for (int alignment = VKP_ALIGN_PICTURE_TOP; alignment <= VKP_ALIGN_PICTURE_BOTTOM; alignment++) {
        check(1240, 1100, 2480, 2200, alignment);
        check(1280, 720, 2480, 2200, alignment);
        check(1024, 768, 2480, 2200, alignment);
        check(1920, 1080, 2400, 1080, alignment);
        check(1240, 1100, 2480, 1394, alignment);
    }
    g_align = VKP_ALIGN_TOP; update_map(1280, 720);
    assert(g_map.rh == 697 && g_map.ry == 0);
    puts("PASS");
}
'''
            (tmp / 'map.c').write_text(code)
            subprocess.run([cc, '-std=c11', '-I', str(REPO / 'app/src/main/cpp/waylandcomp/src'), str(tmp / 'map.c'), '-lm',
                            '-o', str(tmp / 'map')], check=True, capture_output=True, text=True)
            result = subprocess.run([str(tmp / 'map')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
