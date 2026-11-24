"""Single-process synchronizer. SQLite checkpoints only what is needed to reconcile notes."""
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import tempfile
import time
import uuid
from datetime import datetime, timedelta, timezone

from .markdown import Note, body, memo_preview, memo_title

FOLDERS = {"task": "Tasks", "memo": "Memos"}
FIELDS = {"task": ("title", "details", "planned_at", "due_at", "done"), "memo": ("body", "show_on_device")}
ASSETS = Path(__file__).with_name("templates")
MOVED_WARNING = "文件已移动，已暂停删除。移回 Murmur 后继续同步。"
SCHEMA = """
CREATE TABLE IF NOT EXISTS files(uid TEXT PRIMARY KEY, kind TEXT, path TEXT, stamp TEXT,
  base TEXT, pending TEXT, missing REAL, today TEXT, title TEXT);
CREATE INDEX IF NOT EXISTS paths ON files(path);
CREATE INDEX IF NOT EXISTS days ON files(today);
CREATE TABLE IF NOT EXISTS inbox(uid TEXT PRIMARY KEY, data TEXT);
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY,v TEXT);
CREATE TABLE IF NOT EXISTS errors(path TEXT PRIMARY KEY, reason TEXT);
CREATE TABLE IF NOT EXISTS conflicts(path TEXT PRIMARY KEY);
CREATE TABLE IF NOT EXISTS removed_task_lines(uid TEXT PRIMARY KEY, since REAL NOT NULL);
CREATE TEMP TABLE IF NOT EXISTS ambiguous(uid TEXT PRIMARY KEY);
"""


class Changed(OSError):
    """The editor changed a file while this cycle was running."""


def signature(s):
    return f"{s.st_mtime_ns}:{s.st_ctime_ns}:{s.st_size}:{s.st_ino}"


def linked(s):
    return stat.S_ISLNK(s.st_mode) or getattr(s, "st_file_attributes", 0) & 1024


def record_values(r):
    if not r or r["deleted"]:
        return None
    return {key: r[key] for key in FIELDS[r["kind"]]}


def dump(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


class Bridge:
    def __init__(self, root, state, api, grace=120, dry=False, vault=None):
        self.root, self.api, self.grace, self.dry = Path(root), api, grace, dry
        self.vault = Path(vault or self.root.parent)
        self.prefix = self.root.relative_to(self.vault).as_posix()
        self.writes = 0
        self.overlay = {}
        self.plan = []
        self.db = sqlite3.connect(":memory:" if dry else state)
        if dry and Path(state).exists():
            src = sqlite3.connect(Path(state).resolve().as_uri() + "?mode=ro", uri=True)
            try:
                src.backup(self.db)
            finally:
                src.close()
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        if not dry:
            self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA cache_size=-1024")

    def close(self):
        self.db.close()

    def meta(self, key, default=None):
        r = self.db.execute("SELECT v FROM meta WHERE k=?", (key,)).fetchone()
        return r[0] if r else default

    def setmeta(self, key, value):
        self.db.execute("INSERT OR REPLACE INTO meta VALUES(?,?)", (key, str(value)))

    def path(self, rel):
        p = self.root / rel
        if not p.resolve().is_relative_to(self.root.resolve()):
            raise ValueError("路径越出 Murmur 目录")
        return p

    def read(self, rel):
        if rel in self.overlay:
            return self.overlay[rel]
        p = self.path(rel)
        try:
            before = p.lstat()
            if linked(before) or not stat.S_ISREG(before.st_mode):
                raise ValueError("无法同步链接或特殊文件")
            data = p.read_bytes().decode("utf-8")
            stamp = signature(before)
            if stamp != signature(p.stat()):
                raise Changed(rel)
            return data, stamp
        except FileNotFoundError:
            return None, None

    def write(self, rel, content, expected):
        if self.dry:
            self.overlay[rel] = (content, "preview" if content is not None else None)
            self.plan.append(("remove " if content is None else "write ") + rel)
            return self.overlay[rel][1]
        p = self.path(rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = None
        try:
            if content is not None:
                with tempfile.NamedTemporaryFile(dir=p.parent, delete=False) as f:
                    tmp = Path(f.name)
                    f.write(content.encode("utf-8"))
                    f.flush()
                    os.fsync(f.fileno())
            if expected is None:
                # Both operations fail if an editor has just created this path.
                try:
                    if os.name == "nt":
                        os.rename(tmp, p)
                    else:
                        os.link(tmp, p)
                except FileExistsError:
                    raise Changed(rel) from None
            else:
                if signature(p.stat()) != expected:
                    raise Changed(rel)
                if os.name == "nt" or content is None:
                    self.replace_with_backup(p, tmp, expected)
                else:
                    os.replace(tmp, p)
            self.writes += 1
            if content is not None:
                actual, stamp = self.read(rel)
                if actual != content:
                    raise Changed(rel)
                return stamp
        finally:
            if tmp:
                tmp.unlink(missing_ok=True)

    def replace_with_backup(self, path, tmp, expected):
        # ReplaceFileW saves the actual file displaced by the atomic replace.
        # An editor save in the stat/replace window remains recoverable.
        recovery = "Conflicts/" + uuid.uuid4().hex + "-" + path.name[-80:]
        backup = self.path(recovery)
        backup.parent.mkdir(exist_ok=True)
        if tmp is None:
            os.rename(path, backup)
        else:
            import ctypes
            from ctypes import wintypes
            replace = ctypes.WinDLL("kernel32", use_last_error=True).ReplaceFileW
            replace.argtypes = [wintypes.LPCWSTR]*3 + [wintypes.DWORD, wintypes.LPVOID, wintypes.LPVOID]
            if not replace(str(path), str(tmp), str(backup), 0, None, None):
                raise ctypes.WinError(ctypes.get_last_error())
        displaced = signature(backup.stat())
        # POSIX rename changes ctime, Windows reports creation time here.
        def without_ctime(stamp):
            mtime, _, size, inode = stamp.split(":")
            return mtime, size, inode
        if displaced == expected or (os.name != "nt" and without_ctime(displaced) == without_ctime(expected)):
            backup.unlink()
        else:
            self.db.execute("INSERT OR IGNORE INTO conflicts VALUES(?)", (recovery,))

    def ensure_root(self):
        saved = self.meta("vault")
        bound = str(self.root.resolve())
        if self.root.is_symlink() or saved and saved != bound:
            raise ValueError("状态文件绑定了其他目录")
        marker = ".murmur-cache"
        text, stamp = self.read(marker)
        key = self.meta("cache")
        if key and key != text:
            raise ValueError("Murmur 缓存不可用，已暂停同步")
        if not key:
            if text is None:
                text = uuid.uuid4().hex
                self.write(marker, text, stamp)
            self.setmeta("cache", text)
            self.setmeta("vault", bound)
        for folder in FOLDERS.values():
            if not self.dry:
                self.path(folder).mkdir(exist_ok=True)
        for f in ASSETS.iterdir():
            target = "Templates/" + f.name if f.name in ("task.md", "memo.md") else f.name
            if not self.path(target).exists() and target not in self.overlay:
                content = f.read_text(encoding="utf-8").replace("{{ROOT}}", self.prefix)
                self.write(target, content, None)

    def get(self, uid):
        return self.db.execute("SELECT * FROM files WHERE uid=?", (uid,)).fetchone()

    def scan_file(self, rel, stamp, seen, removed, refresh_memos):
        known = self.db.execute("SELECT * FROM files WHERE path=?", (rel,)).fetchone()
        # Windows directory enumeration returns zero for st_ino. Use the
        # cached directory metadata here and the full file identity for writes.
        unchanged = known and (known["stamp"] == stamp or stamp.endswith(":0") and
                               known["stamp"].rsplit(":", 1)[0] == stamp.rsplit(":", 1)[0])
        if unchanged and not known["missing"] and not (refresh_memos and known["kind"] == "memo"):
            if known["uid"] in seen:
                raise ValueError("重复 ID")
            seen.add(known["uid"])
            return
        text, stamp = self.read(rel)
        if text is None:
            raise Changed(rel)
        note = Note(text)
        uid = note.props.get("murmur_id") or note.props.get("murmur_client_id")
        row = self.get(uid)
        folder = rel.partition("/")[0]
        kind = note.props.get("murmur_type") or next((k for k,v in FOLDERS.items() if v == folder), None)
        if not kind and not uid:
            return
        if kind not in FOLDERS or (row and kind != row["kind"]) or (known and uid != known["uid"]):
            raise ValueError("记录 ID 或类型与原文件不一致")
        if (row and row["path"] != rel and self.path(row["path"]).exists()
                and not os.path.samefile(self.path(row["path"]), self.path(rel))):
            raise ValueError("多个文件使用相同 ID")
        if uid in seen:
            raise ValueError("重复 ID")
        base = json.loads(row["base"]) if row and row["base"] else None
        try:
            values = note.values(kind, base)
        except ValueError:
            previous = json.loads(row["pending"]) if row and row["pending"] else record_values(base)
            if kind != "task" or not previous:
                raise
            # Hearth removes the checkbox, its contiguous indented children
            # and one blank line. Only accept the expected remaining detail.
            details = previous["details"] or ""
            lines = details.split("\n")
            start = 0
            while start < len(lines) and lines[start].strip() and lines[start][:1].isspace():
                start += 1
            if note.body.strip("\n") not in (details.strip("\n"), "\n".join(lines[start:]).strip("\n")):
                raise
            removed[uid] = (rel, stamp, note.render({}, body("task", previous)))
            seen.add(uid)
            return
        props = {}
        if not uid:
            uid = uuid.uuid4().hex
            props.update(murmur_type=kind, murmur_client_id=uid)
        if kind == "memo":
            preview = memo_preview(values["body"])
            if note.props.get("murmur_preview") != preview:
                props["murmur_preview"] = json.dumps(preview, ensure_ascii=False)
            source_link = re.fullmatch(r"\[\[(rec-[0-9a-f]{20})\]\]", note.props.get("recording", ""))
            if source_link:
                props["recording"] = dump(f"[[{source_link[1]}|录音原文]]")
        if props:
            stamp = self.write(rel, note.render(props), stamp)
        elif kind == "task" and "\r\n" in text:
            # Hearth 3.9 splits on LF. A remaining CR prevents its checkbox
            # regex from recognising a Windows-editor task line.
            stamp = self.write(rel, text.replace("\r\n", "\n"), stamp)
        seen.add(uid)
        pending = None if values == record_values(base) else dump(values)
        self.db.execute("""INSERT INTO files(uid,kind,path,stamp,pending) VALUES(?,?,?,?,?)
            ON CONFLICT(uid) DO UPDATE SET path=excluded.path,stamp=excluded.stamp,
            pending=excluded.pending,missing=NULL""", (uid,kind,rel,stamp,pending))
        self.today(uid, kind, values, base)

    def today(self, uid, kind, values, base):
        if kind == "task":
            when = None
            if not values["done"]:
                dates = [v[:10] for v in (values["planned_at"], values["due_at"]) if v]
                when = "" if (base or {}).get("promoted") else min(dates, default=None)
            self.db.execute("UPDATE files SET today=?,title=? WHERE uid=?", (when, values["title"], uid))

    def note_entries(self):
        directories = [(self.root, "")]
        while directories:
            folder, prefix = directories.pop()
            with os.scandir(folder) as entries:
                for e in entries:
                    rel = prefix + e.name
                    if e.name.startswith(".") or linked(e.stat(follow_symlinks=False)):
                        continue
                    if e.is_dir(follow_symlinks=False):
                        if rel not in ("Recordings", "Conflicts", "Archive", "Templates"):
                            directories.append((e.path, rel+"/"))
                    elif e.name.lower().endswith(".md") and rel not in ("README.md","Dashboard.md","Today.md"):
                        yield rel, signature(e.stat())

    def scan(self, confirm_deletions=False):
        seen, errors, removed = set(), {}, {}
        refresh_memos = self.meta("memo_previews") != "2"
        self.db.execute("DELETE FROM ambiguous")
        if self.root.exists():
            for rel, stamp in self.note_entries():
                try:
                    self.scan_file(rel, stamp, seen, removed, refresh_memos)
                except (UnicodeError, ValueError) as err:
                    errors[rel] = str(err)
                    # A copied ID blocks both copies, while other records can
                    # still sync. A malformed file alone has no second identity.
                    try:
                        props = Note(self.read(rel)[0] or "").props
                    except (UnicodeError, ValueError):
                        continue
                    uid = props.get("murmur_id") or props.get("murmur_client_id")
                    if uid:
                        self.db.execute("INSERT OR IGNORE INTO ambiguous VALUES(?)", (uid,))
        self.db.execute("DELETE FROM errors")
        if refresh_memos:
            self.setmeta("memo_previews", 2)
        self.db.executemany("INSERT INTO errors VALUES(?,?)", errors.items())
        now = time.time()
        timers = dict(self.db.execute("SELECT uid,since FROM removed_task_lines"))
        for uid in timers.keys() - removed.keys():
            self.db.execute("DELETE FROM removed_task_lines WHERE uid=?", (uid,))
        for uid, (rel, stamp, original) in removed.items():
            if uid not in timers:
                self.db.execute("INSERT INTO removed_task_lines VALUES(?,?)", (uid, now))
            elif not errors and (confirm_deletions or now - timers[uid] >= self.grace):
                # Keep a restorable task before removing the residual note.
                self.archive(rel, original)
                self.write(rel, None, stamp)
                self.db.execute("UPDATE files SET path=?,stamp=?,missing=?,today=NULL,pending='null' WHERE uid=?",
                                (rel, stamp, timers[uid], uid))
                self.db.execute("DELETE FROM removed_task_lines WHERE uid=?", (uid,))
        # A malformed or duplicate note might be a moved file. Do not infer a
        # deletion until the managed tree has been enumerated successfully.
        if errors:
            return
        for row in self.db.execute("SELECT uid,kind,path,missing,stamp FROM files"):
            if row["uid"] in seen:
                continue
            if row["missing"] is None:
                self.db.execute("UPDATE files SET missing=?,today=NULL,pending=NULL WHERE uid=?", (now,row["uid"]))
            else:
                expired = now - row["missing"] >= self.grace
                if not expired and not (confirm_deletions and row["kind"] == "memo" and os.name == "nt"):
                    continue
                trashed = False
                if os.name == "nt" and row["stamp"] != "preview":
                    from .windows import moved_file
                    target = moved_file(self.path(".murmur-cache"), int(row["stamp"].rsplit(":", 1)[1]))
                    trashed = target is not None and (target.is_relative_to(self.vault / ".trash")
                              or target.is_relative_to(Path(target.anchor) / "$Recycle.Bin"))
                    if target and not trashed:
                        self.db.execute("INSERT OR REPLACE INTO errors VALUES(?,?)",
                                        (row["path"], MOVED_WARNING))
                        self.db.execute("UPDATE files SET pending=NULL WHERE uid=?", (row["uid"],))
                        continue
                if expired or trashed:
                    self.db.execute("UPDATE files SET pending='null' WHERE uid=?", (row["uid"],))

    def archive(self, rel, content, conflict=False):
        folder = "Conflicts" if conflict else "Archive"
        digest = hashlib.sha256((rel+"\0"+content).encode()).hexdigest()[:16]
        path = f"{folder}/{Path(rel).stem}-{digest}.md"
        if self.read(path)[0] is None:
            self.write(path, content, None)
        if conflict:
            self.db.execute("INSERT OR IGNORE INTO conflicts VALUES(?)", (path,))
        return path

    def apply(self, remote, sent=None):
        uid, kind = remote["uid"], remote["kind"]
        row = self.get(uid)
        base = json.loads(row["base"]) if row and row["base"] else None
        if base and remote["rev"] <= base["rev"]:
            return
        if not row and remote["deleted"]:
            return
        if row:
            rel = row["path"]
        elif kind == "memo":
            title = memo_title(remote["body"], remote.get("title"))
            rel = f"Memos/{title}.md"
            number = 1
            while self.path(rel).exists() or rel in self.overlay or self.db.execute(
                    "SELECT 1 FROM files WHERE path=? COLLATE NOCASE", (rel,)).fetchone():
                number += 1
                rel = f"Memos/{title} ({number}).md"
        else:
            label = remote["title"].strip().splitlines()[0][:36]
            label = re.sub(r'[\x00-\x1f<>:"/\\|?*#^\[\]]', " ", label).strip(". ")
            rel = f"{FOLDERS[kind]}/{label or kind} - {uid}.md"
        text, stamp = self.read(rel)
        if row and stamp != row["stamp"] and not (text is None and row["missing"] is not None):
            raise Changed(rel)
        if not row and text is not None:
            raise Changed(rel)
        note = Note(text or "")
        new = record_values(remote)
        old = record_values(base) if sent is None else sent
        local = note.values(kind, base) if text is not None else None
        merged = new
        if row and local != old and local != new:
            if local is not None and new is not None and old is not None:
                merged = dict(new)
                for k in new:
                    if local[k] == old[k]:
                        continue
                    if new[k] not in (local[k], old[k]):
                        self.archive(rel, text, True)
                        merged = new
                        break
                    merged[k] = local[k]
            elif new == old:
                merged = local
            else:
                self.archive(rel, text if text is not None else "本地已删除 " + rel + "\n", True)
        if merged is None:
            if text is not None:
                self.archive(rel, text)
                self.write(rel, None, stamp)
            if new is None:
                self.db.execute("DELETE FROM files WHERE uid=?", (uid,))
            else:
                self.db.execute("UPDATE files SET base=? WHERE uid=?", (dump(remote),uid))
            return
        props = {"murmur_id":uid, "murmur_type":kind,
                 "recording_id":json.dumps(remote["recording_id"], ensure_ascii=False) if remote["recording_id"] else None,
                 "recorded_at":remote["recorded_at"]}
        if kind == "memo":
            props["show_on_device"] = str(merged["show_on_device"]).lower()
            props["murmur_preview"] = json.dumps(memo_preview(merged["body"]), ensure_ascii=False)
        if source := remote["recording_id"]:
            name = "rec-" + hashlib.sha256(source.encode()).hexdigest()[:20]
            target = "Recordings/" + name + ".md"
            if not self.path(target).exists() and target not in self.overlay:
                transcript = self.api("recording", {"name": source})["transcript"]
                self.write(target, f"# {source}\n\n录音时间　{remote['recorded_at'] or '未知'}\n\n"
                           + (transcript or "") + "\n", None)
            props["recording"] = dump(f"[[{name}|录音原文]]" if kind == "memo" else f"[[{name}]]")
        rendered = note.render(props, None if local == merged else body(kind, merged, note.body))
        if text is None or rendered != text.replace("\r\n", "\n"):
            stamp = self.write(rel, rendered, stamp)
        self.db.execute("""INSERT OR REPLACE INTO files(uid,kind,path,stamp,base,pending)
                        VALUES(?,?,?,?,?,?)""", (uid,kind,rel,stamp,dump(remote),dump(merged) if merged!=new else None))
        self.today(uid, kind, merged, remote)

    def pull(self):
        while True:
            cursor = int(self.meta("cursor", 0))
            page = self.api("changes", {"after":cursor})
            if page["server"] != self.meta("server", page["server"]) or page["head"] < cursor:
                raise ValueError("树莓派数据库已替换或回滚。请保留 Vault 和状态目录，核对备份后重新绑定。")
            if self.meta("server") is None:
                with self.db:
                    self.setmeta("server", page["server"])
            items = page["items"]
            if not items:
                break
            with self.db:
                self.db.executemany("INSERT OR REPLACE INTO inbox VALUES(?,?)", ((r["uid"],dump(r)) for r in items))
                self.setmeta("cursor", items[-1]["rev"])
        for uid, data in self.db.execute("SELECT uid,data FROM inbox"):
            row = self.get(uid)
            if row and (self.blocked(row) or row["missing"] is not None and row["pending"] is None):
                continue
            with self.db:
                try:
                    self.apply(json.loads(data))
                except (Changed, UnicodeError, ValueError) as err:
                    self.db.execute("INSERT OR REPLACE INTO errors VALUES(?,?)",
                                    (row["path"] if row else uid, str(err)))
                else:
                    self.db.execute("DELETE FROM inbox WHERE uid=?", (uid,))

    def blocked(self, row):
        if self.db.execute("SELECT 1 FROM removed_task_lines WHERE uid=?", (row["uid"],)).fetchone():
            return True
        # Ambiguous identities must never publish either duplicate's edits.
        if not self.db.execute("SELECT 1 FROM errors LIMIT 1").fetchone():
            return False
        if row["missing"] is not None and self.db.execute(
                "SELECT 1 FROM errors WHERE reason<>? LIMIT 1", (MOVED_WARNING,)).fetchone():
            return True
        return self.db.execute("SELECT 1 FROM errors WHERE path=? UNION ALL SELECT 1 FROM ambiguous WHERE uid=? LIMIT 1",
                               (row["path"],row["uid"])).fetchone()

    def push(self):
        batch = []
        size = 0
        for row in self.db.execute("SELECT * FROM files WHERE pending IS NOT NULL ORDER BY uid"):
            if self.blocked(row):
                continue
            base = json.loads(row["base"]) if row["base"] else {}
            values = json.loads(row["pending"])
            if self.dry:
                self.plan.append(("delete " if values is None else "upload ") + row["path"])
                continue
            item = {"kind":row["kind"], "uid":row["uid"], "base":base.get("rev",0), "values":values}
            length = len(dump(item).encode()) + 1
            if length > 250_000:
                with self.db:
                    self.db.execute("INSERT OR REPLACE INTO errors VALUES(?,?)", (row["path"], "单条编辑超过 250 KB"))
                continue
            if len(batch) == 32 or size + length > 250_000:
                self.send_batch(batch)
                batch, size = [], 0
            batch.append((row["path"], item))
            size += length
        if batch:
            self.send_batch(batch)

    def send_batch(self, batch):
        items = [item for _, item in batch]
        results = self.api("edit", {"edits": items})["results"]
        if len(results) != len(items):
            raise ValueError("incomplete edit response")
        with self.db:
            for (path, item), result in zip(batch, results):
                if "error" in result:
                    self.db.execute("INSERT OR REPLACE INTO errors VALUES(?,?)", (path, result["error"]))
                else:
                    self.apply(result["record"], None if result.get("conflict") else item["values"])

    def dashboard(self):
        lines = ["# 今天的任务", ""]
        today = datetime.now(timezone(timedelta(hours=8))).date().isoformat()
        for title, rel in self.db.execute("SELECT title,path FROM files WHERE today<=? ORDER BY today,title,uid",
                                         (today,)):
            link = self.prefix + "/" + rel[:-3]
            lines.append(f"- [[{link}|{title.replace('|', '｜')}]]")
        content = "\n".join(lines) + "\n"
        prev, stamp = self.read("Today.md")
        if prev != content:
            self.write("Today.md", content, stamp)

    def cycle(self, manual=False):
        self.writes = 0
        self.plan.clear()
        with self.db:
            self.db.execute("BEGIN")
            self.ensure_root()
            self.scan()
            if manual and self.db.execute("""SELECT 1 FROM removed_task_lines UNION ALL
                    SELECT 1 FROM files WHERE kind='memo' AND missing IS NOT NULL LIMIT 1""").fetchone():
                # Recheck the tree before confirming a removed task line or a recycled memo.
                self.scan(confirm_deletions=True)
            conflict_dir = self.path("Conflicts")
            if conflict_dir.is_dir():
                for file in conflict_dir.glob("*.md"):
                    self.db.execute("INSERT OR IGNORE INTO conflicts VALUES(?)", ("Conflicts/" + file.name,))
            for (path,) in self.db.execute("SELECT path FROM conflicts"):
                if not self.path(path).exists():
                    self.db.execute("DELETE FROM conflicts WHERE path=?", (path,))
        try:
            self.pull()
            self.push()
        finally:
            self.dashboard()
        with self.db:
            self.setmeta("last_sync", time.strftime("%Y-%m-%d %H:%M:%S"))
            self.setmeta("error", "")
