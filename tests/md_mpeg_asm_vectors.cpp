// Writes random cases for the Cortex-M0+ assembler (pico/idct_m0.S, pico/predict_m0.S) with the decoder's C
// results, for tests/asm_check.py to run the assembler on in an emulator. The cases are shaped like
// pico/asm_test.cpp's, which does the same on the board.
//
// Usage: md_mpeg_asm_vectors <output file> <cases of each>
//
// Little-endian records, each starting with a uint32_t kind:
//   1, IDCT: add, masks, int32_t block[64], uint8_t picture[IDCT_PICTURE], uint8_t expected[IDCT_PICTURE]. The
//      block is written at column IDCT_COLUMN of an 8 row picture with stride IDCT_STRIDE.
//   2, prediction: size (8 or 16), half pel, rows, source offset (0..3), uint8_t source[PREDICT_SOURCE],
//      uint8_t picture[PREDICT_PICTURE], uint8_t expected[PREDICT_PICTURE]. Predicts from source + offset, with
//      stride PREDICT_STRIDE, into the picture at column PREDICT_COLUMN.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mpeg1/VideoInternal.h"

namespace {
	constexpr uint32_t IDCT_STRIDE = 24, IDCT_COLUMN = 8, IDCT_PICTURE = IDCT_STRIDE * 8;
	// A 16 pixel block reads up to a word past its right edge and a row below
	constexpr uint32_t PREDICT_STRIDE = 28, PREDICT_COLUMN = 4;
	constexpr uint32_t PREDICT_SOURCE = PREDICT_STRIDE * 17, PREDICT_PICTURE = PREDICT_STRIDE * 16;

	uint32_t s_random = 12345;

	uint32_t random() {
		s_random ^= s_random << 13;
		s_random ^= s_random >> 17;
		s_random ^= s_random << 5;
		return s_random;
	}

	void put32(FILE *out, const uint32_t value) {
		fwrite(&value, 4, 1, out);
	}

	void writeIdct(FILE *out) {
		int32_t block[64] = {};
		uint32_t masks = 0;
		const uint32_t shape = random() % 4;
		const uint32_t count = 1 + random() % (shape == 3 ? 40 : 6);
		for (uint32_t c = 0; c < count; c++) {
			uint32_t index = random() & 63;
			if (shape == 0) {
				index &= 0x38; // first column only
			} else if (shape == 1) {
				index &= 7; // first row only
			}
			const int32_t level = static_cast<int32_t>(random() % 4096) - 2048;
			const int32_t small = static_cast<int32_t>(random() % 64) - 32;
			block[index] = videoPremultiply((random() & 3) ? small : level, index);
			masks |= (index >= 8 ? 3u : 2u) << (2 * (index & 7));
		}
		const uint32_t add = random() & 1;
		uint8_t picture[IDCT_PICTURE], expected[IDCT_PICTURE];
		for (uint32_t p = 0; p < IDCT_PICTURE; p++) {
			picture[p] = expected[p] = static_cast<uint8_t>(random());
		}
		put32(out, 1);
		put32(out, add);
		put32(out, masks);
		fwrite(block, 4, 64, out);
		fwrite(picture, 1, IDCT_PICTURE, out);
		video_idct(block);
		for (uint32_t y = 0; y < 8; y++) {
			for (uint32_t x = 0; x < 8; x++) {
				uint8_t &pixel = expected[y * IDCT_STRIDE + IDCT_COLUMN + x];
				pixel = clampToByte(add ? pixel + block[y * 8 + x] : block[y * 8 + x]);
			}
		}
		fwrite(expected, 1, IDCT_PICTURE, out);
	}

	void writePredict(FILE *out) {
		alignas(4) uint8_t source[PREDICT_SOURCE];
		alignas(4) uint8_t picture[PREDICT_PICTURE], expected[PREDICT_PICTURE];
		for (auto &pixel : source) {
			// Mostly the extremes, to catch carries between pixels
			const uint32_t r = random();
			pixel = (r & 0x300) == 0 ? 0 : (r & 0x300) == 0x100 ? 255 : static_cast<uint8_t>(r);
		}
		for (uint32_t p = 0; p < PREDICT_PICTURE; p++) {
			picture[p] = expected[p] = static_cast<uint8_t>(random());
		}
		const uint32_t size = (random() & 1) ? 16 : 8;
		const uint32_t halfPel = random() & 3;
		const uint32_t rows = (random() & 1) ? size : size / 2;
		const uint32_t offset = random() & 3;
		if (size == 16) {
			videoPredictBlockHalfPel<16, false>(expected + PREDICT_COLUMN, source + offset, PREDICT_STRIDE, halfPel, rows);
		} else {
			videoPredictBlockHalfPel<8, false>(expected + PREDICT_COLUMN, source + offset, PREDICT_STRIDE, halfPel, rows);
		}
		put32(out, 2);
		put32(out, size);
		put32(out, halfPel);
		put32(out, rows);
		put32(out, offset);
		fwrite(source, 1, PREDICT_SOURCE, out);
		fwrite(picture, 1, PREDICT_PICTURE, out);
		fwrite(expected, 1, PREDICT_PICTURE, out);
	}
}

int main(const int argc, char *argv[]) {
	if (argc != 3) {
		std::fprintf(stderr, "Usage: %s <output file> <cases of each>\n", argv[0]);
		return 2;
	}
	FILE *out = std::fopen(argv[1], "wb");
	if (!out) {
		std::perror(argv[1]);
		return 1;
	}
	const long cases = std::strtol(argv[2], nullptr, 10);
	for (long i = 0; i < cases; i++) {
		writeIdct(out);
		writePredict(out);
	}
	return std::fclose(out) == 0 ? 0 : 1;
}
