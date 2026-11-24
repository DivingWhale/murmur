import argparse
from contextlib import contextmanager
import hashlib
import http.client
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import sqlite3
import sys

from .sync import Bridge
from .transport import API
from .manual import ManualRequests


@contextmanager
def single_writer(path):
    with path.open("a+b") as lock:
        lock.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def status(db):
    def rows(sql):
        return [dict(row) for row in db.execute(sql)]
    pending = "SELECT count(*) FROM files WHERE pending IS NOT NULL OR missing IS NOT NULL"
    # --status opens older state databases read-only, before the first updated sync.
    if db.execute("SELECT 1 FROM sqlite_master WHERE name='removed_task_lines'").fetchone():
        pending += " OR uid IN (SELECT uid FROM removed_task_lines)"
    return {"last_sync":dict(db.execute("SELECT k,v FROM meta")).get("last_sync"),
            "error":dict(db.execute("SELECT k,v FROM meta")).get("error"),
            "pending":db.execute(pending).fetchone()[0],
            "incoming":db.execute("SELECT count(*) FROM inbox").fetchone()[0],
            "errors":rows("SELECT * FROM errors"), "conflicts":rows("SELECT * FROM conflicts")}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Murmur Obsidian 同步服务")
    parser.add_argument("--config", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    for action in ("once","daemon","status","dry-run"):
        modes.add_argument("--" + action, action="store_true")
    args = parser.parse_args(argv)
    cfg = json.loads(args.config.read_text(encoding="utf-8-sig"))
    vault = Path(cfg["vault"]).expanduser().resolve(strict=True)
    root = (vault / cfg.get("data_dir", "Murmur")).resolve()
    if not vault.is_dir() or vault not in root.parents:
        raise ValueError("data_dir 必须是 Vault 的子目录")
    profile = hashlib.sha256((str(root) + cfg["api_url"]).encode()).hexdigest()[:16]
    folder = Path(cfg.get("state_dir", Path(os.getenv("LOCALAPPDATA") or Path.home()/".local/state")
                          / "Murmur" / profile)).expanduser().resolve()
    if folder.is_relative_to(vault):
        raise ValueError("state_dir 应位于 Vault 外")
    state = folder / "state.sqlite"
    if args.status:
        if not state.exists():
            print("尚未同步")
            return 0
        from contextlib import closing
        with closing(sqlite3.connect(state.as_uri()+"?mode=ro", uri=True)) as db:
            db.row_factory = sqlite3.Row
            print(json.dumps(status(db), ensure_ascii=False, indent=2))
        return 0
    token = (Path(cfg["token_file"]).read_text(encoding="utf-8-sig").strip() if cfg.get("token_file")
             else os.environ[cfg.get("token_env","MURMUR_PC_TOKEN")])
    if not token:
        raise ValueError("缺少 PC 凭据")
    interval = max(10, int(cfg.get("interval", 60)))
    timeout = max(1, int(cfg.get("timeout", 15)))
    api = API(cfg["api_url"], token, cfg.get("ca_file"), timeout)
    if args.dry_run:
        from contextlib import nullcontext
        lock = nullcontext()
    else:
        folder.mkdir(parents=True, exist_ok=True)
        lock = single_writer(folder / "writer.lock")
        handler = RotatingFileHandler(folder/"bridge.log", maxBytes=256*1024, backupCount=2, encoding="utf-8")
        logging.basicConfig(level=cfg.get("log_level","INFO"), handlers=[handler])
    try:
        with lock:
            bridge = Bridge(root, state, api, max(0,int(cfg.get("delete_grace",120))), args.dry_run, vault)
            try:
                delay = interval
                manual = ManualRequests(vault) if args.daemon else None
                requests = manual.wait(0) if manual else []
                while True:
                    try:
                        bridge.cycle(manual=bool(requests))
                        if args.dry_run:
                            for item in bridge.plan:
                                print(item)
                        if not args.daemon:
                            report = status(bridge.db)
                            print(json.dumps(report, ensure_ascii=False))
                            return int(bool(report["errors"] or report["conflicts"]))
                        delay = interval
                        if requests:
                            report = status(bridge.db)
                            if report["errors"] or report["conflicts"]:
                                message = "部分记录需要处理，请检查 Murmur 的同步状态或 Conflicts 文件夹。"
                            elif report["pending"] or report["incoming"]:
                                message = "仍有待处理的修改。近期删除的文件会在宽限期后提交。"
                            else:
                                message = "已同步到树莓派，掌机下次同步时会获取最新内容。"
                    except (OSError, ValueError, sqlite3.Error, http.client.HTTPException) as err:
                        message = f"{type(err).__name__} {err}"
                        with bridge.db:
                            bridge.setmeta("error", message)
                        logging.error("%s", message)
                        if not args.daemon:
                            print(message, file=sys.stderr)
                            return 1
                        delay = min(max(900,interval), delay * 2)
                        message = "本次未能完成同步，电脑后台会自动重试。"
                    if requests:
                        manual.reply(requests, message)
                    requests = manual.wait(delay)
            finally:
                bridge.close()
    finally:
        api.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
    except (ValueError, OSError, KeyError, sqlite3.Error) as exc:
        sys.exit(str(exc))
