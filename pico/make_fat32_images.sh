#!/usr/bin/env bash
# Makes the FAT images fat32_test checks, in the given directory. Needs dosfstools and mtools.
set -euo pipefail

cd "$1"
export MTOOLS_SKIP_CHECK=1

head -c 1000 /dev/urandom > small.bin
head -c 512 /dev/urandom > onesect.bin
# 4 KiB clusters below
head -c 4096 /dev/urandom > onecluster.bin
head -c 300000 /dev/urandom > frag.bin
head -c 60000 /dev/urandom > filler.bin
head -c 60000 /dev/urandom > deleted.bin

truncate -s 40M whole.img
mkfs.fat -F 32 -S 512 -s 8 whole.img > /dev/null

mcopy -i whole.img small.bin ::SMALL.BIN
mcopy -i whole.img onesect.bin ::ONESECT.BIN
mcopy -i whole.img onecluster.bin ::ONECLUS.BIN
mcopy -i whole.img small.bin ::NOEXT

# Files interleaved then deleted, so FRAG.MPG is scattered across the gaps
for i in 1 2 3 4 5 6 7 8; do
	mcopy -i whole.img filler.bin "::GAP$i.BIN"
	mcopy -i whole.img filler.bin "::KEEP$i.BIN"
done
for i in 1 2 3 4 5 6 7 8; do
	mdel -i whole.img "::GAP$i.BIN"
done
mcopy -i whole.img frag.bin ::FRAG.MPG

mcopy -i whole.img deleted.bin ::DELETED.BIN
mdel -i whole.img ::DELETED.BIN
mmd -i whole.img ::SUBDIR
mcopy -i whole.img small.bin ::SUBDIR/INSUB.BIN
# A long name, which the 8.3 name beside it stands in for
mcopy -i whole.img small.bin "::A long file name.bin"

# Enough entries that the root directory spans clusters: 4 KiB holds 128
for i in $(seq 1 150); do
	mcopy -i whole.img small.bin "::F$i.BIN"
done

# The same volume in the first partition, as most cards are formatted
python3 - <<'EOF'
START = 2048
volume = open("whole.img", "rb").read()
mbr = bytearray(512)
entry = bytearray(16)
entry[4] = 0x0C  # FAT32 LBA
entry[8:12] = START.to_bytes(4, "little")
entry[12:16] = (len(volume) // 512).to_bytes(4, "little")
mbr[0x1BE:0x1CE] = entry
mbr[510:512] = b"\x55\xaa"
with open("partitioned.img", "wb") as out:
    out.write(mbr)
    out.write(bytes((START - 1) * 512))
    out.write(volume)
EOF

truncate -s 16M fat16.img
mkfs.fat -F 16 fat16.img > /dev/null
head -c 1048576 /dev/zero > blank.img
