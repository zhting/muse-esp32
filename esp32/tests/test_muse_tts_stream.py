# SPDX-License-Identifier: Apache-2.0
"""components/muse/muse_tts_stream.c: framing and decoding Volcengine's streaming
TTS body, however the network chunks it."""

from __future__ import annotations

import base64
import json
import os
import random
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
END = 20000000


def audio_pieces(seed: int, count: int = 6) -> list[bytes]:
    rng = random.Random(seed)
    return [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 3000))) for _ in range(count)]


def piece(data: bytes | None, code: int = 0, message: str = "", **extra) -> str:
    obj = {"code": code, "message": message}
    if data is not None:
        obj["data"] = base64.b64encode(data).decode()
    obj.update(extra)
    return json.dumps(obj, ensure_ascii=False)


class TtsStreamTest(unittest.TestCase):
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
        cls.binary = out / "harness"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE", "-I", str(JSON), "-I", str(MUSE)]
        for cmd in (
            [*cc, *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, *flags, str(ROOT / "tests/muse_tts_stream_harness.c"), str(MUSE / "muse_tts_stream.c"),
             str(out / "cjson.o"), "-lm", "-o", str(cls.binary)],
        ):
            built = subprocess.run(cmd, capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)

    def run_body(self, body: str | bytes, chunk: int, *extra: int) -> tuple[dict, bytes]:
        path = Path(self.tmp.name) / "body"
        path.write_bytes(body.encode() if isinstance(body, str) else body)
        result = subprocess.run([str(self.binary), str(path), str(chunk), *map(str, extra)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        head, audio = result.stdout.split("\n")[:2]
        fields = dict(kv.split("=", 1) for kv in head.split(" ", 3))
        return fields, bytes.fromhex(audio)

    def test_ndjson_in_any_chunking(self):
        pieces = audio_pieces(1)
        body = "".join(piece(p) + "\n" for p in pieces) + piece(None, END, "ok", usage={"text_words": 10}) + "\n"
        for chunk in (1, 2, 3, 7, 64, 1000, 4096, len(body)):
            with self.subTest(chunk=chunk):
                fields, audio = self.run_body(body, chunk)
                self.assertEqual(fields["status"], "end")
                self.assertEqual(int(fields["code"]), END)
                self.assertEqual(audio, b"".join(pieces))

    def test_sse_and_back_to_back_objects(self):
        pieces = audio_pieces(2)
        sse = "".join(f"event: 352\ndata: {piece(p)}\n\n" for p in pieces) + f"data: {piece(None, END, 'ok')}\n\n"
        joined = "".join(piece(p) for p in pieces) + piece(None, END, "ok")
        crlf = "".join(piece(p) + "\r\n" for p in pieces) + piece(None, END, "ok") + "\r\n"
        for name, body in (("sse", sse), ("joined", joined), ("crlf", crlf)):
            with self.subTest(name):
                fields, audio = self.run_body(body, 5)
                self.assertEqual(fields["status"], "end")
                self.assertEqual(audio, b"".join(pieces))

    def test_strings_with_braces_quotes_and_escaped_slashes(self):
        data = bytes(range(256)) * 3
        b64 = base64.b64encode(data).decode().replace("/", "\\/")
        body = (
            '{"code":0,"message":"a } brace, a \\" quote { and a [","sentence":{"text":"}}]]"},'
            f'"data":"{b64}"}}\n' + piece(None, END, "ok")
        )
        fields, audio = self.run_body(body, 3)
        self.assertEqual(fields["status"], "end")
        self.assertEqual(audio, data)

    def test_error_piece_stops_with_its_message(self):
        pieces = audio_pieces(3, 2)
        body = piece(pieces[0]) + "\n" + piece(None, 45000030, "resource not granted 资源未授权") + "\n" + piece(pieces[1])
        fields, audio = self.run_body(body, 16)
        self.assertEqual(fields["status"], "error")
        self.assertEqual(int(fields["code"]), 45000030)
        self.assertEqual(fields["message"], "resource not granted 资源未授权")
        self.assertEqual(audio, pieces[0])   # what came before the error was handed over

    def test_end_of_body(self):
        pieces = audio_pieces(4, 3)
        no_end = "".join(piece(p) + "\n" for p in pieces)
        fields, audio = self.run_body(no_end, 100)
        self.assertEqual(fields["status"], "end", "audio without the end piece still counts")
        self.assertEqual(audio, b"".join(pieces))
        fields, _ = self.run_body(no_end[:-40], 100)
        self.assertEqual((fields["status"], fields["message"]), ("error", "the body ended mid-piece"))
        fields, _ = self.run_body("", 100)
        self.assertEqual((fields["status"], fields["message"]), ("error", "no audio"))
        fields, _ = self.run_body("<html>502 Bad Gateway</html>", 100)
        self.assertEqual(fields["status"], "error")

    def test_oversized_piece_and_bad_json(self):
        big = audio_pieces(5, 1)[0] * 4
        fields, _ = self.run_body(piece(big) + piece(None, END, "ok"), 512, 1024)
        self.assertEqual(fields["status"], "error")
        self.assertIn("over 1024 bytes", fields["message"])
        fields, _ = self.run_body('{"code":0,"data":"AAAA",}' + piece(None, END), 7)
        self.assertEqual((fields["status"], fields["message"]), ("error", "not JSON"))
        fields, _ = self.run_body('{"code":0,"data":"A*AA"}', 7)
        self.assertEqual((fields["status"], fields["message"]), ("error", "bad base64"))

    def test_sink_can_stop_it(self):
        pieces = audio_pieces(6)
        body = "".join(piece(p) + "\n" for p in pieces) + piece(None, END, "ok")
        fields, audio = self.run_body(body, 50, 512 * 1024, len(pieces[0]))
        self.assertEqual((fields["status"], fields["message"]), ("error", "stopped"))
        self.assertEqual(audio, pieces[0])

    def test_base64_decoder(self):
        rng = random.Random(7)
        for n in (0, 1, 2, 3, 4, 5, 57, 58, 59, 1000):
            data = bytes(rng.randrange(256) for _ in range(n))
            for encoded in (base64.b64encode(data).decode(), base64.urlsafe_b64encode(data).decode(),
                            base64.encodebytes(data).decode()):   # the last has newlines every 76
                with self.subTest(n=n, encoded=encoded[:12]):
                    out = subprocess.run([str(self.binary), "--base64", encoded or "="],
                                         capture_output=True, text=True).stdout.strip()
                    self.assertEqual(bytes.fromhex(out), data)
        out = subprocess.run([str(self.binary), "--base64", "QUJD!"], capture_output=True, text=True).stdout
        self.assertEqual(out.strip(), "-1")


if __name__ == "__main__":
    unittest.main()