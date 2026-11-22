import io
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

import server


class VoiceSyncTest(unittest.TestCase):
    def test_interrupted_upload_reply_loss_and_worker_retry(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(server, "DATA", Path(directory)):
            server.initialize()
            sample = io.BytesIO()
            with wave.open(sample, "wb") as wav:
                wav.setparams((1, 2, 16000, 0, "NONE", ""))
                wav.writeframes(bytes(32000))
            data = sample.getvalue()
            name = "rec-20261123-090000-00000001.wav"
            def send(offset, block):
                return server.receive(name, offset, len(data), block)
            self.assertFalse(send(0, data[:1024])["received"])
            self.assertEqual(send(0, data[:1024])["offset"], 1024)
            self.assertEqual(send(2048, data[2048:])["offset"], 1024)
            self.assertTrue(send(1024, data[1024:])["received"])
            self.assertTrue(send(0, data[:1024])["received"])
            def record():
                with server.database() as db:
                    return db.execute("SELECT * FROM recordings").fetchone()
            with patch.object(server.cloud, "transcribe", return_value="明天还书") as asr:
                with patch.object(server.cloud, "extract", side_effect=TimeoutError), self.assertRaises(TimeoutError):
                    server.process(record())
                with patch.object(server.cloud, "extract", return_value=([("还书", "", "2026-11-24", None)], ["原文"])):
                    server.process(record())
                asr.assert_called_once()
            self.assertEqual(record()["state"], "ready")
            task, = server.sync([])["tasks"]
            self.assertEqual(server.sync([[task[0], 1, 1]])["tasks"][0][-2:], [1, 1])
            self.assertEqual(server.sync([[task[0], 1, 1]])["tasks"][0][-2:], [1, 1])
            self.assertEqual(server.sync([[task[0], 0, 1]])["tasks"][0][-2:], [0, 1])
            with self.assertRaises(ValueError):
                send(1024, bytes([10]) + data[1025:])


if __name__ == "__main__":
    unittest.main()
