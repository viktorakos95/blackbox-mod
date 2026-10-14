#!/bin/bash
# M1: compile MCL's core (src/mcl + src/resources/rp2040 + Adafruit GFX 1.12.1) for Cortex-M7 and report the size after a gc-sections link.
# usage: tools/mcl/mcl_size.sh <MCL checkout> <Adafruit-GFX checkout> <outdir> [extra compile flags, e.g. -flto]
# Needs arm-none-eabi-gcc/g++. Platform layer and libc are NOT included (undefined symbols are ignored): the numbers are a lower bound.
set -e
M=$(realpath "$1"); G=$(realpath "$2"); O=$(realpath -m "$3"); shift 3; mkdir -p "$O"
INC=$(cd "$M"; for d in src/platform/desktop src/platform/desktop_common src src/mcl include src/resources/rp2040 $(find src/mcl -type d | grep -vE "/tools(/|$)"); do echo -n "-I$M/$d "; done)
CPU="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
printf '#define VERSION_STR "T5.00"\n' > "$O/defs.h"
COMMON="$CPU -Os -ffunction-sections -fdata-sections -w -DARDUINO=10800 -DPLATFORM_DESKTOP=1 -DVERSION=5000 -include $O/defs.h -I$G $INC $*"
cd "$M"
find src/mcl src/resources/rp2040 -name '*.cpp' | grep -vE "/(tools|Tests)/|Drivers/TBD" | xargs -P8 -I{} sh -c \
  'arm-none-eabi-g++ '"$COMMON"' -fno-exceptions -fno-rtti -fpermissive -std=gnu++17 -c {} -o '"$O"'/$(echo {} | tr / _).o'
arm-none-eabi-gcc $COMMON -c src/mcl/CommonTools/helpers.c -o "$O/helpers.o"
arm-none-eabi-g++ $COMMON -fno-exceptions -fno-rtti -fpermissive -std=gnu++17 -c "$G/Adafruit_GFX.cpp" -o "$O/gfx.o"
echo 'void* __dso_handle;' > "$O/dso.c"; arm-none-eabi-gcc $CPU $* -c "$O/dso.c" -o "$O/dso.o"
arm-none-eabi-g++ $CPU -Os $* -fno-exceptions -fno-rtti -nostartfiles -nostdlib -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
  -Wl,-e,_Z5setupv -Wl,-u,_Z4loopv "$O"/*.o -o "$O/mcl.elf" 2>&1 | grep -v warning || true
arm-none-eabi-size -A "$O/mcl.elf" | grep -E "^\.(text|rodata|data|bss)"
