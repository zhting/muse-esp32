# SPDX-License-Identifier: Apache-2.0
"""components/muse/muse_tts.c on pthreads against a scripted streaming TTS
server (tests/tts_fakes): speech is handed over as it arrives, a cancel or a
new message never lets the old one's audio through, and failures end cleanly."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
FAKES = ROOT / "tests/tts_fakes"
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
# Short timeouts, so the stall and no-answer cases take a second, not twenty.
TIMING = ["-DHEADERS_POLL_MS=200", "-DPOLL_MS=50", "-DFIRST_AUDIO_TIMEOUT_US=1000000LL",
          "-DSTALL_TIMEOUT_US=500000LL"]


class MuseTtsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        if not (JSON / "cJSON.c").exists():
            raise unittest.SkipTest(f"cJSON sources not found in {JSON} (run one idf.py build first)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-pthread",
                 "-I", str(FAKES), "-I", str(MUSE), "-I", str(JSON), *TIMING]
        sources = [str(ROOT / "tests/muse_tts_harness.c"), str(MUSE / "muse_tts.c"),
                   str(MUSE / "muse_tts_stream.c"), str(FAKES / "fakes.c"), str(out / "cjson.o")]
        cls.binaries = {}
        built = subprocess.run([*cc, *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
                               capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)
        for name, extra in (("key", []), ("no-key", ['-DCONFIG_MUSE_TTS_API_KEY=""'])):
            binary = out / f"harness-{name}"
            built = subprocess.run([*cc, *flags, *extra, *sources, "-lm", "-o", str(binary)],
                                   capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries[name] = binary

    def run_harness(self, name: str, *args: str) -> None:
        result = subprocess.run([str(self.binaries[name]), *args], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_streaming_cancel_and_failures(self):
        self.run_harness("key")

    def test_without_a_key_replies_stay_text(self):
        self.run_harness("no-key", "no-key")


if __name__ == "__main__":
    unittest.main()