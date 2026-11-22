"""Single-handheld voice inbox. Run with Python 3.13+ and OpenSSL PSK support."""
import json
import logging
import os
import re
import sqlite3
import ssl
import struct
import tempfile
import threading
import time
import wave
from contextlib import contextmanager
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import cloud

DATA = Path(os.environ.get("MURMUR_DATA", "."))
PART = 256 * 1024
NAME = re.compile(r"rec-(unset|\d{8}-\d{6})-[0-9a-f]{8}\.wav")
WAKE = threading.Event()
SCHEMA = """
CREATE TABLE IF NOT EXISTS recordings(
 name TEXT PRIMARY KEY, recorded_at TEXT, size INTEGER NOT NULL, received INTEGER DEFAULT 0,
 state TEXT DEFAULT 'receiving', transcript TEXT, attempts INTEGER DEFAULT 0,
 retry_at REAL DEFAULT 0, error TEXT);
CREATE TABLE IF NOT EXISTS chunks(name TEXT, offset INTEGER, data BLOB, PRIMARY KEY(name,offset));
CREATE TABLE IF NOT EXISTS segments(name TEXT, part INTEGER, text TEXT, PRIMARY KEY(name,part));
CREATE TABLE IF NOT EXISTS tasks(id INTEGER PRIMARY KEY, source TEXT NOT NULL, title TEXT, details TEXT,
 planned_at TEXT, due_at TEXT, done INTEGER DEFAULT 0, promoted INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS notes(id INTEGER PRIMARY KEY, source TEXT NOT NULL, body TEXT);
"""


@contextmanager
def database():
    db = sqlite3.connect(DATA / "murmur.sqlite", timeout=15)
    db.row_factory = sqlite3.Row
    try:
        with db:
            yield db
    finally:
        db.close()


def initialize():
    DATA.mkdir(mode=0o700, parents=True, exist_ok=True)
    with database() as db:
        db.execute("PRAGMA journal_mode=WAL")
        db.executescript(SCHEMA)


def receive(name, offset, total, data):
    match = NAME.fullmatch(name)
    if not match or not (44 < total <= 0xfff00000 + 44 and 0 <= offset < total
                         and 0 < len(data) <= PART and offset + len(data) <= total):
        raise ValueError("invalid recording or range")
    stamp = match[1]
    when = None if stamp == "unset" else datetime.strptime(stamp, "%Y%m%d-%H%M%S").replace(tzinfo=cloud.ZONE).isoformat()
    if offset == 0:
        # The handheld writes a canonical 44-byte PCM header when it finalizes a WAV.
        if len(data) < 44 or data[:4] != b"RIFF" or data[8:16] != b"WAVEfmt " or data[36:40] != b"data":
            raise ValueError("invalid WAV header")
        fmt = struct.unpack_from("<IHHIIHH", data, 16)
        if fmt != (16, 1, 1, 16000, 32000, 2, 16) or struct.unpack_from("<I", data, 40)[0] != total-44 or (total-44)%2:
            raise ValueError("invalid WAV format or length")
    with database() as db:
        db.execute("BEGIN IMMEDIATE")
        db.execute("INSERT OR IGNORE INTO recordings(name,recorded_at,size) VALUES(?,?,?)", (name, when, total))
        row = db.execute("SELECT received,size FROM recordings WHERE name=?", (name,)).fetchone()
        if row["size"] != total:
            raise ValueError("recording ID conflict")
        end = row["received"]
        if offset < end:
            old = db.execute("SELECT data FROM chunks WHERE name=? AND offset=?", (name, offset)).fetchone()
            if not old or old[0] != data:
                raise ValueError("recording content conflict")
        elif offset == end:
            db.execute("INSERT INTO chunks VALUES(?,?,?)", (name, offset, data))
            end += len(data)
            db.execute("UPDATE recordings SET received=?,state=? WHERE name=?",
                       (end, "queued" if end == total else "receiving", name))
    if end == total:
        WAKE.set()
    return {"offset": end, "received": end == total}


def sync(changes):
    if not isinstance(changes, list):
        raise ValueError("expected an array of task changes")
    with database() as db:
        db.execute("BEGIN IMMEDIATE")
        for edit in changes:
            if (not isinstance(edit, list) or len(edit) != 3 or type(edit[0]) is not int
                    or any(type(v) is not int or v not in (0, 1) for v in edit[1:])):
                raise ValueError("invalid task edit")
            db.execute("UPDATE tasks SET done=?, promoted=? WHERE id=?", (edit[1], edit[2], edit[0]))
        fields = "id,title,details,planned_at,due_at,done,promoted"
        tasks = [list(r) for r in db.execute("SELECT " + fields + " FROM tasks ORDER BY id")]
        pending = db.execute("SELECT count(*) FROM recordings WHERE state='queued'").fetchone()[0]
    return {"tasks": tasks, "processing": pending}


def process(row):
    if row["state"] != "queued":
        return
    name = row["name"]
    text = row["transcript"]
    if text is None:
        # Bound memory even for a long offline recording. Persist each ASR result before moving on.
        parts = []
        with tempfile.TemporaryFile() as audio:
            with database() as db:
                for chunk in db.execute("SELECT data FROM chunks WHERE name=? ORDER BY offset", (name,)):
                    audio.write(chunk[0])
            audio.seek(0)
            with wave.open(audio, "rb") as wav:
                while pcm := wav.readframes(16000 * 180):
                    index = len(parts)
                    with database() as db:
                        old = db.execute("SELECT text FROM segments WHERE name=? AND part=?", (name,index)).fetchone()
                    segment = old[0] if old else cloud.transcribe(pcm)
                    if not old:
                        with database() as db:
                            db.execute("INSERT OR IGNORE INTO segments VALUES(?,?,?)", (name,index,segment))
                    parts.append(segment)
        text = "\n".join(parts)
        with database() as db:
            db.execute("UPDATE recordings SET transcript=? WHERE name=?", (text,name))
    tasks, notes = cloud.extract(text, row["recorded_at"])
    with database() as db:
        # Publish tasks, notes and completion together, even across a power loss.
        claimed = db.execute("UPDATE recordings SET state='ready',error=NULL WHERE name=? AND state='queued'",
                             (name,))
        if not claimed.rowcount:
            return
        db.executemany("INSERT INTO tasks(source,title,details,planned_at,due_at) VALUES(?,?,?,?,?)",
                       [(name, *task) for task in tasks])
        db.executemany("INSERT INTO notes(source,body) VALUES(?,?)", [(name,n) for n in notes])


def worker():
    while True:
        WAKE.clear()
        with database() as db:
            row = db.execute("SELECT * FROM recordings WHERE state='queued' AND retry_at<=? ORDER BY rowid LIMIT 1",
                             (time.time(),)).fetchone()
        if not row:
            WAKE.wait(5)
            continue
        try:
            process(row)
        except Exception as err:
            # Keep the recording and retry without blocking unrelated jobs.
            error = type(err).__name__ + " " + str(getattr(err, "code", ""))
            logging.warning("Recording processing deferred (%s)", error)
            delay = min(3600, 10 * 2**min(row["attempts"], 9))
            with database() as db:
                db.execute("UPDATE recordings SET error=?,attempts=attempts+1,retry_at=? WHERE name=?",
                           (error,time.time()+delay,row["name"]))


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        self.connection.settimeout(15)
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            if not 0 <= length <= PART:
                raise ValueError("invalid length")
            data = self.rfile.read(length)
            if len(data) != length:
                raise ValueError("incomplete body")
            if self.path == "/v1/sync":
                result = sync(json.loads(data))
            elif self.path.startswith("/v1/recordings/"):
                result = receive(self.path.removeprefix("/v1/recordings/"),
                                 int(self.headers.get("X-Offset","-1")), int(self.headers.get("X-Size","-1")), data)
            else:
                self.send_error(404)
                return
        except (ValueError, TypeError, IndexError):
            self.send_error(400)
            return
        body = json.dumps(result, ensure_ascii=False, separators=(",",":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


class Server(ThreadingHTTPServer):
    def get_request(self):
        conn, addr = super().get_request()
        conn.settimeout(12)
        return self.tls.wrap_socket(conn, server_side=True, do_handshake_on_connect=False), addr


if __name__ == "__main__":
    os.umask(0o077)
    initialize()
    key = bytes.fromhex(os.environ["MURMUR_PSK"])
    if len(key) != 32 or not os.environ.get("DASHSCOPE_API_KEY"):
        raise ValueError("missing credentials")
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_2
    tls.set_ciphers("PSK-AES128-GCM-SHA256")
    tls.set_psk_server_callback(lambda identity: key if identity == "murmur" else b"")
    with Server(("", int(os.environ.get("MURMUR_PORT","8443"))), Handler) as server:
        server.tls = tls
        threading.Thread(target=worker, daemon=True).start()
        server.serve_forever()
