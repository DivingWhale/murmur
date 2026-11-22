"""Cycle the handheld's campus Wi-Fi over USB serial until it hangs or reboots.

Needs a saved campus profile (see join_campus.py) and the usual TF card.
Each cycle runs one of:
  auto    `wifi auto`: the Settings → Wi-Fi session, which switches itself off
  manual  `wifi connect`, stay online briefly, then `wifi off`
  abort   `wifi connect`, then `wifi off` partway through the login

If the device stops answering, the script waits for a watchdog reboot and
prints `wifi diag`, which says where the firmware was when it hung.
"""
import argparse
import random
import sys
import time

import serial
from serial.tools import list_ports

MURMUR = (0x303A, 0x4003)


class Hang(Exception):
    pass


def find_port(explicit):
    if explicit:
        return explicit
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == MURMUR]
    return ports[0] if len(ports) == 1 else None


class Device:
    def __init__(self, port):
        self.name = port
        self.port = serial.Serial(port, 115200, timeout=5, write_timeout=3)
        self.port.reset_input_buffer()

    def ask(self, line):
        """Send a command and return its one-line reply, or raise Hang."""
        try:
            self.port.reset_input_buffer()
            self.port.write(line.encode() + b"\n")
            self.port.flush()
            reply = self.port.readline().decode("ascii", errors="replace").strip()
        except (serial.SerialException, OSError) as e:
            raise Hang(f"serial error after {line!r}: {e}")
        if not reply:
            raise Hang(f"no reply to {line!r} within 5 s")
        return reply

    def wait_for(self, wanted, timeout, poll=0.5):
        """Poll `wifi status` until a reply starts with one of `wanted`."""
        deadline = time.monotonic() + timeout
        seen = []
        while time.monotonic() < deadline:
            reply = self.ask("wifi status")
            if not seen or seen[-1] != reply:
                seen.append(reply)
            if reply.startswith(wanted):
                return reply, seen
            if reply.startswith(("wifi failed", "wifi error", "wifi profile unavailable", "wifi waiting clock")):
                return reply, seen
            time.sleep(poll)
        return None, seen


def cycle(dev, mode):
    if mode == "auto":
        dev.ask("wifi auto")
        reply, seen = dev.wait_for(("wifi online",), 45)
        if not reply or not reply.startswith("wifi online"):
            return False, seen
        reply, more = dev.wait_for(("wifi off",), 30)
        return bool(reply and reply == "wifi off"), seen + more
    if mode == "manual":
        dev.ask("wifi connect")
        reply, seen = dev.wait_for(("wifi online",), 45)
        if not reply or not reply.startswith("wifi online"):
            return False, seen
        time.sleep(random.uniform(0, 3))
        return dev.ask("wifi off") == "wifi off", seen
    # abort
    dev.ask("wifi connect")
    time.sleep(random.uniform(0.3, 4))
    return dev.ask("wifi off") == "wifi off", []


def after_hang(dev, explicit_port):
    """Wait for a watchdog reboot and collect diagnostics."""
    print("Device stopped answering. Waiting up to 60 s for a watchdog reboot…", flush=True)
    try:
        dev.port.close()
    except Exception:
        pass
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        time.sleep(2)
        port = find_port(explicit_port)
        if not port:
            continue
        try:
            fresh = Device(port)
            time.sleep(1)
            diag = fresh.ask("wifi diag")
            print(diag)
            print("Paste the line above into the PR. Codes are in docs/campus-wifi.md.")
            return 1
        except (Hang, serial.SerialException, OSError):
            continue
    print("No reboot within 60 s: the hang did not trip the loop watchdog.")
    print("Note what the screen shows, then power-cycle the device and run `wifi diag`.")
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port")
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--modes", default="auto,manual,abort", help="Comma-separated list to rotate through.")
    args = parser.parse_args()
    modes = args.modes.split(",")
    port = find_port(args.port)
    if not port:
        sys.exit("Connect one Murmur by USB or specify --port.")
    dev = Device(port)
    try:
        print(dev.ask("wifi diag"), "(from the previous reset)")
        dev.port.write(f"time {int(time.time())}\n".encode())
        if dev.ask("wifi profile") != "wifi saved":
            sys.exit("No saved campus profile. Run join_campus.py first.")
        dev.ask("wifi off")
        failures = 0
        for i in range(1, args.cycles + 1):
            mode = modes[(i - 1) % len(modes)]
            started = time.monotonic()
            ok, seen = cycle(dev, mode)
            failures += not ok
            print(f"{i:3}/{args.cycles} {mode:6} {'ok  ' if ok else 'FAIL'} "
                  f"{time.monotonic() - started:5.1f} s  {' → '.join(seen)}", flush=True)
            if not ok:
                dev.ask("wifi off")
            time.sleep(random.uniform(0.5, 2))
    except Hang as e:
        print(f"\nHANG: {e}")
        return after_hang(dev, args.port)
    print(f"\nFinished {args.cycles} cycles without a hang. {failures} cycles did not complete normally.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
