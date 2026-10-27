#!/bin/bash
# Builds out/Image (arm64 Linux-style image) and out/a30s-hello-boot.img (+ .tar for Odin).
# Usage: ./build.sh [probe]   (probe adds MMIO ID-register reads)
set -euo pipefail
cd "$(dirname "$0")"
EXTRA=""
NAME="a30s-hello"
if [ "${1:-}" = "probe" ]; then EXTRA="-DPROBE"; NAME="a30s-probe"; fi
SRCS="start.S hello.c console.c"
if [ "${1:-}" = "m2" ]; then NAME="a30s-m2d2"; SRCS="start.S m2.c fdt.c mmu.c console.c"; fi
if [ "${1:-}" = "m3" ]; then NAME="a30s-m3b"; SRCS="start.S m3.c fdt.c mmu.c console.c"; fi
if [ "${1:-}" = "crash" ]; then EXTRA="-DCRASHTEST"; NAME="a30s-crash"; fi
mkdir -p out
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -nostdlib -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra $EXTRA ${FBFLAGS:-}"
for f in $SRCS; do
	clang $CFLAGS -c $f -o out/${f%.*}.o
done
# no absolute relocations allowed: the image runs wherever the bootloader puts it
abs=$(for f in ${SRCS#start.S }; do llvm-readelf -r out/${f%.*}.o; done | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations" >&2; exit 1; }
ld.lld -T shim.ld --build-id=none -N $(for f in $SRCS; do echo out/${f%.*}.o; done) -o out/$NAME.elf
llvm-objcopy -O binary out/$NAME.elf out/$NAME.bin
size=$(llvm-nm out/$NAME.elf | awk '$3=="_end"{print strtonum("0x"$1)}' 2>/dev/null || true)
if [ -z "$size" ]; then
	size=$(python3 -I -c "
import subprocess
o=subprocess.check_output(['llvm-nm','out/$NAME.elf']).decode().split('\n')
print([int(l.split()[0],16) for l in o if l.endswith(' _end')][0])")
fi
cp out/$NAME.bin out/$NAME.Image
truncate -s "$size" out/$NAME.Image
python3 -I bootimg.py out/$NAME.Image out/$NAME-boot.img
ls -l out/$NAME.Image out/$NAME-boot.img*
