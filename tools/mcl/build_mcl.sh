#!/bin/sh
# M8: build the blob WITH MCL itself: MCL's core (compiled by tools/mcl/mcl_size.sh into out/mclobj), the Blackbox platform layer (src/mcl_bb/platform, plus the desktop
# platform files of the MCL checkout, vendored in src/mcl_bb/platform/common: oled, GUI hardware, LEDs, global objects, MidiUart) and the runtime. Result: out/mcl.elf / out/mcl.bin as build_blob.sh makes them.
# usage: MCL_DIR=<MCL checkout> MCL_GFX=<Adafruit-GFX checkout> [MCL_REBUILD=1] tools/mcl/build_mcl.sh     (flags of build_blob.sh: SDTEST=1 COMPACT=1 ... still work)
set -e
cd "$(dirname "$0")/../.."
: "${MCL_DIR:?set MCL_DIR to the MCL checkout}" "${MCL_GFX:?set MCL_GFX to the Adafruit-GFX checkout}"
M=$(realpath "$MCL_DIR"); G=$(realpath "$MCL_GFX"); O=out/mclobj; P=out/mclplat
PLAT="$(realpath src/mcl_bb/platform/common)"
# the core is compiled against the Blackbox platform headers (locks, soft-interrupt triggers, buffer sizes, MidiUart), not MCL's desktop ones; it is rebuilt
# whenever those headers change (or a core was built without them)
STAMP="$PLAT $M $(cat "$PLAT"/*.h | sha1sum | cut -c1-16)"
if [ ! -f $O/defs.h ] || [ -n "$MCL_REBUILD" ] || [ "$(cat $O/plat 2>/dev/null)" != "$STAMP" ]; then
  rm -f $O/*.o; PLAT_INC="-I$PLAT" tools/mcl/mcl_size.sh "$M" "$G" "$O" >/dev/null; echo "$STAMP" > $O/plat
fi
rm -rf $P; mkdir -p $P
INC=$(cd "$M"; for d in src src/mcl include src/resources/rp2040 $(find src/mcl -type d | grep -vE "/tools(/|$)"); do echo -n "-I$M/$d "; done)
CPU="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
COMMON="$CPU -Os -ffunction-sections -fdata-sections -w -DARDUINO=10800 -DPLATFORM_DESKTOP=1 -DVERSION=5000 -include $O/defs.h -Isrc/mcl_bb/platform/common -Isrc/mcl_bb/platform -Isrc/mcl_bb -I$G $INC -fno-exceptions -fno-rtti -fno-threadsafe-statics -fpermissive -std=gnu++17"
for f in oled GUI_hardware LED_hardware global hardware MidiUart; do arm-none-eabi-g++ $COMMON -c src/mcl_bb/platform/common/$f.cpp -o $P/$f.o; done
for f in bb_libc bb_platform bb_sdfat bb_mcl; do arm-none-eabi-g++ $COMMON -fno-builtin -fno-tree-loop-distribute-patterns -c src/mcl_bb/platform/$f.cpp -o $P/$f.o; done        # (a string function must not be turned into a call to itself)
EXTRA_OBJS="$(ls $O/*.o | grep -v "/dso.o" | tr '\n' ' ') $(ls $P/*.o | tr '\n' ' ')" EXTRA_LD="-Wl,--wrap=_ZN4Oled7displayEv -Wl,--defsym=mclstr_title_project=mclstr_title_files" sh tools/mcl/build_blob.sh
