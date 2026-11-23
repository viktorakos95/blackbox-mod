#!/bin/sh
# Build the MCL blob (runtime now; the MCL core and the Blackbox platform layer join it in later milestones): out/mcl.elf, out/mcl.bin
set -e
cd "$(dirname "$0")/../.."
mkdir -p out
CPU="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
# M3TEST=1 builds the hardware self test into the blob (it sends sysex on the MIDI jack at boot)
DEF=; [ -n "$M3TEST" ] && DEF=-DM3_TEST; [ -n "$M4TEST" ] && DEF="$DEF -DM4_TEST"; [ -n "$M5TEST" ] && DEF="$DEF -DM5_TEST"; [ -n "$COMPACT" ] && DEF="$DEF -DCOMPACT_LABEL"; [ -n "$SDSPY" ] && DEF="$DEF -DSD_SPY"; [ -n "$SDTEST" ] && DEF="$DEF -DSD_TEST"
OBJS=
for f in mcl_rt bb_midi bb_clock bb_m3test bb_ui bb_sdspy bb_sd bb_sdtest; do
  arm-none-eabi-g++ $CPU -Os -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-builtin -fno-math-errno -std=gnu++17 \
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror $DEF -c -o out/$f.o src/mcl_bb/$f.cpp
  OBJS="$OBJS out/$f.o"
done
arm-none-eabi-g++ $CPU -nostartfiles -nostdlib -Wl,--gc-sections $EXTRA_LD -Wl,-T,src/mcl_bb/mcl_blob.ld -Wl,-Map=out/mcl.map -o out/mcl.elf $OBJS $EXTRA_OBJS -lm -lgcc
arm-none-eabi-objcopy -O binary -j .entry -j .hook -j .text -j .ARM.extab -j .init_array -j .data out/mcl.elf out/mcl.bin
arm-none-eabi-size -A out/mcl.elf | grep -E '^\.(entry|text|init_array|data|bss)'
