"""Set up or reuse the handheld's encrypted HKUSTGZ profile over USB."""
import argparse
from getpass import getpass
from ipaddress import IPv4Address
import sys
import time

import serial
from serial.tools import list_ports


def command(port, line):
    port.write(line + b"\n")
    port.flush()
    return port.readline().decode("ascii", errors="replace").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--replace", action="store_true", help="Replace the saved campus login after successful authentication.")
    parser.add_argument("--pi", type=IPv4Address, help="Check the Pi's SSH banner from the handheld.")
    args = parser.parse_args()
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x303A, 0x4003)]
    if not args.port and len(ports) != 1:
        sys.exit("Connect one Murmur by USB or specify --port.")
    with serial.Serial(args.port or ports[0], 115200, timeout=4, write_timeout=3) as port:
        port.reset_input_buffer()
        if not command(port, b"wifi status").startswith("wifi "):
            sys.exit("Murmur did not answer the Wi-Fi status command.")
        port.write(f"time {int(time.time())}\n".encode())
        profile = command(port, b"wifi profile")
        if profile not in ("wifi saved", "wifi empty"):
            sys.exit("此固件不支持设备端保存。")
        if profile == "wifi saved" and not args.replace:
            reply = command(port, b"wifi connect")
        else:
            print("Murmur → HKUSTGZ  |  保存设备登录（需插入日常使用的 TF 卡）", flush=True)
            user = input("校园账号（邮箱 @ 前的部分）: ").strip().encode()
            password = getpass("校园密码（输入时隐藏）: ").encode()
            if not all(0 < len(v) <= 128 and not any(c in v for c in (b"\n", b"\r", b"\x00")) for v in (user, password)) or b"\t" in user:
                sys.exit("账号或密码格式不符合要求。")
            reply = command(port, b"wifi save " + user + b"\t" + password)
            del password
        if reply != "wifi joining" and not reply.startswith("wifi online "):
            sys.exit("Connection setup failed: " + reply)
        print("正在连接并等待 DHCP…", flush=True)
        deadline = time.monotonic() + 38
        while time.monotonic() < deadline:
            time.sleep(.6)
            reply = command(port, b"wifi status")
            if reply.startswith("wifi online "):
                print(reply, flush=True)
                if args.pi:
                    probe = command(port, f"wifi probe {args.pi}".encode())
                    print(probe, flush=True)
                    return 0 if probe == "wifi pi reachable" else 2
                return 0
            if reply != "wifi joining":
                sys.exit("Connection failed: " + reply)
        sys.exit("Timed out waiting for the handheld.")


if __name__ == "__main__":
    sys.exit(main())
