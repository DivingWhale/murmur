"""Name-check the EAP TLS session in the pinned Arduino 2.0.17 SDK."""
from pathlib import Path
import subprocess

Import("env")

platform = env.PioPlatform()
sdk = Path(platform.get_package_dir("framework-arduinoespressif32")) / "tools/sdk/esp32s3"
tools = Path(platform.get_package_dir("toolchain-xtensa-esp32s3")) / "bin"


def prepare_eap_backend(target, source, env):
    output = Path(str(target[0]))
    prefix = str(tools / "xtensa-esp32s3-elf-")
    # Redirect only the supplicant's TLS setup. HTTPS and other TLS users
    # continue to use their own server names and trust settings.
    member = "tls_mbedtls.c.obj"
    subprocess.run([prefix + "ar", "x", str(Path(str(source[0])).resolve()), member],
                   cwd=output.parent, check=True)
    subprocess.run([prefix + "objcopy", "--redefine-sym",
                    "mbedtls_ssl_setup=murmur_eap_ssl_setup", str(output.parent / member), str(output)], check=True)
    symbols = subprocess.check_output([prefix + "nm", "-u", str(output)])
    if b" U murmur_eap_ssl_setup" not in symbols:
        raise RuntimeError("Supplicant TLS setup hook missing")


backend = env.Command("$BUILD_DIR/campus_eap.o", str(sdk / "lib/libwpa_supplicant.a"), prepare_eap_backend)
env.Depends(backend, "$PROJECT_DIR/tools/campus_tls.py")
env.Append(PIOBUILDFILES=backend)
env.Depends("$BUILD_DIR/src/campus_root.S.o", "$PROJECT_DIR/assets/hkustgz-root.der")
