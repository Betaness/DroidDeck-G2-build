import collections
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
RES = ROOT / "app/src/main/res"
ZEROFG = ROOT / "app/src/main/cpp/thirdparty/zerofg"
WAYLANDCOMP = ROOT / "app/src/main/cpp/waylandcomp"
FORMAT = re.compile(r"%(?:\d+\$)?[-#+ 0,(]*\d*(?:\.\d+)?[a-zA-Z%]")
LABELS = ("frame_gen_zerofg_really", "frame_gen_zerofg_zero")


def strings(path):
    return {
        element.attrib["name"]: "".join(element.itertext())
        for element in ET.parse(path).getroot()
        if element.tag == "string"
    }


def locales():
    source = (ROOT / "app/src/main/java/com/droiddeck/launcher/core/AppLanguage.kt").read_text(encoding="utf-8")
    tags = re.findall(r'"([a-zA-Z0-9-]+)"', re.search(r"val supported:.*?listOf\((.*?)\)", source, re.S).group(1))
    special = {"id": "in", "es-419": "b+es+419", "pt-BR": "pt"}
    return {tag: RES / f"values-{special.get(tag, tag.replace('-', '-r'))}/strings.xml" for tag in tags if tag != "en"}


class ZeroFgMenuStringsTest(unittest.TestCase):
    def test_labels_take_the_multiplier(self):
        english = strings(RES / "values/strings.xml")
        for key in LABELS:
            with self.subTest(key=key):
                self.assertEqual(["%1$d"], FORMAT.findall(english[key]))
                self.assertIn("×", english[key])
        self.assertIn("RZ+", english["frame_gen_zerofg_really"])

    def test_every_language_file_has_the_labels(self):
        english = strings(RES / "values/strings.xml")
        files = sorted(RES.glob("values-*/strings.xml"))
        self.assertTrue(set(locales().values()) <= set(files))
        for path in files:
            translated = strings(path)
            for key in LABELS:
                with self.subTest(file=path.parent.name, key=key):
                    self.assertTrue(translated.get(key, "").strip(), "missing")
                    self.assertEqual(collections.Counter(FORMAT.findall(english[key])),
                                     collections.Counter(FORMAT.findall(translated[key])))


class ZeroFgVendoredTest(unittest.TestCase):
    def test_version_is_the_same_everywhere(self):
        upstream = (ZEROFG / "UPSTREAM.md").read_text(encoding="utf-8")
        tag = re.search(r"tag `(v[0-9.]+)`", upstream).group(1)
        self.assertRegex(upstream, r"commit\s+`[0-9a-f]{40}`")
        for path, text in ((WAYLANDCOMP / "CMakeLists.txt", f"ZeroFG {tag} (../thirdparty/zerofg"),
                           (WAYLANDCOMP / "ZEROFG.md", f"ZeroFG {tag}"),
                           (WAYLANDCOMP / "src/zerofg_glue.h", tag),
                           (WAYLANDCOMP / "src/zerofg_zc.c", f'"on: ZeroFG {tag[1:]}')):
            with self.subTest(file=path.name):
                self.assertTrue(text in path.read_text(encoding="utf-8"), f"{text!r} not in {path.name}")

    def test_apache_licence(self):
        licence = (ZEROFG / "LICENSE").read_text(encoding="utf-8")
        self.assertIn("Apache License", licence)
        self.assertIn("Version 2.0", licence)
        self.assertFalse((ZEROFG / "LICENSE-GPL-2.0").exists())
        self.assertTrue((ROOT / "app/src/main/assets/licenses/zerofg/LICENSE").exists())

    def test_the_build_finds_every_source_and_shipped_shader(self):
        cmake = (WAYLANDCOMP / "CMakeLists.txt").read_text(encoding="utf-8")
        sources = re.findall(r"\$\{DROIDDECK_ZEROFG_DIR\}/(src/\S+\.cc)", cmake)
        self.assertEqual({"src/zerofg.cc", "src/df/df_context.cc", "src/rc7/rc7_context.cc", "src/hud/hud_layer.cc"},
                         set(sources))
        prebuilt = ZEROFG / "shaders/prebuilt/rc3"
        for source in sources:
            text = (ZEROFG / source).read_text(encoding="utf-8")
            for header in re.findall(r'#include\s+"((?:rc3|rc7|df|hud)_[a-z0-9_]+\.h)"', text):
                with self.subTest(source=source, header=header):
                    self.assertTrue((prebuilt / header).exists() or (ZEROFG / "src" / header).exists()
                                    or any((ZEROFG / "src").rglob(header)))

    def test_shipped_spirv_has_a_source(self):
        prebuilt = sorted((ZEROFG / "shaders/prebuilt/rc3").glob("*.h"))
        self.assertTrue(prebuilt)
        sources = {f"{p.parent.name}_{p.stem}" if p.parent.name == "rc3" else p.stem
                   for p in (ZEROFG / "shaders").rglob("*.comp")}
        for header in prebuilt:
            base = re.sub(r"_(phase|q8|sg|wg|hudgs|gs)$", "", header.stem)
            with self.subTest(header=header.name):
                self.assertTrue(header.stem in sources or base in sources or any(s.startswith(base) for s in sources),
                                header.name)

if __name__ == "__main__":
    unittest.main()
