"""Check a Windows USB reader with an uncached file read and concurrent CDC queries."""
import argparse
from concurrent.futures import ThreadPoolExecutor, wait
import ctypes
from ctypes import wintypes
import hashlib
from pathlib import Path
import re
import sys
import threading

import serial

from collect_logs import find_device, get_status


def file_digest(path, stop):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.ReadFile.argtypes = (wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID)
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    size = path.stat().st_size
    chunk = 256 * 1024
    # Align both the request length and the address for FILE_FLAG_NO_BUFFERING.
    alignment = 64 * 1024
    storage = ctypes.create_string_buffer(chunk + alignment - 1)
    address = (ctypes.addressof(storage) + alignment - 1) & -alignment
    handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x20000000, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    digest = hashlib.sha256()
    total = 0
    try:
        while total < size and not stop.is_set():
            received = wintypes.DWORD()
            if not kernel.ReadFile(handle, address, chunk, ctypes.byref(received), None):
                error = ctypes.WinError(ctypes.get_last_error())
                raise OSError(f"USB read failed after {total:,} bytes: {error}") from error
            if not received.value:
                raise OSError(f"Unexpected end of file after {total:,} of {size:,} bytes")
            digest.update(ctypes.string_at(address, received.value))
            total += received.value
    finally:
        kernel.CloseHandle(handle)
    return total, digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("sha256", help="Known reference SHA-256 of the file")
    parser.add_argument("--port", help="Application serial port (auto-detect by default)")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("The uncached reader requires Windows")
    if not re.fullmatch("[0-9a-fA-F]{64}", args.sha256):
        parser.error("Expected a 64-digit SHA-256")
    device = find_device(args.port)
    if not device:
        raise OSError("Murmur application serial port unavailable")

    stop = threading.Event()
    with serial.Serial(device.device, 115200, timeout=0.3, write_timeout=2) as port:
        boot = get_status(port)
        print(f"Starting on {device.device}, boot {boot}", flush=True)
        with ThreadPoolExecutor(max_workers=1) as pool:
            job = pool.submit(file_digest, args.file.resolve(), stop)
            try:
                while not wait([job], timeout=8).done:
                    if get_status(port) != boot:
                        raise OSError("Boot ID changed during the read")
                size, actual = job.result()
                if get_status(port) != boot:
                    raise OSError("Boot ID changed at the end of the read")
            finally:
                stop.set()
    if actual != args.sha256.lower():
        raise ValueError(f"SHA-256 mismatch after {size:,} bytes: {actual}")
    print(f"Verified {size:,} bytes, SHA-256 {actual}, boot {boot}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(str(error))
