"""Build only the DWC2 controller against its matching TinyUSB headers."""
from pathlib import Path

Import("env")


def build_controller(source_env, node):
    upstream = Path(source_env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")) / "MurmurDwc2" / "src"
    controller_env = source_env.Clone()
    controller_env.Prepend(CPPPATH=[str(upstream)])
    return controller_env.Object(node)


env.AddBuildMiddleware(build_controller, "*usb_dwc2.c")
