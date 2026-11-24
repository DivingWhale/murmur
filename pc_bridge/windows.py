"""Locate a missing NTFS note by file ID without enumerating the rest of the Vault."""
import ctypes
from ctypes import wintypes
import msvcrt
from pathlib import Path


class FileID(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("kind", ctypes.c_int), ("id", ctypes.c_uint64 * 2)]


def moved_file(marker, inode):
    if not inode:
        return None
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.OpenFileById.argtypes = [wintypes.HANDLE, ctypes.POINTER(FileID), wintypes.DWORD,
                               wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD]
    k.OpenFileById.restype = wintypes.HANDLE
    k.GetFinalPathNameByHandleW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    key = FileID()
    key.size = ctypes.sizeof(key)
    key.id[0] = inode & ((1 << 64)-1)
    key.id[1] = inode >> 64
    key.kind = 2 if key.id[1] else 0
    with marker.open("rb") as hint:
        handle = k.OpenFileById(msvcrt.get_osfhandle(hint.fileno()), ctypes.byref(key), 0, 7, None, 0)
    if handle == wintypes.HANDLE(-1).value:
        error = ctypes.get_last_error()
        if error in (1, 2, 3, 50, 87, 1168):
            # Missing file or a filesystem without open-by-ID support.
            return None
        raise ctypes.WinError(error)
    try:
        buf = ctypes.create_unicode_buffer(32768)
        length = k.GetFinalPathNameByHandleW(handle,buf,len(buf),0)
        if not 0 < length < len(buf):
            raise ctypes.WinError(ctypes.get_last_error())
        name = buf.value
        if name.startswith("\\\\?\\UNC\\"):
            return Path("\\\\" + name[8:])
        return Path(name.removeprefix("\\\\?\\"))
    finally:
        k.CloseHandle(handle)
