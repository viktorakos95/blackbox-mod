#!/bin/sh
# Build the MCL blob (runtime now; the MCL core and the Blackbox platform layer join it in later milestones): out/mcl.elf, out/mcl.bin
set -e
cd "$(dirname "$0")/../.."
mkdir -p out
CPU="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
arm-none-eabi-g++ $CPU -Os -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-builtin -fno-math-errno -std=gnu++17 \
  -ffunction-sections -fdata-sections -Wall -Wextra -Werror -c -o out/mcl_rt.o src/mcl_bb/mcl_rt.cpp
arm-none-eabi-g++ $CPU -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-T,src/mcl_bb/mcl_blob.ld -Wl,-Map=out/mcl.map -o out/mcl.elf out/mcl_rt.o -lgcc
arm-none-eabi-objcopy -O binary -j .entry -j .text -j .ARM.extab -j .init_array -j .data out/mcl.elf out/mcl.bin
arm-none-eabi-size -A out/mcl.elf | grep -E '^\.(entry|text|init_array|data|bss)'
