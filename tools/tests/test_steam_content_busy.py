from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'linuxfs/overlay/usr/local/bin/droiddeck-session'

# content_log.txt, 2026-11-27: A.V.A. downloaded, then its install script ran.
DOWNLOAD = [
    '[2026-11-27 13:00:09] AppID 1473480 state changed : Update Required,Update Queued,Update Running,',
    '[2026-11-27 13:00:09] AppID 1473480 App update changed : Running Update,',
    '[2026-11-27 13:00:12] AppID 1473480 App update changed : Running Update,Downloading,Staging,',
    '[2026-11-27 13:02:24] Current download rate: 723.647 Mbps',
    '[2026-11-27 13:03:19] AppID 1473480 App update changed : Running Update,Running Script,',
]
DONE = [
    '[2026-11-27 13:03:25] AppID 1473480 App update changed : Running Update,',
    '[2026-11-27 13:03:25] AppID 1473480 App update changed : None',
    '[2026-11-27 13:03:25] AppID 1473480 state changed : Fully Installed,',
]


def function_source(name):
    source = SCRIPT.read_text()
    start = source.index('      %s() {' % name)
    end = source.index('\n      }\n', start)
    return source[start:end + len('\n      }')]


class SteamContentBusyTest(unittest.TestCase):
    def busy(self, lines, newline='\n'):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            if lines is not None:
                (root / 'logs').mkdir()
                (root / 'logs/content_log.txt').write_bytes(''.join(l + newline for l in lines).encode())
            script = 'steam_root=%s\n%s\nsteam_content_busy' % (tmp, function_source('steam_content_busy'))
            result = subprocess.run(['bash', '-c', script], capture_output=True, text=True, timeout=10)
        self.assertEqual('', result.stderr)
        return result.returncode == 0

    def test_a_download_and_its_install_script_are_busy_until_the_update_ends(self):
        self.assertTrue(self.busy(DOWNLOAD))
        self.assertTrue(self.busy(DOWNLOAD, newline='\r\n'))
        self.assertFalse(self.busy(DOWNLOAD + DONE))
        self.assertFalse(self.busy(DOWNLOAD + DONE, newline='\r\n'))

    def test_a_stopped_update_is_not_busy_and_an_uninstall_is(self):
        stopped = ['[2026-11-27 16:07:55] AppID 730 App update changed : Running Update,Downloading,Staging,Stopping,']
        self.assertFalse(self.busy(DOWNLOAD + DONE + stopped))
        uninstall = ['[2026-11-27 16:08:00] AppID 730 state changed : Update Required,Update Queued,Uninstalling,']
        self.assertTrue(self.busy(stopped + uninstall))
        self.assertFalse(self.busy(stopped + uninstall + ['[2026-11-27 16:08:00] AppID 730 state changed : Uninstalled,']))

    def test_shader_and_workshop_updates_count(self):
        self.assertTrue(self.busy(['[2026-11-27 13:26:59] AppID 1473480 Shader update changed : Running Update,Downloading,']))
        self.assertTrue(self.busy(['[2026-11-27 13:26:59] AppID 4000 Workshop update changed : Running Update,']))

    def test_a_client_restart_forgets_earlier_updates_and_no_log_is_idle(self):
        self.assertFalse(self.busy(DOWNLOAD + ['[2026-11-27 13:12:14] Client version: 1793740892']))
        self.assertTrue(self.busy(['[2026-11-27 13:12:14] Client version: 1793740892'] + DOWNLOAD))
        self.assertFalse(self.busy([]))
        self.assertFalse(self.busy(None))

    def test_the_compat_restart_waits_for_it(self):
        source = SCRIPT.read_text()
        start = source.index('      compat_settled() {')
        body = source[start:source.index('\n      }\n', start)]
        self.assertIn("! pgrep -f 'reaper SteamLaunch' >/dev/null 2>&1 && ! steam_content_busy", body)


if __name__ == '__main__':
    unittest.main()
