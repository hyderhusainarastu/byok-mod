"""tests/host/test_presets.py -- byok.dashboard.presets + the bundled
presets/*.yaml manifest."""

from __future__ import annotations

import datetime
import os
import sys
import tempfile
import textwrap
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)

from byok.dashboard import presets  # noqa: E402
from byok.dashboard import render as render_mod  # noqa: E402
from byok.dashboard.fonts import FontSet  # noqa: E402

_FONTS = FontSet()
_NOW = datetime.datetime(2026, 9, 4, 14, 32, 7)


class BundledManifestTests(unittest.TestCase):
    """The real presets/manifest.yaml + presets/*.yaml shipped in this repo."""

    def test_loads_four_presets_in_manifest_order(self):
        entries = presets.default_presets()
        self.assertEqual([e.name for e in entries], ["clock", "work", "media", "writing"])

    def test_display_names_are_all_within_the_wire_limit(self):
        for e in presets.default_presets():
            self.assertLessEqual(len(e.display.encode("utf-8")), presets.MAX_DISPLAY_NAME_LEN)

    def test_every_preset_config_loads_and_renders_offline(self):
        for e in presets.default_presets():
            with self.subTest(preset=e.name):
                cfg = presets.load_preset_config(e)
                img = render_mod.render_and_quantize(cfg, 240, 80, 1, _NOW, providers={}, fonts=_FONTS)
                self.assertEqual(img.size, (240, 80))

    def test_media_preset_has_no_qr_widget(self):
        cfg = presets.load_preset_config(presets.find_preset("media"))
        types = [w.type for w in cfg.widgets]
        self.assertNotIn("qr", types)

    def test_find_preset_is_case_insensitive(self):
        self.assertEqual(presets.find_preset("MEDIA").name, "media")
        self.assertEqual(presets.find_preset("Clock").name, "clock")

    def test_find_preset_unknown_name_raises_with_available_list(self):
        with self.assertRaises(presets.PresetError) as cm:
            presets.find_preset("no-such-preset")
        self.assertIn("clock", str(cm.exception))

    def test_display_names_helper_matches_manifest_order(self):
        entries = presets.default_presets()
        self.assertEqual(presets.display_names(entries), ["CLOCK", "WORK", "MEDIA", "WRITING"])


class ManifestValidationTests(unittest.TestCase):
    """Structural validation against small synthetic manifests -- proves
    presets.py fails loudly (PresetError) on a malformed manifest rather
    than silently degrading, unlike every widget Provider in this
    package (see presets.py's PresetError docstring for why the two
    failure philosophies are deliberately different)."""

    def _write(self, tmp, name, text):
        path = os.path.join(tmp, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(textwrap.dedent(text))
        return path

    def test_missing_presets_key_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(tmp, "manifest.yaml", "not_presets: []\n")
            with self.assertRaises(presets.PresetError):
                presets.load_manifest(path)

    def test_missing_required_key_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._write(tmp, "a.yaml", "widgets: []\n")
            path = self._write(
                tmp, "manifest.yaml",
                """
                presets:
                  - name: a
                    file: a.yaml
                """,
            )
            with self.assertRaises(presets.PresetError) as cm:
                presets.load_manifest(path)
            self.assertIn("display", str(cm.exception))

    def test_display_name_over_limit_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._write(tmp, "a.yaml", "widgets: []\n")
            path = self._write(
                tmp, "manifest.yaml",
                """
                presets:
                  - name: a
                    display: "This display name is definitely far too long"
                    file: a.yaml
                """,
            )
            with self.assertRaises(presets.PresetError) as cm:
                presets.load_manifest(path)
            self.assertIn("exceeds", str(cm.exception))

    def test_duplicate_name_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._write(tmp, "a.yaml", "widgets: []\n")
            path = self._write(
                tmp, "manifest.yaml",
                """
                presets:
                  - name: a
                    display: "A"
                    file: a.yaml
                  - name: a
                    display: "A2"
                    file: a.yaml
                """,
            )
            with self.assertRaises(presets.PresetError) as cm:
                presets.load_manifest(path)
            self.assertIn("duplicate", str(cm.exception).lower())

    def test_missing_referenced_file_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(
                tmp, "manifest.yaml",
                """
                presets:
                  - name: a
                    display: "A"
                    file: does-not-exist.yaml
                """,
            )
            with self.assertRaises(presets.PresetError) as cm:
                presets.load_manifest(path)
            self.assertIn("does-not-exist.yaml", str(cm.exception))

    def test_missing_manifest_file_raises_file_not_found(self):
        with self.assertRaises(FileNotFoundError):
            presets.load_manifest("/no/such/manifest.yaml")

    def test_more_than_eight_presets_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            lines = ["presets:"]
            for i in range(9):
                self._write(tmp, f"p{i}.yaml", "widgets: []\n")
                lines.append(f"  - name: p{i}\n    display: \"P{i}\"\n    file: p{i}.yaml")
            path = self._write(tmp, "manifest.yaml", "\n".join(lines) + "\n")
            with self.assertRaises(presets.PresetError) as cm:
                presets.load_manifest(path)
            self.assertIn("9", str(cm.exception))

    def test_bundled_manifest_is_within_the_eight_preset_limit(self):
        self.assertLessEqual(len(presets.default_presets()), presets.MAX_PRESET_COUNT)


if __name__ == "__main__":
    unittest.main()
