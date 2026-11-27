import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
COMPOSITOR = REPO / 'app/src/main/cpp/waylandcomp/src/compositor.c'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class CursorPublishTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get('CC', 'cc'))
        if compiler is None:
            raise unittest.SkipTest('A host C compiler is required')
        source = COMPOSITOR.read_text()
        state = [re.search(r'^static int g_cursor_hidden\b[^;]*;$', source, re.M),
                 re.search(r'^static int g_cursor_serial\b[^;]*;$', source, re.M)]
        if not all(state):
            raise AssertionError('compositor.c no longer declares g_cursor_hidden and g_cursor_serial as before')
        cls.temp = tempfile.TemporaryDirectory(prefix='droiddeck-cursor-publish-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        (folder / 'cursor_state.inc').write_text('\n'.join(m.group(0) for m in state) + '\n')
        (folder / 'cursor_publish.inc').write_text(''.join(function(source, signature) for signature in (
            'static void cursor_publish_shown(void) {',
            'static void cursor_publish_hidden(void) {',
            'static void cursor_publish_pixels(const uint8_t *src, int w, int h, size_t stride, int hx, int hy) {',
            'int droiddeck_cursor_serial(void) {',
            'int droiddeck_cursor_snapshot(int *out, int cap) {',
        )))
        cls.binary = folder / 'cursor-publish-test'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(folder),
                        str(REPO / 'tools/tests/cursor_publish_harness.c'), '-o', str(cls.binary)],
                       check=True, capture_output=True, text=True)

    def test_a_cursor_hidden_from_the_start_is_reported(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: cursor state publishing', result.stdout)


if __name__ == '__main__':
    unittest.main()
