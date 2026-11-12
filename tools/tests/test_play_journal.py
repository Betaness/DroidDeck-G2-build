"""mali-g2/tools/play-journal.py: reading the play journal DroidDeck G2 writes (PlayJournal.kt)."""
import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "mali-g2" / "tools" / "play-journal.py"

# A record as the app writes it: PlayRecord.base/finish and JournalFile.line on PlayRecordTest's play.
PLAY = '{"v":1,"type":"play","id":"2026-10-28-07-steam#1","logs":"2026-10-28-07-steam","app":{"version":"0.6.0 (17)","commit":"e18add60c1a2","ci":true},"appid":3167020,"game":"Escape from Duckov","exe":"Escape from Duckov/Duckov.exe","launch":"steam","relaunchOf":null,"start":"2026-10-28T01:17:32+11:00","end":"2026-10-28T01:48:02+11:00","durS":1830,"focusedS":1745,"exit":{"reason":"crash","steamCode":null,"signal":null,"vortek":null,"appExit":null,"detail":"Player.log: err:vulkan:vkCreateDevice failed"},"flags":["crash","at60_thermal","marked"],"gpu":{"asked":"hybris","used":"hybris","renderer":"Mali-G2-Ultra-NX MC16 (ID=0x8001)","dxvk":null},"env":{"BL_SYNC":"pool","DROIDDECK_GPU_PATH":"hybris","LD_PRELOAD":"libhybris-preload.so,libfakeinput.so","PROTON_NO_NTSYNC":"1","VK_ICD_FILENAMES":"hybris_icd.aarch64.json"},"settings":{"mibridge":"mibridge: asking for cpu level 1, gpu level 1, ddr level 1 (see the MiBridgeBoost lines for the outcome)","joyose":"joyose: stopping it during the session","regulator":"CLEAR","joyoseWatch":"stopping","frameSchedOptOut":"frame reports to SchedGenius turned off","colocation":"colocation: automatic A/B","threads":"threads: watching (cluster steering on)","gpuPath":"hybris"},"display":{"render":"1704x1136","choice":"half","panel":"3408x2272","window":"fullscreen","wantedHz":120,"upscaler":0,"hdr":false,"fpsLimit":0,"swapchain":null,"present":"zero-copy"},"fps":{"source":"game-layer","mean":78,"meanAfter1m":78,"low1":45.4,"low5":68.9,"ftP50":12.63,"ftP90":12.73,"ftP99":12.75,"frames":136110,"layerFrames":136110,"composedFps":null,"countedS":1745,"stalls":2,"longestStallS":3,"freezeS":0,"freezeAtS":null,"perMin":[78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,78,null]},"hz":{"at60":0.32,"why":{"thermal":0.32,"app":0,"system":0,"unknown":0},"switches":0,"perMin":[120,120,120,120,120,120,120,120,120,120,120,120,120,120,120,120,120,120,120,120,110,60,60,60,60,60,60,60,60,60,null]},"thermal":{"statusMax":"MODERATE","headroomMax":0.6,"skinC":[34,46],"socCMax":70,"batteryC":[31,37],"skinPerMin":[34.4,34.8,35.2,35.6,36,36.4,36.8,37.2,37.6,38,38.4,38.8,39.2,39.6,40,40.4,40.8,41.2,41.6,42,42.4,42.8,43.2,43.6,44,44.4,44.8,45.2,45.6,46,46],"batteryPerMin":[31.2,31.4,31.6,31.8,32,32.2,32.4,32.6,32.8,33,33.2,33.4,33.6,33.8,34,34.2,34.4,34.6,34.8,35,35.2,35.4,35.6,35.8,36,36.2,36.4,36.6,36.8,37,37],"statusPerMin":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,2,2],"source":"shell"},"cpu":{"meanMHz":[2210,2860,3100],"capMinMHz":[3600,2400,4200],"steers":0},"mibridge":{"cpu":0.56,"gpu":1,"ddr":1},"mem":{"game":"Duckov.exe","gameHwmMB":2210,"gameRssMaxMB":1953,"gameAnonMaxMB":1650,"vortekAnonMaxMB":410,"vortekHwmMB":488,"appNativeMaxMB":120,"availMinMB":2900,"lowMemory":false,"anonPerMin":[1155,1156,1157,1158,1159,1160,1161,1162,1163,1164,1165,1166,1167,1168,1169,1170,1171,1172,1173,1174,1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185],"vortekAnonPerMin":null},"errors":[{"k":"crash","n":1,"atS":null,"game":true,"first":["Player.log: Crash!!!"]},{"k":"vulkan","n":1,"atS":null,"game":true,"first":["Player.log: err:vulkan:vkCreateDevice failed"]}],"marks":[{"atS":1210,"t":"2026-10-28T01:37:42+11:00"}],"timeline":[[600,"focus away (769)"],[615,"focus back"],[1210,"mark"]],"scanned":["Player.log"]}'


def load():
    spec = importlib.util.spec_from_file_location("play_journal", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class JournalTest(unittest.TestCase):
    def setUp(self):
        self.pj = load()
        self.dir = tempfile.TemporaryDirectory()
        play = json.loads(PLAY)
        other = json.loads(PLAY)
        other.update(id="2026-10-28-08-steam#1", logs="2026-10-28-08-steam", appid=1473480, game="A.V.A Global",
                     start="2020-01-02T02:10:00+11:00", end="2020-01-02T02:12:00+11:00", durS=120, flags=[],
                     exit={"reason": "normal", "steamCode": 0}, errors=[], marks=[])
        other["fps"] = {"source": "screen", "mean": 41.5, "perMin": [None, None]}
        other["gpu"] = {"asked": "hybris", "used": "vortek"}
        del other["thermal"]["skinC"]
        mark = {"v": 1, "type": "mark", "t": "2026-10-28T01:37:42+11:00", "logs": "2026-10-28-07-steam",
                "play": play["id"], "appid": 3167020, "game": "Escape from Duckov", "playS": 1210,
                "fpsLast60s": [78, 77, 12, 0, 0, 3, 40, 60, 78, 78, 77, 78], "modeHz": 60, "thermalStatus": "MODERATE", "skinC": 41.8}
        lines = [json.dumps(play), "not json at all", json.dumps(mark), json.dumps(other), '{"v":1,"type":"play","id":"cut-of']
        self.path = pathlib.Path(self.dir.name) / "plays.jsonl"
        self.path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        logs = pathlib.Path(self.dir.name) / "logs"
        (logs / "2026-10-28-07-steam").mkdir(parents=True)
        self.logs = str(logs)

    def tearDown(self):
        self.dir.cleanup()

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(0, self.pj.main(["--file", str(self.path), "--logs-dir", self.logs, *argv]))
        return out.getvalue(), err.getvalue()

    def test_bad_and_partial_lines_are_skipped_and_counted(self):
        records, skipped = self.pj.parse_lines(self.path.read_text())
        self.assertEqual(["play", "mark", "play"], [r["type"] for r in records])
        self.assertEqual(2, skipped)
        _, err = self.run_cli("list")
        self.assertIn("2 line(s) skipped", err)

    def test_list_has_a_row_per_play(self):
        out, _ = self.run_cli()
        lines = out.splitlines()
        self.assertTrue(lines[0].startswith("#  start"))
        duckov = next(l for l in lines if "Escape from Duckov" in l)
        for text in ("10-28 01:17", "30m", "crash", "hybris", "78/45", "32%(32)", "CRASH,60Hz-hot,MARKED", "2026-10-28-07-steam"):
            self.assertIn(text, duckov)
        ava = next(l for l in lines if "A.V.A Global" in l)
        self.assertIn("42s/", ava)  # composited frames when the game layer had none, marked "s"
        self.assertIn("normal", ava)
        self.assertIn("1 mark(s)", out)

    def test_filters(self):
        out, _ = self.run_cli("list", "--game", "duckov")
        self.assertIn("Escape from Duckov", out)
        self.assertNotIn("A.V.A", out)
        out, _ = self.run_cli("list", "-n", "1")
        self.assertNotIn("Duckov", out)
        out, _ = self.run_cli("list", "--since", "1h")
        self.assertNotIn("A.V.A", out)  # 2020
        out, _ = self.run_cli("list", "--since", "100000d")
        self.assertIn("A.V.A", out)

    def test_games_summary(self):
        out, _ = self.run_cli("games")
        duckov = next(l for l in out.splitlines() if "Escape from Duckov" in l)
        self.assertIn("3167020", duckov)
        self.assertIn("1/0/0/0", duckov)
        self.assertTrue(duckov.rstrip().endswith("-"))  # no stream output recorded
        self.assertIn("vortek", next(l for l in out.splitlines() if "A.V.A" in l))

    def test_games_summary_shows_the_most_stream_output_of_any_play(self):
        plays = []
        for i, xfb in enumerate(({"pipelines": 3, "gs": 0, "begins": 900, "drawAuto": 0, "beginsPerMin": 30.0},
                                 {"pipelines": 1, "gs": 2, "begins": 9000, "drawAuto": 4, "beginsPerMin": 1800.4}, None)):
            play = json.loads(PLAY)
            play["id"] = "p%d" % i
            if xfb:
                play["gpu"]["xfb"] = xfb
            plays.append(json.dumps(play))
        self.path.write_text("\n".join(plays) + "\n", encoding="utf-8")
        out, _ = self.run_cli("games")
        duckov = next(l for l in out.splitlines() if "Escape from Duckov" in l)
        self.assertTrue(duckov.rstrip().endswith("3p/2g 1800/min"), duckov)
        self.assertIn("xfb", out.splitlines()[0])
        shown, _ = self.run_cli("show", "p1")
        self.assertIn('xfb       {"pipelines": 1, "gs": 2, "begins": 9000, "drawAuto": 4, "beginsPerMin": 1800.4}', shown)

    def test_show_prints_the_whole_play(self):
        out, _ = self.run_cli("show", "1")
        for text in ("#1 2026-10-28-07-steam#1", "exit: crash", "Player.log: Crash!!!", "per minute:", "fps  [",
                     "MODERATE", "libhybris-preload.so", "2026-10-28T01:37:42+11:00", "mibridge (share granted):"):
            self.assertIn(text, out)
        self.assertEqual(out, self.run_cli("show", "2026-10-28-07-steam#1")[0])
        # The timeline, and the game's memory over the play.
        self.assertIn("+   10m  focus away (769)", out)
        self.assertIn("+   20m  mark", out)
        self.assertIn("game anon: 1155 -> 1185 MB, +1.0 MB/min over 30 min", out)
        self.assertIn("scanned: Player.log", out)
        last, _ = self.run_cli("show", "last")
        self.assertIn("A.V.A Global", last)
        self.assertIn("errors: -", last)

    def test_a_pack_without_the_semaphore_pool_shows(self):
        play = json.loads(PLAY)
        play["flags"] = ["no_sempool"]
        play["env"].update(BL_SYNC="0", BL_SYNC_PACK="valve-experimental-11.0-20261027b-r1", BL_SYNC_SEMPOOL="none", WINEESYNC="1")
        self.path.write_text(json.dumps(play) + "\n", encoding="utf-8")
        out, _ = self.run_cli("show", "1")
        sync = out[out.index("sync:"):out.index("display:")]
        self.assertIn("BL_SYNC_SEMPOOL  none", sync)
        self.assertIn("BL_SYNC_PACK     valve-experimental-11.0-20261027b-r1", sync)
        self.assertNotIn("VK_ICD_FILENAMES", sync)
        self.assertIn("nopool", self.run_cli("problems")[0])

    def test_the_preload_semaphore_pool_shows_without_a_problem(self):
        play = json.loads(PLAY)
        play["flags"] = []
        play["env"].update(BL_SYNC="0", BL_SYNC_PACK="valve-experimental-11.0-20261105c-r1", BL_SYNC_SEMPOOL="preload", WINEESYNC="1")
        self.path.write_text(json.dumps(play) + "\n", encoding="utf-8")
        out, _ = self.run_cli("show", "1")
        sync = out[out.index("sync:"):out.index("display:")]
        self.assertIn("BL_SYNC_SEMPOOL  preload", sync)
        self.assertNotIn("nopool", self.run_cli("problems")[0])

    def test_show_unknown_play_fails(self):
        with self.assertRaises(SystemExit):
            self.run_cli("show", "9")

    def test_problems_lists_flagged_plays_and_marks_with_their_logs(self):
        out, _ = self.run_cli("problems")
        lines = out.splitlines()
        flagged = next(l for l in lines if "Escape from Duckov" in l and "CRASH" in l)
        self.assertTrue(flagged.rstrip().endswith("yes"))
        self.assertNotIn("A.V.A", out)
        mark = next(l for l in lines if l.startswith("10-28 01:37"))
        self.assertIn("20m", mark)
        self.assertIn("MODERATE", mark)

    def test_json_passthrough(self):
        out, _ = self.run_cli("--json", "list")
        records = [json.loads(l) for l in out.splitlines()]
        self.assertEqual(["2026-10-28-07-steam#1", "2026-10-28-08-steam#1"], [r["id"] for r in records])

    def test_open_plays(self):
        (pathlib.Path(self.dir.name) / "open.json").write_text(json.dumps(
            {"v": 1, "appPid": 4242, "written": "2026-10-28T03:00:00+11:00", "logs": "2026-10-28-09-steam",
             "plays": [{"id": "2026-10-28-09-steam#1", "record": json.loads(PLAY)}]}))
        out, _ = self.run_cli("open")
        self.assertIn("app pid 4242", out)
        self.assertIn("Escape from Duckov", out)

    def test_sparkline(self):
        self.assertEqual(" .@", self.pj.sparkline([0, 6, 60], lo=0))
        self.assertEqual("_+", self.pj.sparkline([None, 5]))
        self.assertEqual("", self.pj.sparkline([]))


if __name__ == "__main__":
    unittest.main()
