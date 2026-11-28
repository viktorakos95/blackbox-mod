#!/bin/sh
# Compile src/*.c into the code cave: bank 1 (out/cave.bin, with the core's ITCM image) and bank 2 (out/b2code.bin + out/itcm2.bin, the machines: src/b2.h).
# Each source is compiled to its own object (out/obj/), because the linker script places the objects by name (tools/b2link.py writes it).
set -e
cd "$(dirname "$0")"
mkdir -p out out/obj
# The machines: everything the SONG round's pages, the Looper / SAMPLR / Trigger2MIDI engines, the DRONE-8 engine and the clock monitor are made of. All else is the core.
MACH="looper looper_page samplr t2m t2m_bb t2m_page clkmon lira lira_chord lira_pad looper_thunk lira_thunk"
CF="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
arm-none-eabi-g++ $CF -O2 -ffreestanding -nostdlib -fno-exceptions \
  -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-math-errno -std=gnu++17 -Wall -Werror -Wno-unused-function \
  -Wno-class-memaccess -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter -ffunction-sections -isystem src/munchi/sys -c -o out/munchi.o src/munchi.cpp
OBJS="out/munchi.o"
for f in solo slice duck chord cond filter cpu od comp seqfix seqmode savexml fx2 looper looper_page samplr t2m t2m_bb t2m_page clkmon lira lira_chord lira_pad crash b2 b2load; do
  arm-none-eabi-gcc $CF -Os -ffunction-sections -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror -c -o out/obj/$f.o src/$f.c
  OBJS="$OBJS out/obj/$f.o"
done
for f in duck_thunk cond_thunk filter_thunk seqmode_thunk fx2_thunk looper_thunk lira_thunk crash_thunk itcm_thunk b2load_thunk; do
  arm-none-eabi-gcc $CF -c -o out/obj/$f.o src/$f.S
  OBJS="$OBJS out/obj/$f.o"
done
cp out/munchi.o out/obj/munchi.o
python3 tools/b2link.py $MACH -- $OBJS
arm-none-eabi-gcc $CF -c -o out/obj/b2stubs.o out/b2stubs.S
arm-none-eabi-gcc $CF -Os -ffreestanding -nostdlib -fno-builtin -T out/cave.ld -Wl,-e,0 -o out/cave.elf $OBJS out/obj/b2stubs.o
arm-none-eabi-objcopy -O binary -j .itcm -j .cave out/cave.elf out/cave.bin
arm-none-eabi-objcopy -O binary -j .bank2 out/cave.elf out/b2code.bin
arm-none-eabi-objcopy -O binary -j .itcm2 out/cave.elf out/itcm2.bin
arm-none-eabi-size -A out/cave.elf | grep -E 'cave|itcm|bank2'
