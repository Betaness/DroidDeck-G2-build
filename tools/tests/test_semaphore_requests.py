import platform
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest

ESYNC = Path(__file__).resolve().parents[1] / "linuxfs/overlay/usr/local/bin/droiddeck-esync"


def server_source(names, decoy=False):
    table = ",\n".join('  "%s"' % name for name in names)
    other = 'const char *const other_names[] = { "new_process", "close_handle", "create_semaphore" };\n' if decoy else ""
    use = " + (other_names[argc % 3][0] == 'x')" if decoy else ""
    return """
#include <stdio.h>
%sconst char *const req_names[] = {
%s
};
int main(int argc, char **argv) { (void)argv; puts(req_names[argc %% %d]); return 0%s; }
""" % (other, table, len(names), use)


def request_names(create=40, close=21, count=150):
    names = ["new_process"] + ["request_%d" % n for n in range(1, count)]
    names[create] = "create_semaphore"
    names[close] = "close_handle"
    return names


def dynamic_tags(binary):
    out = subprocess.run(["readelf", "-d", str(binary)], capture_output=True, text=True).stdout
    return out


@unittest.skipUnless(platform.machine() in ("aarch64", "arm64", "x86_64") and shutil.which("gcc") and shutil.which("readelf"),
                     "needs a native aarch64 or x86_64 gcc")
class SemaphoreRequestsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.find = staticmethod(runpy.run_path(str(ESYNC))["semaphore_requests"])
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, name, names, flags=(), decoy=False, cc="gcc"):
        source = Path(self.tmp.name) / (name + ".c")
        binary = Path(self.tmp.name) / name
        source.write_text(server_source(names, decoy))
        subprocess.run([cc, "-O2", "-fPIE", "-pie", *flags, "-o", str(binary), str(source)], check=True, capture_output=True)
        return binary

    def test_reads_both_request_numbers_from_the_name_table(self):
        binary = self.build("wineserver", request_names())
        self.assertIn("RELA", dynamic_tags(binary))
        self.assertEqual(self.find(str(binary)), "40,21")

    def test_other_positions_are_read_as_they_are(self):
        self.assertEqual(self.find(str(self.build("moved", request_names(create=57, close=3)))), "57,3")

    def test_packed_relative_relocations_are_read(self):
        ways = [("gcc", ["-Wl,-z,pack-relative-relocs"])]
        if shutil.which("clang") and shutil.which("ld.lld"):
            ways.append(("clang", ["-fuse-ld=lld", "-Wl,--pack-dyn-relocs=relr"]))
        for cc, flags in ways:
            try:
                binary = self.build("relr-" + cc, request_names(create=12, close=99), flags, cc=cc)
            except subprocess.CalledProcessError:
                continue
            if "(RELR)" in dynamic_tags(binary):
                break
        else:
            self.skipTest("no linker here writes a RELR table")
        self.assertEqual(self.find(str(binary)), "12,99")

    def test_a_stripped_server_still_has_its_table(self):
        binary = self.build("stripped", request_names())
        subprocess.run(["strip", "--strip-all", str(binary)], check=True)
        self.assertEqual(self.find(str(binary)), "40,21")

    def test_a_short_list_that_also_starts_at_new_process_is_not_taken(self):
        self.assertEqual(self.find(str(self.build("decoy", request_names(create=7, close=8), decoy=True))), "7,8")

    def test_a_short_table_or_missing_request_gives_nothing(self):
        self.assertIsNone(self.find(str(self.build("short", request_names(count=60)))))
        names = request_names()
        names[21] = "request_21"
        self.assertIsNone(self.find(str(self.build("noclose", names))))
        names = request_names()
        names[77] = "create_semaphore"
        self.assertIsNone(self.find(str(self.build("twice", names))))

    def test_a_file_that_is_not_elf_gives_nothing(self):
        path = Path(self.tmp.name) / "script"
        path.write_text("#!/bin/sh\n")
        self.assertIsNone(self.find(str(path)))
        self.assertIsNone(self.find(str(Path(self.tmp.name) / "missing")))
        truncated = Path(self.tmp.name) / "truncated"
        truncated.write_bytes(self.build("whole", request_names()).read_bytes()[:4096])
        self.assertIsNone(self.find(str(truncated)))

    def test_a_launch_reads_the_numbers_from_the_proton_builds_own_wineserver(self):
        from test_esync_packs import SyncTestCase, make_tool, WINESERVER

        class Launch(SyncTestCase):
            def runTest(self):
                pass

        case = Launch()
        case.setUp()
        try:
            case.store()
            tool = make_tool(case.tools, "Proton Experimental (ARM64)")
            shutil.copyfile(self.build("tool-server", request_names(create=44, close=19)), tool / WINESERVER)
            command, env, said = case.select(tool, case.env())
        finally:
            case.doCleanups()
        self.assertEqual((env["BL_SYNC_SEMPOOL"], env["BL_SEMPOOL"]), ("preload", "44,19"))
        self.assertTrue(said.endswith("\ndroiddeck-esync: preload semaphore pool on (create_semaphore 44, close_handle 19)\n"), said)


if __name__ == "__main__":
    unittest.main()
