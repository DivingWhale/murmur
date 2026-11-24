"""Windows acceptance benchmark. Run python -m pc_bridge.benchmark --openssl <path>."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class Counters(ctypes.Structure):
    _fields_ = [("cb",wintypes.DWORD),("faults",wintypes.DWORD)] + [
        (field,ctypes.c_size_t) for field in ("peak","rss","qpp","qp","qnp","qn","page","ppeak")]


def working_set():
    c = Counters()
    c.cb = ctypes.sizeof(c)
    if not ctypes.windll.psapi.GetProcessMemoryInfo(wintypes.HANDLE(-1),
                                                    ctypes.byref(c),c.cb):
        raise ctypes.WinError()
    return [round(x / 1048576, 2) for x in (c.rss,c.peak)]


def client(args):
    from .sync import Bridge
    from .transport import API
    root = Path(args.temp)
    api = API(args.url,"benchmark-token",root/"cert.pem")
    bridge = Bridge(root/"Murmur",root/"state.sqlite",api)
    try:
        t = time.perf_counter()
        bridge.cycle()
        if args.client == "seed":
            print("initial_seconds",round(time.perf_counter()-t,2),flush=True)
            return
        print("incremental_seconds",round(time.perf_counter()-t,2),"rss_peak_MiB",working_set(),
              "markdown_writes",bridge.writes,flush=True)
        t = time.perf_counter()
        start_cpu = time.process_time()
        samples, writes = [], 0
        while time.perf_counter()-t < args.idle:
            time.sleep(min(10,args.idle))
            bridge.cycle()
            samples.append(working_set()[0])
            writes += bridge.writes
        print(json.dumps(dict(idle_seconds=round(time.perf_counter()-t,1),
              idle_cpu_percent=round(100*(time.process_time()-start_cpu)/(time.perf_counter()-t),3),
              idle_writes=writes,rss_samples_MiB=samples)),flush=True)
    finally:
        bridge.close()
        api.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--openssl",default="openssl")
    p.add_argument("--idle",type=int,default=300)
    p.add_argument("--client",choices=["seed","increment"])
    p.add_argument("--url")
    p.add_argument("--temp")
    args = p.parse_args()
    if args.client:
        client(args)
        return
    import ssl
    import threading
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"backend"))
    import server
    import bridge_api
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        server.DATA = folder/"pi"
        server.initialize()
        with server.database() as db:
            db.executemany("INSERT INTO tasks(title,details) VALUES(?,'')", [(f"task {i}",) for i in range(1000)])
            db.executemany("INSERT INTO notes(body) VALUES(?)",[(f"memo {i}",) for i in range(2000)])
        subprocess.run([args.openssl,"req","-x509","-newkey","rsa:2048","-nodes","-days","1",
                        "-subj","/CN=localhost","-addext","subjectAltName=IP:127.0.0.1",
                        "-out","cert.pem","-keyout","key.pem"],cwd=tmp,check=True,stderr=subprocess.DEVNULL)
        with server.Server(("127.0.0.1",0),bridge_api.Handler) as srv:
            srv.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            srv.tls.load_cert_chain(folder/"cert.pem",folder/"key.pem")
            srv.database = server.database
            srv.authorization = b"Bearer benchmark-token"
            threading.Thread(target=srv.serve_forever,daemon=True).start()
            cmd = [sys.executable,"-m","pc_bridge.benchmark","--temp",tmp,"--url",
                   f"https://127.0.0.1:{srv.server_port}","--idle",str(args.idle),"--client"]
            try:
                subprocess.run(cmd+["seed"],check=True)
                import itertools
                for f in itertools.islice((folder/"Murmur/Tasks").glob("*.md"),100):
                    f.write_text(f.read_text(encoding="utf-8").replace("- [ ]","- [x]"),encoding="utf-8",newline="\n")
                subprocess.run(cmd+["increment"],check=True)
                with server.database() as db:
                    assert db.execute("SELECT count(*) FROM tasks WHERE done").fetchone()[0] == 100
            finally:
                srv.shutdown()


if __name__ == "__main__":
    main()
