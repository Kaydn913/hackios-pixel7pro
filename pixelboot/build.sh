#!/bin/bash
# Build one of the bare-metal programs (pixelboot/<name>/*.c + common/start.S) as a Linux arm64 Image,
# lz4 it like cheetah's stock kernel and pack a boot image laid out like the stock boot.img, for
# `fastboot boot`. Nothing is flashed.
#
#   [VAR=value ...] pixelboot/build.sh <name>        -> pixelboot/out/<name>-boot.img
#
# Every VAR=value in the environment that the program's main.c knows (FB_BASE, RUN_SECS, ...) is
# passed as -DVAR=value through DEFINES="FB_BASE=0x... RUN_SECS=60".
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=${1:?usage: pixelboot/build.sh <name>}
[ -d "$HERE/$NAME" ] || { echo "no program $NAME in $HERE" >&2; exit 1; }
LLVM=${LLVM:-"/c/Program Files/LLVM/bin"}
[ -x "$LLVM/clang.exe" ] || LLVM=$(dirname "$(command -v clang)")
OUT=$HERE/out/$NAME
mkdir -p "$OUT"

# -mstrict-align: with the MMU off every access is to Device memory, where an unaligned one faults
CFLAGS=(--target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align
	-fno-pic -fno-jump-tables -fno-stack-protector -mcmodel=small -O2 -Wall -Wextra -I"$HERE/common"
	-I"$HERE/..")	# the repository root: .incbin "downloads/..." (vendor firmware, not in the repository)
for d in ${DEFINES:-}; do CFLAGS+=("-D$d"); done
# a program's own include/ (headers it needs that a freestanding build lacks, e.g. wifiprobe12's string.h)
[ -d "$HERE/$NAME/include" ] && CFLAGS+=(-I"$HERE/$NAME/include")
OBJS=("$OUT/start.o")
"$LLVM/clang" "${CFLAGS[@]}" -c "$HERE/common/start.S" -o "$OUT/start.o"
for c in "$HERE/common"/*.c "$HERE/$NAME"/*.c; do
	"$LLVM/clang" "${CFLAGS[@]}" -c "$c" -o "$OUT/$(basename "${c%.c}").o"
	OBJS+=("$OUT/$(basename "${c%.c}").o")
done
"$LLVM/ld.lld" -T "$HERE/common/link.ld" --no-dynamic-linker -static -o "$OUT/$NAME.elf" "${OBJS[@]}"
"$LLVM/llvm-objcopy" -O binary "$OUT/$NAME.elf" "$OUT/Image"
# PAYLOAD=file: an XNUP payload placed behind the program (xnuboot finds it at _end)
if [ -n "${PAYLOAD:-}" ]; then
	python -I "$HERE/tools/append_payload.py" "$OUT/Image" "$PAYLOAD" "$OUT/Image.full" >/dev/null
	mv "$OUT/Image.full" "$OUT/Image"
fi

# cheetah's stock kernel is lz4 (legacy format); COMPRESS=none packs the raw Image instead
KERNEL=$OUT/Image
if [ "${COMPRESS:-lz4}" = lz4 ]; then
	python -I "$HERE/tools/lz4legacy.py" "$OUT/Image" "$OUT/Image.lz4"
	KERNEL=$OUT/Image.lz4
fi
# laid out like the stock boot.img: no boot signature, os_version 0, AVB footer, 64 MB
python -I "$HERE/tools/mkpixelboot.py" "$KERNEL" "$HERE/out/$NAME-boot.img" >/dev/null
echo "$NAME: pixelboot/out/$NAME-boot.img (Image $(wc -c < "$OUT/Image") bytes; ${DEFINES:-no defines})"
