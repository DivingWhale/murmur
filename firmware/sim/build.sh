#!/bin/sh
# Build the host UI simulator into work/sim/. LVGL objects are cached.
set -e
here=$(cd "$(dirname "$0")" && pwd)
firmware=$(dirname "$here")
out=$(dirname "$firmware")/work/sim
lvgl=$firmware/.pio/libdeps/handheld/lvgl
[ -d "$lvgl" ] || { echo "Run firmware/tools/build.py once first so PlatformIO fetches LVGL." >&2; exit 1; }
mkdir -p "$out/lvgl"
# Keep LVGL settings in step with build_flags in platformio.ini (except the SPI byte swap).
flags="-DLV_CONF_SKIP -DLV_COLOR_16_SWAP=0 $(sed -n 's/^ *-D\(LV_[A-Z_]*=[^ ]*\).*/-D\1/p' "$firmware/platformio.ini" | grep -v SWAP | tr '\n' ' ')"
objs=""
for src in $(cd "$lvgl" && find src -name '*.c'); do
    obj="$out/lvgl/$(echo "$src" | tr / _).o"
    if [ ! -f "$obj" ] || [ "$lvgl/$src" -nt "$obj" ] || [ "$firmware/platformio.ini" -nt "$obj" ]; then
        cc -O2 -w $flags -I"$lvgl" -I"$lvgl/src" -c "$lvgl/$src" -o "$obj"
    fi
    objs="$objs $obj"
done
cc -c -DFONTS_PATH="\"$firmware/assets/fonts.bin\"" "$here/fontdata_mac.S" -o "$out/fontdata.o"
c++ -std=gnu++17 -O1 -g -Wall -Wno-unused-function $flags -include Arduino.h \
    -I"$here/include" -I"$lvgl" -I"$firmware/src" \
    "$firmware/src/main.cpp" "$firmware/src/fonts.cpp" "$firmware/src/icons.cpp" "$firmware/src/icons_data.cpp" "$here/sim.cpp" \
    "$out/fontdata.o" $objs -lz -o "$out/murmur-sim"
echo "$out/murmur-sim"
