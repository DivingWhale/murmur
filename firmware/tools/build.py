"""Build and merge the handheld image using a project-local toolchain."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys


def main():
    firmware = Path(__file__).resolve().parents[1]
    work = firmware.parent / "work"
    temp = work / "tmp"
    temp.mkdir(parents=True, exist_ok=True)
    core = work / "pio"
    env = dict(os.environ, PLATFORMIO_CORE_DIR=str(core), PIP_CACHE_DIR=str(work / "pip-cache"),
               PLATFORMIO_SETTING_ENABLE_TELEMETRY="No", PYTHONNOUSERSITE="1",
               TMP=str(temp), TEMP=str(temp), TMPDIR=str(temp))
    venv_dir = work / "venv"
    python = venv_dir / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.exists():
        subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], env=env, check=True)

    def run(*args):
        subprocess.run([str(python), *map(str, args)], env=env, cwd=firmware, check=True)

    check = subprocess.run([str(python), "-c", "from importlib.metadata import version; "
                            "assert version('platformio') == '6.2.0'"], env=env, capture_output=True)
    if check.returncode:
        run("-m", "pip", "install", "platformio==6.2.0")
    run("-m", "platformio", "run", "-e", "handheld")
    build = firmware / ".pio/build/handheld"
    packages = core / "packages"
    esptool = packages / "tool-esptoolpy/esptool.py"
    app = build / "firmware.bin"
    image = firmware / "build/murmur-handheld.bin"
    image.parent.mkdir(exist_ok=True)
    run(esptool, "--chip", "esp32s3", "merge_bin", "-o", image, "--flash_size", "16MB",
        "0x0", build / "bootloader.bin", "0x8000", build / "partitions.bin",
        "0xe000", packages / "framework-arduinoespressif32/tools/partitions/boot_app0.bin",
        "0x10000", app)
    data, application = image.read_bytes(), app.read_bytes()
    assert data[0x10000:] == application and len(application) <= 0x700000
    assert (firmware / "assets/fonts.bin").read_bytes() in application
    for offset, part in [(0x8000, build / "partitions.bin"),
                         (0xe000, packages / "framework-arduinoespressif32/tools/partitions/boot_app0.bin")]:
        contents = part.read_bytes()
        assert data[offset:offset+len(contents)] == contents
    run(esptool, "--chip", "esp32s3", "image_info", app)
    print(f"{image}\n{len(data):,} bytes\nSHA-256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
