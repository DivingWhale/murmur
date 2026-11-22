"""Pair one USB-connected Murmur with its Pi using the Pi's existing SSH login."""
import argparse
from ipaddress import IPv4Address
import re
import subprocess
import sys

import serial
from serial.tools import list_ports

from join_campus import command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ssh_host", help="SSH alias for the Pi")
    parser.add_argument("campus_ip", type=IPv4Address)
    parser.add_argument("--port")
    args = parser.parse_args()
    ports = [p.device for p in list_ports.comports() if (p.vid,p.pid) == (0x303a,0x4003)]
    if not args.port and len(ports) != 1:
        sys.exit("Connect one Murmur or provide --port.")
    # Only the device pairing secret crosses USB. The cloud credential stays on the Pi.
    key = subprocess.check_output(["ssh", "-o", "ClearAllForwardings=yes", "--", args.ssh_host,
        "sed -n 's/^MURMUR_PSK=//p' ~/.config/murmur/service.env"]).strip()
    if not re.fullmatch(b"[a-fA-F0-9]{64}", key):
        sys.exit("No valid device pairing key in the Pi service configuration.")
    with serial.Serial(args.port or ports[0], 115200, timeout=5, write_timeout=3) as port:
        port.reset_input_buffer()
        reply = command(port, b"sync pair " + str(args.campus_ip).encode() + b" " + key)
    if reply != "sync paired":
        sys.exit("Pairing was not acknowledged. Wait for any current sync to finish and retry.")
    print("Murmur paired; campus sync requested.")


if __name__ == "__main__":
    main()
