"""Export Murmur diagnostics and optionally watch for USB reconnects."""

import argparse
from datetime import datetime
from pathlib import Path
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports


STATUS = re.compile(r"log status version=1 boot=([0-9a-f]{8})\b.*")
SOURCE = re.compile(
    r"log source=(checkpoint|incident|previous|current|usb_gap) boot=([0-9a-f]{8}) "
    r"build=[0-9a-f]{16} reset=\d+ phase=\d+ wifi_step=\d+ count=(\d+)"
)
EVENT = re.compile(r"log seq=\d+ ms=\d+ core=\d+ event=[a-z_.]+ a=-?\d+ b=-?\d+")
END = re.compile(r"log end count=(\d+)")


def find_device(port, identity=None):
    found = [p for p in list_ports.comports()
             if (p.vid, p.pid) in ((0x303A, 0x4003), (0x303A, 0x4001))
             and (not port or p.device.casefold() == port.casefold())
             and (not identity or p.serial_number == identity)]
    if len(found) > 1:
        raise ValueError("Connect one Murmur or specify --port.")
    return found[0] if found else None


def lines(port, deadline):
    pending = bytearray()
    while time.monotonic() < deadline:
        pending.extend(port.readline(256))
        if len(pending) > 256:
            raise ValueError("Diagnostic line exceeds the protocol limit")
        if pending.endswith(b"\n"):
            yield pending.decode("ascii").rstrip("\r\n")
            pending.clear()


def get_status(port):
    port.write(b"log status\n")
    for line in lines(port, time.monotonic() + 5):
        match = STATUS.fullmatch(line)
        if match:
            return match[1]
    raise TimeoutError("The device did not answer log status")


def read_dump(port, usb=False):
    port.write(b"log usb\n" if usb else b"log dump\n")
    records = []
    started = False
    count = remaining = 0
    boot = None
    for line in lines(port, time.monotonic() + 18):
        if not started:
            if line == "log begin version=1":
                started = True
                records.append(line)
            continue
        source = SOURCE.fullmatch(line)
        end = END.fullmatch(line)
        if source and remaining == 0:
            remaining = int(source[3])
            if not 1 <= remaining <= 128:
                raise ValueError("Invalid diagnostic section length")
            if source[1] == "current":
                boot = source[2]
        elif end and remaining == 0 and int(end[1]) == count and boot:
            return boot, records + [line]
        elif remaining > 0 and EVENT.fullmatch(line):
            count += 1
            remaining -= 1
        else:
            raise ValueError("Invalid or incomplete diagnostic section")
        records.append(line)
    raise TimeoutError("The diagnostic export did not finish")


def windows_usb_errors():
    if sys.platform != "win32":
        return None
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
             "(Get-CimInstance Win32_PnPEntity | Where-Object { "
             "$_.ConfigManagerErrorCode -eq 43 -and $_.PNPDeviceID -like 'USB\\*' "
             "} | Measure-Object).Count"],
            capture_output=True, text=True, timeout=6, check=True,
        )
        return int(result.stdout.strip())
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def run(args, output):
    def host(message):
        print(f"{datetime.now().astimezone().isoformat(timespec='milliseconds')} host {message}",
              file=output, flush=True)
        print(message, flush=True)

    identity = active = boot = None
    last_attempt = next_usb_check = 0.0
    previous_error = None
    host("waiting for Murmur" if args.watch else "looking for Murmur")
    while True:
        device = find_device(args.port, identity)
        now = time.monotonic()
        name = device.device if device else None
        if name != active:
            if active:
                host(f"USB serial disappeared: {active}")
            if device:
                identity = device.serial_number
                host(f"USB serial appeared: {name} PID={device.pid:04x}")
                boot = None
                last_attempt = 0.0
            previous_error = None
            active = name
            next_usb_check = now
        if device and (not args.watch or now - last_attempt >= 10):
            last_attempt = now
            try:
                with serial.Serial(name, 115200, timeout=0.3, write_timeout=1.5) as port:
                    time.sleep(0.2)
                    port.reset_input_buffer()
                    if boot is None or get_status(port) != boot:
                        boot, records = read_dump(port, args.usb)
                        host(f"collected boot {boot} on {name}")
                        output.write("\n".join(records) + "\n")
                        for line in records:
                            if " event=wdt.task " in line:
                                fields = dict(field.split("=", 1) for field in line.split()[1:])
                                packed = b"".join(int(fields[key]).to_bytes(4, "little", signed=True)
                                                  for key in ("a", "b"))
                                task = packed.split(b"\0", 1)[0].decode("ascii", errors="replace")
                                host(f"watchdog interrupted CPU {fields['core']} running {task}")
                        output.flush()
                    previous_error = None
                if not args.watch:
                    return
            except (OSError, ValueError) as error:
                if not args.watch:
                    raise
                # Retry while the port is in use or the device is restarting.
                if str(error) != previous_error:
                    host(f"collection delayed on {name}: {error}")
                    previous_error = str(error)
        elif not device:
            if not args.watch:
                raise FileNotFoundError("No Murmur USB serial port found")
            if now >= next_usb_check:
                code43 = windows_usb_errors()
                if code43:
                    host(f"Windows reports USB Code 43 for {code43} device(s)")
                next_usb_check = now + 15
        time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Use a specific serial port")
    parser.add_argument("--watch", action="store_true", help="Collect again after a reboot or reconnect")
    parser.add_argument("--usb", action="store_true", help="Read the last saved USB traffic gap and current events")
    parser.add_argument("--out", type=Path, help="Append to this file")
    args = parser.parse_args()
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    out = args.out or Path(__file__).resolve().parents[2] / "work" / "diagnostics" / f"murmur-{stamp}.log"
    try:
        out.parent.mkdir(parents=True, exist_ok=True)
        print(f"Writing {out.resolve()}", flush=True)
        with out.open("a", encoding="utf-8") as output:
            run(args, output)
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
