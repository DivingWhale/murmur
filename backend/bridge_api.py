"""PC change feed over the same tasks and notes used by the handheld."""
import hmac
import json
import re
from datetime import date, datetime
from http.server import BaseHTTPRequestHandler

TABLE = {"task": "tasks", "memo": "notes"}
FIELDS = {"task": ("title", "details", "planned_at", "due_at", "done"),
          "memo": ("body", "show_on_device")}
PAGE = 64
UID = re.compile("[0-9a-f]{32}")


def migrate(db):
    # The original INTEGER PRIMARY KEY can reuse a deleted device ID. Preserve
    # every existing ID and make source nullable for genuinely manual records.
    db.execute("BEGIN IMMEDIATE")
    db.execute("CREATE TABLE IF NOT EXISTS bridge_identity(singleton INTEGER PRIMARY KEY, token TEXT)")
    db.execute("INSERT OR IGNORE INTO bridge_identity VALUES(1,lower(hex(randomblob(16))))")
    for kind, table in TABLE.items():
        if "uid" in {r[1] for r in db.execute(f"PRAGMA table_info({table})")}:
            continue
        extra = ("title TEXT, details TEXT, planned_at TEXT, due_at TEXT, done INTEGER DEFAULT 0, "
                 "promoted INTEGER DEFAULT 0" if kind == "task" else
                 "body TEXT, show_on_device INTEGER NOT NULL DEFAULT 0")
        db.execute(f"ALTER TABLE {table} RENAME TO bridge_old")
        db.execute(f"""CREATE TABLE {table}(id INTEGER PRIMARY KEY AUTOINCREMENT,
                   uid TEXT NOT NULL UNIQUE DEFAULT (lower(hex(randomblob(16)))), source TEXT, {extra})""")
        cols = ",".join(r[1] for r in db.execute("PRAGMA table_info(bridge_old)"))
        db.execute(f"INSERT INTO {table}({cols}) SELECT {cols} FROM bridge_old")
        db.execute("DROP TABLE bridge_old")
    defaults = {r[1]: r[4] for r in db.execute("PRAGMA table_info(notes)")}
    if defaults["show_on_device"] != "0":
        # SQLite cannot change a column default in place. Keep each saved selection.
        db.execute("ALTER TABLE notes ADD COLUMN selected INTEGER NOT NULL DEFAULT 0")
        db.execute("UPDATE notes SET selected=show_on_device WHERE show_on_device<>0")
        db.execute("ALTER TABLE notes DROP COLUMN show_on_device")
        db.execute("ALTER TABLE notes RENAME COLUMN selected TO show_on_device")
    if "title" not in defaults:
        db.execute("ALTER TABLE notes ADD COLUMN title TEXT")
    db.execute("""CREATE TABLE IF NOT EXISTS bridge_changes(
        rev INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, uid TEXT UNIQUE NOT NULL,
        deleted INTEGER NOT NULL)""")
    for kind, table in TABLE.items():
        db.execute(f"""INSERT OR IGNORE INTO bridge_changes(kind,uid,deleted)
                       SELECT ?,t.uid,0 FROM {table} t WHERE NOT EXISTS
                       (SELECT 1 FROM bridge_changes c WHERE c.uid=t.uid)""", (kind,))
        for event in ("INSERT", "UPDATE", "DELETE"):
            ref = "old" if event == "DELETE" else "new"
            db.execute(f"""CREATE TRIGGER IF NOT EXISTS bridge_{table}_{event}
                AFTER {event} ON {table} BEGIN
                INSERT OR REPLACE INTO bridge_changes(kind,uid,deleted)
                VALUES('{kind}',{ref}.uid,{int(event == 'DELETE')}); END""")


def record(db, change):
    result = dict(change)
    kind = result["kind"]
    if not result["deleted"]:
        fields = ",".join("t." + f for f in FIELDS[kind])
        metadata = "t.promoted" if kind == "task" else "t.title"
        r = db.execute(f"""SELECT {fields},{metadata},t.source AS recording_id,r.recorded_at
                           FROM {TABLE[kind]} t LEFT JOIN recordings r ON r.name=t.source
                           WHERE t.uid=?""", (result["uid"],)).fetchone()
        result.update(dict(r))
        if kind == "task":
            result["details"] = result["details"] or ""
        flag = "done" if kind == "task" else "show_on_device"
        result[flag] = bool(result[flag])
    return result


def validate(kind, values):
    if not isinstance(values, dict) or values.keys() != set(FIELDS[kind]):
        raise ValueError("invalid fields")
    for field, value in values.items():
        if field in ("done", "show_on_device"):
            if type(value) is not bool:
                raise ValueError("invalid checkbox")
        elif field in ("planned_at", "due_at"):
            if value is not None:
                (date if len(value) == 10 else datetime).fromisoformat(value)
        elif not isinstance(value, str) or "\x00" in value or (field != "details" and not value.strip()):
            raise ValueError("invalid text")
    if kind == "task" and len(values["title"].splitlines()) != 1:
        raise ValueError("multiline title")


def dispatch(db, path, arg):
    if not isinstance(arg, dict):
        raise ValueError("expected object")
    if path == "/v1/pc/recording":
        row = db.execute("SELECT transcript FROM recordings WHERE name=?", (arg["name"],)).fetchone()
        return {"transcript": row[0] if row else None}
    if path == "/v1/pc/changes":
        after = arg["after"]
        if type(after) != int or after < 0:
            raise ValueError("invalid cursor")
        db.execute("BEGIN")
        rows = db.execute("SELECT * FROM bridge_changes WHERE rev>? ORDER BY rev LIMIT ?", (after, PAGE))
        return {"items": [record(db, row) for row in rows],
                "server": db.execute("SELECT token FROM bridge_identity WHERE singleton=1").fetchone()[0],
                "head": db.execute("SELECT coalesce(max(rev),0) FROM bridge_changes").fetchone()[0]}
    if path != "/v1/pc/edit":
        raise ValueError("unknown endpoint")
    db.execute("BEGIN IMMEDIATE")
    if "edits" not in arg:
        return edit(db, arg)
    edits = arg["edits"]
    if not isinstance(edits, list) or not 1 <= len(edits) <= 32:
        raise ValueError("invalid batch")
    results = []
    for item in edits:
        try:
            results.append(edit(db, item))
        except (TypeError, ValueError, KeyError, OverflowError):
            # Validation precedes the single SQL write for each edit.
            results.append({"error": "invalid record"})
    return {"results": results}


def edit(db, arg):
    kind, uid, base, values = (arg[k] for k in ("kind", "uid", "base", "values"))
    if kind not in TABLE or not UID.fullmatch(uid) or type(base) != int or base < 0:
        raise ValueError("invalid identity or revision")
    if values is not None:
        validate(kind, values)
    def current():
        row = db.execute("SELECT * FROM bridge_changes WHERE uid=?", (uid,)).fetchone()
        return record(db, row) if row else None
    old = current()
    if old and old["kind"] != kind:
        raise ValueError("identity type mismatch")
    table = TABLE[kind]
    if old and (values is None and old["deleted"] or values is not None and not old["deleted"]
                and all(old[k] == v for k, v in values.items())):
        return {"record": old}
    if (old["rev"] if old else 0) != base or old and old["deleted"]:
        return {"conflict": True, "record": old}
    if values is None:
        if not old:
            return {"record": {"uid": uid, "kind": kind, "deleted": 1, "rev": 0}}
        db.execute(f"DELETE FROM {table} WHERE uid=?", (uid,))
    elif old:
        assignments = ",".join(k + "=?" for k in values)
        db.execute(f"UPDATE {table} SET {assignments} WHERE uid=?", (*values.values(), uid))
    else:
        db.execute(f"INSERT INTO {table}(uid,{','.join(values)}) VALUES({','.join('?' for _ in range(len(values)+1))})",
                   (uid, *values.values()))
    return {"record": current()}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True
    def do_POST(self):
        auth = self.headers.get("Authorization", "").encode()
        if not hmac.compare_digest(auth, self.server.authorization):
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            if not 0 < length <= 256 * 1024:
                raise ValueError("invalid length")
            with self.server.database() as db:
                result = dispatch(db, self.path, json.loads(self.rfile.read(length)))
        except (ValueError, KeyError, TypeError, OverflowError):
            self.send_error(400)
            return
        data = json.dumps(result, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass
