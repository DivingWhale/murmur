"""Local requests from the Obsidian ribbon, consumed by the regular writer."""
import hashlib
import logging
import os
from pathlib import Path
import re
import time


class ManualRequests:
    def __init__(self, vault):
        key = hashlib.sha256(os.path.normcase(str(vault)).encode()).hexdigest()
        local = Path(os.environ.get("LOCALAPPDATA") or Path.home()/".local/state")
        self.folder = local / "Murmur" / "manual-sync" / key

    def wait(self, seconds):
        deadline = time.monotonic() + seconds
        while True:
            requests = [p for p in self.folder.glob("*.request")
                        if re.fullmatch(r"[0-9a-f]{32}\.request", p.name)]
            remaining = deadline - time.monotonic()
            if requests or remaining <= 0:
                return requests
            time.sleep(min(1, remaining))

    def reply(self, requests, message):
        for request in requests:
            try:
                draft = request.with_suffix(".tmp")
                draft.write_text(message, encoding="utf-8")
                draft.replace(request.with_suffix(".result"))
                request.unlink(missing_ok=True)
            except OSError:
                logging.exception("Could not deliver the manual sync result")
        # Obsidian can close between requesting and reading its receipt.
        for receipt in self.folder.glob("*.result"):
            try:
                if time.time() - receipt.stat().st_mtime > 86400:
                    receipt.unlink(missing_ok=True)
            except OSError:
                logging.debug("Could not clean up an old sync result")
