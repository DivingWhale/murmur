import io
import json
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

import server
import cloud


class VoiceSyncTest(unittest.TestCase):
    def test_note_pages_and_cached_revision(self):
        def fetch(version="", after=0):
            return server.sync({"edits": [], "notes_version": version, "notes_after": after})["notes"]
        with tempfile.TemporaryDirectory() as directory, patch.object(server, "DATA", Path(directory)):
            server.initialize()
            empty = fetch()
            self.assertEqual(empty["items"], [])
            self.assertIsNone(fetch(empty["version"]))
            with server.database() as db:
                db.execute("INSERT INTO recordings(name,size) VALUES('sample',0)")
                db.executemany("INSERT INTO notes(source,body,show_on_device) VALUES('sample',?,1)", [("随记 " + str(i),) for i in range(24)])
            first = fetch(empty["version"])
            self.assertTrue(first["more"])
            last = fetch(first["version"], first["items"][-1][0])
            self.assertFalse(last["more"])
            self.assertEqual([i for i, _, _ in first["items"] + last["items"]], list(range(1, 25)))
            self.assertEqual(last["items"][-1][1], "")
            with server.database() as db:
                db.execute("DELETE FROM notes WHERE id=3")
            restart = fetch(first["version"], 20)
            self.assertEqual(restart["after"], 0)
            self.assertNotEqual(restart["version"], first["version"])
            self.assertNotIn("notes", server.sync([]))

    def test_excerpt_and_model_contract(self):
        self.assertEqual(server.excerpt("一" * 44), "一" * 44)
        self.assertEqual(server.excerpt("一" * 43 + "，。再见"), "一" * 42 + "…")
        self.assertEqual(server.excerpt("一" * 42 + " English"), "一" * 42 + "…")
        self.assertEqual(server.excerpt("Cafe\u0301\n 有风。"), "Café 有风。")
        output = {"tasks": [], "notes": [{"title": " 雨里的长椅 ", "body": " 公园那张长椅淋湿了。 "}]}
        with patch.object(cloud, "chat", return_value=json.dumps(output)) as chat:
            self.assertEqual(cloud.extract("公园那张长椅淋湿了。", None), ([], [("雨里的长椅", "公园那张长椅淋湿了。")]))
            fmt = chat.call_args.kwargs["response_format"]["json_schema"]
            self.assertTrue(fmt["strict"])
            self.assertEqual(set(fmt["schema"]["properties"]["notes"]["items"]["required"]), {"title", "body"})
        for invalid in ({"tasks": [], "notes": ["原话"]}, {"tasks": [], "notes": [{"body": "原话"}]},
                        {"tasks": [], "notes": [{"title": " ", "body": "原话"}]},
                        {"tasks": [], "notes": [{"title": "一\n二", "body": "原话"}]},
                        {"tasks": [], "notes": [{"title": "标题", "body": ""}]},
                        {"tasks": [{"title": "买书"}], "notes": []},
                        {"tasks": [], "notes": [], "extra": True}):
            with patch.object(cloud, "chat", return_value=json.dumps(invalid)), self.assertRaises(ValueError):
                cloud.extract("文字", None)

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
                with patch.object(server.cloud, "extract", return_value=([("还书", "", "2026-11-24", None)], [("书里的落叶", "原文")])):
                    queued = record()
                    server.process(queued)
                    server.process(queued)
                asr.assert_called_once()
            self.assertEqual(record()["state"], "ready")
            with server.database() as db:
                self.assertEqual([tuple(r) for r in db.execute("SELECT title,body FROM notes")], [("书里的落叶", "原文")])
            self.assertEqual(server.sync({"edits": [], "notes_after": 0, "notes_version": ""})["notes"]["items"], [])
            task, = server.sync([])["tasks"]
            self.assertEqual(server.sync([[task[0], 1, 1]])["tasks"][0][-2:], [1, 1])
            self.assertEqual(server.sync([[task[0], 1, 1]])["tasks"][0][-2:], [1, 1])
            self.assertEqual(server.sync([[task[0], 0, 1]])["tasks"][0][-2:], [0, 1])
            with self.assertRaises(ValueError):
                send(1024, bytes([10]) + data[1025:])


if __name__ == "__main__":
    unittest.main()
