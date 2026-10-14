#!/bin/sh
# M2: link the real MCL core (objects from tools/mcl/mcl_size.sh) with the blob runtime and linker script, to see (a) that .data/.bss/heap fit the
# reserved SDRAM region and the code the blob window, (b) which symbols the Blackbox platform layer still has to provide (undefined list).
# usage: tools/mcl/mcl_link_report.sh <outdir of mcl_size.sh> <report dir>
set -e
cd "$(dirname "$0")/../.."
O=$(realpath "$1"); R=$(realpath -m "$2"); mkdir -p "$R"
CPU="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
tools/mcl/build_blob.sh >/dev/null
OBJS=$(ls "$O"/*.o | grep -v '/dso\.o$')
arm-none-eabi-ld -r -o "$R/all.o" $OBJS out/mcl_rt.o out/bb_midi.o out/bb_m3test.o 2>&1 | grep -v warning || true
arm-none-eabi-nm -u -C "$R/all.o" | sed 's/^ *U //' | sort -u > "$R/undefined.txt"
arm-none-eabi-g++ $CPU -nostartfiles -nostdlib -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all -Wl,-T,src/mcl_bb/mcl_blob.ld \
  -Wl,-u,_Z5setupv -Wl,-u,_Z4loopv -Wl,-Map="$R/mcl.map" -o "$R/mcl.elf" $OBJS out/mcl_rt.o out/bb_midi.o out/bb_m3test.o -lgcc 2>&1 | grep -v warning || true
arm-none-eabi-size -A "$R/mcl.elf" | grep -E '^\.(entry|text|init_array|data|bss)'
echo "undefined symbols: $(wc -l < "$R/undefined.txt") (list in $R/undefined.txt)"
