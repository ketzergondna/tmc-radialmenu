#!/bin/sh
# Builds the radial-menu hack on top of the vanilla EU ROM built from the decomp.
set -e
TMC=${TMC:-../../tmc}   # path to a built zeldaret/tmc checkout
BASE=0x08EF4000          # free space (ROM data ends at 0x08EF333C)
TABLE=0x080FBF34         # GameTask_Main state table, entry [GAMEMAIN_UPDATE]
CFLAGS="-mthumb -mcpu=arm7tdmi -O2 -mlong-calls -fno-toplevel-reorder -ffreestanding -fno-builtin -Wall -Wno-unused-function \
 -DEU -DREVISION=0 -DENGLISH -I $TMC/include -I $TMC/build/EU -I $TMC/tools/agbcc/include -I ."
python3 gfx.py radial_gfx.h preview.png
arm-none-eabi-gcc $CFLAGS -c radial.c -o radial.o
# resolve game symbols from the decomp ELF (thumb funcs keep their odd address)
arm-none-eabi-nm -u radial.o | awk '{print $2}' > undef.txt
readelf -sW $TMC/tmc_eu.elf | awk '{print $2, $4, $8}' > syms.txt
: > syms.ld
for s in $(cat undef.txt); do
  case $s in __*|_call_via_*) continue;; esac
  if [ "$s" = GameMain_Update_Orig ]; then n=GameMain_Update; else n=$s; fi
  line=$(awk -v n="$n" '$3==n {print; exit}' syms.txt)
  [ -z "$line" ] && { echo "missing symbol $n"; exit 1; }
  echo "$s = 0x$(echo $line | cut -d' ' -f1);" >> syms.ld
done
cat > link.ld <<EOT
SECTIONS { . = $BASE; .text : { radial.o(.text*) *(.text*) radial.o(.rodata*) *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }
EOT
arm-none-eabi-ld -T link.ld -T syms.ld radial.o $(arm-none-eabi-gcc -mthumb -mcpu=arm7tdmi -print-libgcc-file-name) -o radial.elf
arm-none-eabi-objcopy -O binary -j .text radial.elf radial.bin
[ "$(arm-none-eabi-size -A radial.elf | awk '$1==".data"||$1==".bss"{s+=$2} END{print s+0}')" = 0 ] || { echo ".data/.bss not allowed"; exit 1; }
ENTRY=$(arm-none-eabi-nm radial.elf | awk '$3=="Radial_GameMainUpdate"{print $1}')
python3 - "$TMC/baserom_eu.gba" radial.bin $BASE $TABLE $ENTRY <<'PY'
import sys, struct
rom = bytearray(open(sys.argv[1], 'rb').read())
code = open(sys.argv[2], 'rb').read()
base, table, entry = int(sys.argv[3], 16), int(sys.argv[4], 16), int(sys.argv[5], 16)
off = base - 0x08000000
assert all(b == 0xFF for b in rom[off:off + len(code)]), "target area not free"
assert struct.unpack_from('<I', rom, table - 0x08000000)[0] == 0x080517FD, "unexpected table contents"
rom[off:off + len(code)] = code
struct.pack_into('<I', rom, table - 0x08000000, entry | 1)
open('zelda_radial_eu.gba', 'wb').write(rom)
print("code %d bytes at %08x, entry %08x" % (len(code), base, entry | 1))
PY
