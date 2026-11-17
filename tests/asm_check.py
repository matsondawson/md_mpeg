#!/usr/bin/env python3
# Runs the Pico's Cortex-M0+ assembler (pico/idct_m0.S, pico/predict_m0.S) in the unicorn emulator, as a
# Cortex-M0, on the cases tests/md_mpeg_asm_vectors.cpp wrote with the decoder's C results, and fails if any
# differ. Like pico/asm_test.cpp on the board, without one. Unicorn's Cortex-M0 model still runs some Thumb-2
# instructions; assembling with -mcpu=cortex-m0plus is what rejects those.
#
# Usage: asm_check.py <image.bin> <symbols> <vectors>
#   image.bin: the assembler linked at IMAGE_ADDRESS, as raw bytes
#   symbols: arm-none-eabi-nm output for it
#   vectors: md_mpeg_asm_vectors output

import struct
import sys

from unicorn import UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_CPU_ARM_CORTEX_M0)

IMAGE_ADDRESS = 0x10000000
RAM_ADDRESS, RAM_SIZE = 0x20000000, 0x10000
STACK_TOP = RAM_ADDRESS + RAM_SIZE
BLOCK, PICTURE, SOURCE = RAM_ADDRESS, RAM_ADDRESS + 0x1000, RAM_ADDRESS + 0x2000
# Functions return here: a halfword in the image's mapping, past the code
RETURN = IMAGE_ADDRESS + 0xF000

# Must match tests/md_mpeg_asm_vectors.cpp
IDCT_STRIDE, IDCT_COLUMN = 24, 8
IDCT_PICTURE = IDCT_STRIDE * 8
PREDICT_STRIDE, PREDICT_COLUMN = 28, 4
PREDICT_SOURCE, PREDICT_PICTURE = PREDICT_STRIDE * 17, PREDICT_STRIDE * 16


def main():
    image_path, symbols_path, vectors_path = sys.argv[1:4]
    image = open(image_path, "rb").read()
    symbols = {}
    for line in open(symbols_path):
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t"):
            symbols[parts[2]] = int(parts[0], 16)

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
    uc.mem_map(IMAGE_ADDRESS, 0x10000)
    uc.mem_write(IMAGE_ADDRESS, image)
    uc.mem_map(RAM_ADDRESS, RAM_SIZE)

    def call(name, *args):
        uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
        uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(reg, value & 0xFFFFFFFF)
        uc.emu_start(symbols[name] | 1, RETURN, count=200000)
        if uc.reg_read(UC_ARM_REG_SP) != STACK_TOP:
            raise RuntimeError("stack pointer not restored")

    data = open(vectors_path, "rb").read()
    position = 0
    counts = {"idct": [0, 0], "predict": [0, 0]}

    def take(size):
        nonlocal position
        chunk = data[position:position + size]
        position += size
        return chunk

    def fail(kind, description):
        counts[kind][1] += 1
        if counts[kind][1] <= 8:
            print(f"{kind} FAIL {description}")

    while position < len(data):
        (kind,) = struct.unpack("<I", take(4))
        if kind == 1:
            add, masks = struct.unpack("<II", take(8))
            block, picture, expected = take(256), take(IDCT_PICTURE), take(IDCT_PICTURE)
            uc.mem_write(BLOCK, block)
            uc.mem_write(PICTURE, picture)
            name = "video_idct_add_m0" if add else "video_idct_put_m0"
            counts["idct"][0] += 1
            try:
                call(name, BLOCK, PICTURE + IDCT_COLUMN, IDCT_STRIDE, masks)
            except (UcError, RuntimeError) as error:
                fail("idct", f"{name} masks {masks:04x}: {error}")
                continue
            actual = bytes(uc.mem_read(PICTURE, IDCT_PICTURE))
            cleared = bytes(uc.mem_read(BLOCK, 256)) == bytes(256)
            if actual != expected or not cleared:
                at = next((i for i in range(IDCT_PICTURE) if actual[i] != expected[i]), None)
                where = f"pixel {at}: expected {expected[at]} got {actual[at]}" if at is not None else "block not cleared"
                fail("idct", f"{name} masks {masks:04x}: {where}")
        elif kind == 2:
            size, half_pel, rows, offset = struct.unpack("<IIII", take(16))
            source, picture, expected = take(PREDICT_SOURCE), take(PREDICT_PICTURE), take(PREDICT_PICTURE)
            uc.mem_write(SOURCE, source)
            uc.mem_write(PICTURE, picture)
            name = f"video_predict_m0_{size // 4}_{half_pel}_{offset}"
            counts["predict"][0] += 1
            try:
                call(name, PICTURE + PREDICT_COLUMN, SOURCE, PREDICT_STRIDE, rows)
            except (UcError, RuntimeError) as error:
                fail("predict", f"{name} rows {rows}: {error}")
                continue
            actual = bytes(uc.mem_read(PICTURE, PREDICT_PICTURE))
            if actual != expected:
                at = next(i for i in range(PREDICT_PICTURE) if actual[i] != expected[i])
                fail("predict", f"{name} rows {rows}: pixel {at % PREDICT_STRIDE},{at // PREDICT_STRIDE} "
                                f"expected {expected[at]} got {actual[at]}")
        else:
            sys.exit(f"Bad record kind {kind} at {position - 4}")

    for kind, (cases, failures) in counts.items():
        print(f"{kind}: {failures} of {cases} failed")
    if not counts["idct"][0] or not counts["predict"][0]:
        sys.exit("No cases")
    sys.exit(1 if any(failures for _, failures in counts.values()) else 0)


if __name__ == "__main__":
    main()
