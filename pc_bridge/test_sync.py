import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from pc_bridge.sync import Bridge
from pc_bridge.markdown import Note
from pc_bridge.manual import ManualRequests
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "backend"))
import server
import bridge_api


class SyncTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        work = Path(tmp.name)
        self.root = work / "Murmur"
        self.state = work / "state.sqlite"
        p = patch.object(server,"DATA",work/"pi")
        p.start()
        self.addCleanup(p.stop)
        server.initialize()
        self.b = Bridge(self.root,self.state,self.api,grace=0)
        self.addCleanup(lambda: self.b.close())
        self.b.cycle()

    def api(self, endpoint, data):
        with server.database() as db:
            return bridge_api.dispatch(db, "/v1/pc/"+endpoint, data)

    def reopen(self):
        self.b.close()
        self.b = Bridge(self.root,self.state,self.api,grace=0)

    def file(self, folder):
        return next((self.root/folder).glob("*.md"))

    @staticmethod
    def write(p, data):
        p.write_text(data,encoding="utf-8")

    def test_voice_dates_hearth_and_moved_file(self):
        with server.database() as db:
            db.execute("INSERT INTO recordings(name,recorded_at,size,transcript) VALUES('voice','2025-11-23T10:00:00+08:00',44,'明天打印')")
            db.execute("""INSERT INTO tasks(source,title,details,planned_at,due_at) VALUES('voice','打印','海报',
                       '2025-11-24T10:00+08:00','2025-11-26')""")
        self.b.cycle()
        p = self.file("Tasks")
        data = p.read_text(encoding="utf-8")
        self.assertIn("⏳ 2025-11-24 📅 2025-11-26",data)
        self.assertIn("明天打印",self.file("Recordings").read_text(encoding="utf-8"))
        self.assertIn("打印",(self.root/"Today.md").read_text(encoding="utf-8"))
        self.write(p,data.replace("- [ ]","- [x]").replace("海报","两张海报").replace("⏳ 2025-11-24","⏳ 2025-11-25"))
        p = p.rename(self.root/"renamed.md")
        self.b.cycle()
        t, = server.sync([])["tasks"]
        self.assertEqual(t[2:6],["两张海报","2025-11-25T10:00+08:00","2025-11-26",1])
        self.write(p,p.read_text(encoding="utf-8").replace("- [x]","- [ ]"))
        self.b.cycle()
        self.assertEqual(server.sync([])["tasks"][0][5],0)
        stamp = p.stat().st_mtime_ns
        self.b.cycle()
        self.assertEqual((self.b.writes, p.stat().st_mtime_ns),(0,stamp))
        self.assertIn("2025-11-23T10:00:00+08:00",p.read_text(encoding="utf-8"))

    def test_hearth_delete_line_undo_restart_and_archive(self):
        p = self.root/"Tasks"/"native.md"
        self.write(p, "---\ntags: [personal]\n---\n- [ ] Keep original\n\nDetails\n")
        self.b.cycle()
        original = p.read_text(encoding="utf-8")
        removed = original.replace("- [ ] Keep original\n\n","")
        self.write(p,removed)
        self.b.cycle()
        self.assertEqual(len(server.sync([])["tasks"]),1)
        self.write(p,original)
        self.b.cycle()
        self.assertFalse(self.b.db.execute("SELECT 1 FROM removed_task_lines").fetchone())
        self.write(p,removed.replace("Details","half-written edit"))
        self.b.cycle()
        self.b.cycle()
        self.assertTrue(self.b.db.execute("SELECT 1 FROM errors").fetchone())
        self.assertEqual(len(server.sync([])["tasks"]),1)
        self.write(p,removed)
        self.b.cycle()
        self.reopen()
        self.b.cycle()
        self.assertEqual(server.sync([])["tasks"],[])
        self.assertFalse(p.exists())
        self.assertEqual(self.file("Archive").read_text(encoding="utf-8"),original)

    def test_manual_sync_confirms_hearth_delete_but_preserves_missing_file_grace(self):
        self.b.grace = 120
        p = self.root/"Tasks"/"native.md"
        missing = self.root/"Tasks"/"whole.md"
        self.write(p, "- [ ] Remove through Hearth\n\nOriginal details\n")
        self.write(missing, "- [ ] Missing file\n")
        self.b.cycle()
        original = p.read_text(encoding="utf-8")
        self.write(p, original.replace("- [ ] Remove through Hearth\n\n", ""))
        missing.unlink()
        self.b.cycle(manual=True)
        self.assertEqual([t[1] for t in server.sync([])["tasks"]], ["Missing file"])
        self.assertFalse(p.exists())
        self.assertEqual(self.file("Archive").read_text(encoding="utf-8"), original)

    @unittest.skipUnless(os.name == "nt", "uses NTFS file IDs")
    def test_manual_memo_delete_confirms_only_the_file_in_trash(self):
        self.b.grace = 120
        paths = {}
        for name in ("recycled", "moved", "missing"):
            paths[name] = self.root / "Memos" / (name + ".md")
            self.write(paths[name], "---\nshow_on_device: true\n---\n" + name)
        self.b.cycle()
        trash = self.root.parent / ".trash"
        elsewhere = self.root.parent / "other" / ".trash"
        trash.mkdir()
        elsewhere.mkdir(parents=True)
        paths["recycled"].rename(trash / "renamed.md")
        paths["moved"].rename(elsewhere / "kept.md")
        paths["missing"].unlink()
        self.b.cycle(manual=True)
        with server.database() as db:
            self.assertEqual({r[0] for r in db.execute("SELECT body FROM notes")}, {"moved", "missing"})
        snap = server.sync({"edits": [], "notes_version": "", "notes_after": 0})["notes"]
        self.assertEqual({r[2] for r in snap["items"]}, {"moved", "missing"})

    def test_manual_signal_during_sync_waits_for_its_own_cycle(self):
        queue = ManualRequests(self.root)
        queue.folder = self.root.parent/"signals"
        queue.folder.mkdir()
        first = queue.folder/("1"*32 + ".request")
        second = queue.folder/("2"*32 + ".request")
        first.touch()
        batch = queue.wait(60)
        second.touch()
        queue.reply(batch, "已提交")
        self.assertEqual(first.with_suffix(".result").read_text(encoding="utf-8"), "已提交")
        self.assertEqual(queue.wait(0), [second])
        self.assertFalse(second.with_suffix(".result").exists())

    def test_offline_restart_lost_create_response_delete(self):
        p = self.root/"Tasks"/"a.md"
        self.write(p,"- [ ] thing\n")
        def down(endpoint,data):
            raise OSError("offline")
        self.b.api = down
        with self.assertRaises(OSError):
            self.b.cycle()
        self.assertIn("murmur_client_id",p.read_text())
        self.reopen()
        def lost(endpoint,data):
            result = self.api(endpoint,data)
            if endpoint == "edit":
                raise TimeoutError()
            return result
        self.b.api = lost
        with self.assertRaises(TimeoutError):
            self.b.cycle()
        self.reopen()
        self.b.cycle()
        self.assertEqual(len(server.sync([])["tasks"]),1)
        self.assertIn("murmur_id:",p.read_text())
        p.unlink()
        self.b.cycle()
        self.assertEqual(len(server.sync([])["tasks"]),1)
        self.reopen()
        self.b.cycle()
        self.assertEqual(server.sync([])["tasks"],[])

    def test_memo_properties_tombstone_and_snapshot(self):
        p = self.root/"Memos"/"memo.md"
        self.write(p,"---\ntags:\n  - 自定义\nextra: |\n  hello: world\n---\n随记\n")
        self.b.cycle()
        def page(token=""):
            return server.sync({"edits":[],"notes_version":token,"notes_after":0})["notes"]
        old = page()
        self.assertFalse(old["items"])
        self.assertEqual(Note(p.read_text(encoding="utf-8")).props["show_on_device"], "false")
        self.write(p,p.read_text(encoding="utf-8").replace("false","true").replace("随记","手写"))
        self.b.cycle()
        visible = page(old["version"])
        self.assertEqual(visible["items"][0][1:],["","手写"])
        with server.database() as db:
            db.execute("UPDATE notes SET body='来自树莓派',show_on_device=0")
            self.assertIsNone(db.execute("SELECT source FROM notes").fetchone()[0])
        self.b.cycle()
        self.assertIn("extra: |\n  hello: world",p.read_text(encoding="utf-8"))
        self.assertIn("来自树莓派",p.read_text(encoding="utf-8"))
        self.assertEqual(page(visible["version"])["items"],[])
        p.unlink()
        self.b.cycle()
        self.b.cycle()
        with server.database() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM notes").fetchone()[0],0)
            db.execute("INSERT INTO notes(body) VALUES('另一条')")
        self.b.cycle()
        other = self.file("Memos")
        with server.database() as db:
            db.execute("DELETE FROM notes")
        self.b.cycle()
        self.assertFalse(other.exists())
        self.assertTrue(self.file("Archive").exists())

    def test_conflicting_text_and_independent_device_fields(self):
        with server.database() as db:
            db.execute("INSERT INTO notes(body) VALUES('base')")
            db.execute("INSERT INTO tasks(title,details) VALUES('prepare','')")
        self.b.cycle()
        p = self.file("Memos")
        self.write(p,p.read_text().replace("\nbase","\nlocal"))
        t = self.file("Tasks")
        self.write(t,t.read_text().replace("prepare","ready"))
        with server.database() as db:
            db.execute("UPDATE notes SET body='remote'")
        server.sync([[1,1,0]])
        self.b.cycle()
        self.assertIn("local",self.file("Conflicts").read_text())
        self.assertIn("remote",p.read_text())
        self.assertIn("- [x] ready",t.read_text())
        payload = {"edits":[[1,0,1]],"notes_after":0,"notes_version":"",
                   "task_operations":{"1":[2,"0123456789abcdef"]}}
        self.assertEqual(server.sync(payload)["tasks"][0][-2:],[1,1])
        self.assertEqual(server.sync(payload)["tasks"][0][-2:],[1,1])

    def test_dry_run_no_writes_and_invalid_checkbox(self):
        with server.database() as db:
            db.execute("INSERT INTO notes(body) VALUES('incoming')")
        p = self.root/"Tasks"/"new.md"
        self.write(p,"- [ ] demo")
        dry = Bridge(self.root,self.state,self.api,dry=True)
        try:
            dry.cycle()
            self.assertTrue(any("upload" in x for x in dry.plan))
        finally:
            dry.close()
        self.assertEqual(p.read_text(),"- [ ] demo")
        self.assertFalse(list((self.root/"Memos").iterdir()))
        self.assertEqual(server.sync([])["tasks"],[])
        self.b.cycle()
        self.write(p,"---\nmurmur_id: broken\n---\n- [ ] bad\n")
        self.b.cycle()
        self.assertTrue(self.b.db.execute("SELECT 1 FROM errors").fetchone())
        self.assertEqual(server.sync([])["tasks"][0][1],"demo")

    def test_legacy_migration_and_stable_ids(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(server,"DATA",Path(tmp)):
            with server.database() as db:
                db.executescript(server.SCHEMA)
                db.execute("INSERT INTO notes(id,source,body) VALUES(9,'wav','legacy')")
            server.initialize()
            a, = self.api("changes",{"after":0})["items"]
            self.assertFalse(a["show_on_device"])
            server.initialize()
            self.assertEqual(self.api("changes",{"after":a["rev"]})["items"],[])
            with server.database() as db:
                db.execute("DELETE FROM notes")
                db.execute("INSERT INTO notes(body) VALUES('new')")
                self.assertGreater(db.execute("SELECT id FROM notes").fetchone()[0],9)

    def test_hearth_completion_stamp_and_idle_reads(self):
        p = self.root/"Tasks"/"native.md"
        self.write(p, "- [ ] 打印 ⏳ 2026-11-23 📅 2026-11-26\n")
        self.b.cycle()
        native = p.read_text(encoding="utf-8").replace("[ ]", "[x]").replace("📅 2026-11-26","📅 2026-11-26 ✅ 2026-11-23")
        self.write(p, native)
        self.b.cycle()
        task, = server.sync([])["tasks"]
        self.assertEqual((task[1],task[5]), ("打印",1))
        with server.database() as db:
            db.execute("UPDATE tasks SET title='新版'")
        self.b.cycle()
        self.assertIn("新版 ⏳ 2026-11-23 📅 2026-11-26 ✅ 2026-11-23",p.read_text(encoding="utf-8"))
        with patch.object(self.b, "read", wraps=self.b.read) as read:
            self.b.cycle()
            self.assertFalse(any(args.args[0].startswith("Tasks/") for args in read.call_args_list))
            self.assertEqual(self.b.writes,0)

    @unittest.skipUnless(os.name == "nt", "Windows file ID resolution")
    def test_ntfs_move_outside_and_restore(self):
        p = self.root/"Memos"/"file.md"
        self.write(p,"memo")
        self.b.cycle()
        moved = p.rename(self.root.parent/"outside.md")
        self.b.cycle()
        self.b.cycle()
        with server.database() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM notes").fetchone()[0],1)
        moved.rename(self.root/"returned.md")
        self.b.cycle()
        self.assertFalse(self.b.db.execute("SELECT 1 FROM errors").fetchone())
        self.assertEqual(self.b.db.execute("SELECT path FROM files").fetchone()[0],"returned.md")

    def test_cas_between_pull_and_push_and_batch_replay(self):
        with server.database() as db:
            db.execute("INSERT INTO notes(body) VALUES('base')")
        self.b.cycle()
        f = self.file("Memos")
        self.write(f,f.read_text().replace("\nbase","\nlocal"))
        for i in range(35):
            self.write(self.root/"Tasks"/f"{i}.md",f"- [ ] item {i}")
        def race(endpoint,data):
            if endpoint == "edit":
                with server.database() as db:
                    db.execute("UPDATE notes SET body='other' WHERE body='base'")
            return self.api(endpoint,data)
        self.b.api = race
        self.b.cycle()
        self.assertEqual(len(server.sync([])["tasks"]),35)
        self.assertIn("local",self.file("Conflicts").read_text())
        self.assertIn("other",f.read_text())
        self.reopen()
        self.b.cycle()
        self.assertEqual(len(server.sync([])["tasks"]),35)

    def test_lost_update_and_delete_replies(self):
        p = self.root/"Memos"/"lost.md"
        self.write(p,"one")
        self.b.cycle()
        def lost(endpoint,data):
            result = self.api(endpoint,data)
            if endpoint == "edit":
                raise TimeoutError("reply lost after commit")
            return result
        self.write(p,p.read_text().replace("\none","\ntwo"))
        self.b.api = lost
        with self.assertRaises(TimeoutError):
            self.b.cycle()
        self.reopen()
        self.b.cycle()
        with server.database() as db:
            self.assertEqual([r[0] for r in db.execute("SELECT body FROM notes")],["two"])
        p.unlink()
        self.b.cycle()
        self.b.api = lost
        with self.assertRaises(TimeoutError):
            self.b.cycle()
        self.reopen()
        self.b.cycle()
        self.assertFalse(list((self.root/"Memos").glob("*.md")))
        self.assertIsNone(self.b.db.execute("SELECT 1 FROM files").fetchone())

    def test_memo_names_previews_and_identity(self):
        with server.database() as db:
            db.executemany("INSERT INTO notes(body) VALUES(?)", [
                ("清晨的风，窗外很安静。",), ("清晨的风，沿河走了一圈。",)])
        self.b.cycle()
        files = sorted((self.root / "Memos").glob("*.md"))
        self.assertEqual({p.stem for p in files}, {"清晨的风", "清晨的风 (2)"})
        notes = [Note(p.read_text(encoding="utf-8")) for p in files]
        self.assertNotEqual(notes[0].props["murmur_id"], notes[1].props["murmur_id"])
        for n in notes:
            self.assertEqual(n.props["murmur_preview"], n.body.strip())
        rel = files[0].rename(files[0].with_name("我的标题.md"))
        changed = Note(rel.read_text(encoding="utf-8")).render({}, "后来又下了雨。\n")
        self.write(rel, changed)
        self.b.cycle()
        self.reopen()
        self.b.cycle()
        self.assertEqual(self.b.writes, 0)
        n = Note(rel.read_text(encoding="utf-8"))
        self.assertEqual(n.props["murmur_id"], notes[0].props["murmur_id"])
        self.assertEqual(n.props["murmur_preview"], "后来又下了雨。")

    def test_generated_memo_titles_reach_vault_and_preserve_user_rename(self):
        with server.database() as db:
            db.executemany("INSERT INTO notes(title,body) VALUES(?,?)", [
                ("屋檐下，听雨", "我记得那场雨。"), ("屋檐下，听雨", "今天我又去了。")])
        self.b.cycle()
        folder = self.root / "Memos"
        self.assertEqual({p.stem for p in folder.glob("*.md")}, {"屋檐下，听雨", "屋檐下，听雨 (2)"})
        path = (folder / "屋檐下，听雨.md").rename(folder / "我自己的标题.md")
        note = Note(path.read_text(encoding="utf-8"))
        self.assertEqual(note.body.strip(), "我记得那场雨。")
        self.write(path, note.render({}, "屋檐下，两个人一起听雨。\n"))
        self.b.cycle()
        with server.database() as db:
            row = db.execute("SELECT title,body FROM notes WHERE uid=?", (note.props["murmur_id"],)).fetchone()
            self.assertEqual(tuple(row), ("屋檐下，听雨", "屋檐下，两个人一起听雨。"))
        self.assertTrue(path.exists())
