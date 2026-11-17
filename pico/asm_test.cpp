// Compares the Cortex-M0+ assembler (idct_m0.S, predict_m0.S) with the decoder's C on random data, as the
// assembler can only run on the board. Built with the decoder using its C, and reports over USB serial.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"

#include "mpeg1/VideoInternal.h"

extern "C" void video_idct_put_m0(int32_t *block, uint8_t *dest, uint32_t stride, uint32_t masks);
extern "C" void video_idct_add_m0(int32_t *block, uint8_t *dest, uint32_t stride, uint32_t masks);

#define PREDICT_M0_DECLARE(words, hp) \
	extern "C" void video_predict_m0_##words##_##hp##_0(uint8_t *, const uint8_t *, uint32_t, uint32_t); \
	extern "C" void video_predict_m0_##words##_##hp##_1(uint8_t *, const uint8_t *, uint32_t, uint32_t); \
	extern "C" void video_predict_m0_##words##_##hp##_2(uint8_t *, const uint8_t *, uint32_t, uint32_t); \
	extern "C" void video_predict_m0_##words##_##hp##_3(uint8_t *, const uint8_t *, uint32_t, uint32_t);
PREDICT_M0_DECLARE(2, 0) PREDICT_M0_DECLARE(2, 1) PREDICT_M0_DECLARE(2, 2) PREDICT_M0_DECLARE(2, 3)
PREDICT_M0_DECLARE(4, 0) PREDICT_M0_DECLARE(4, 1) PREDICT_M0_DECLARE(4, 2) PREDICT_M0_DECLARE(4, 3)
#define PREDICT_M0_ROW(words, hp) \
	{video_predict_m0_##words##_##hp##_0, video_predict_m0_##words##_##hp##_1, \
	 video_predict_m0_##words##_##hp##_2, video_predict_m0_##words##_##hp##_3}

namespace {
	using PredictM0 = void (*)(uint8_t *, const uint8_t *, uint32_t, uint32_t);
	PredictM0 const PREDICT_M0[2][4][4] = {
		{PREDICT_M0_ROW(2, 0), PREDICT_M0_ROW(2, 1), PREDICT_M0_ROW(2, 2), PREDICT_M0_ROW(2, 3)},
		{PREDICT_M0_ROW(4, 0), PREDICT_M0_ROW(4, 1), PREDICT_M0_ROW(4, 2), PREDICT_M0_ROW(4, 3)},
	};

	uint32_t s_random = 12345;

	uint32_t random() {
		s_random ^= s_random << 13;
		s_random ^= s_random >> 17;
		s_random ^= s_random << 5;
		return s_random;
	}

	constexpr uint32_t WIDTH = 64, HEIGHT = 48;
	// Padded, as the prediction reads up to a word past a block's right edge and a row below it
	alignas(4) uint8_t s_source[WIDTH * (HEIGHT + 2)];
	alignas(4) uint8_t s_expected[WIDTH * HEIGHT], s_actual[WIDTH * HEIGHT];

	uint32_t testPredict(const uint32_t cases) {
		uint32_t failures = 0;
		for (uint32_t i = 0; i < cases; i++) {
			for (auto &pixel : s_source) {
				// Mostly the extremes, to catch carries between pixels
				const uint32_t r = random();
				pixel = (r & 0x300) == 0 ? 0 : (r & 0x300) == 0x100 ? 255 : static_cast<uint8_t>(r);
			}
			for (uint32_t p = 0; p < sizeof(s_expected); p++) {
				s_expected[p] = s_actual[p] = static_cast<uint8_t>(random());
			}
			const bool big = random() & 1;
			const uint32_t size = big ? 16 : 8;
			const uint32_t halfPel = random() & 3;
			const uint32_t rows = (random() & 1) ? size : size / 2;
			const uint32_t sx = random() % (WIDTH - size - 1), sy = random() % (HEIGHT - size - 1);
			const uint32_t dx = (random() % (WIDTH / 8 - 1)) * 8, dy = random() % (HEIGHT - size);
			const uint8_t *source = s_source + sy * WIDTH + sx;
			if (big) {
				videoPredictBlockHalfPel<16, false>(s_expected + dy * WIDTH + dx, source, WIDTH, halfPel, rows);
			} else {
				videoPredictBlockHalfPel<8, false>(s_expected + dy * WIDTH + dx, source, WIDTH, halfPel, rows);
			}
			const uint32_t offset = reinterpret_cast<uintptr_t>(source) & 3;
			PREDICT_M0[big][halfPel][offset](s_actual + dy * WIDTH + dx, source - offset, WIDTH, rows);
			if (memcmp(s_expected, s_actual, sizeof(s_expected)) != 0) {
				if (failures++ < 8) {
					uint32_t at = 0;
					while (s_expected[at] == s_actual[at]) {
						at++;
					}
					printf("predict FAIL size %lu half pel %lu offset %lu rows %lu: pixel %lu,%lu expected %u got %u\n", size,
					       halfPel, offset, rows, at % WIDTH - dx, at / WIDTH - dy, s_expected[at], s_actual[at]);
				}
			}
		}
		return failures;
	}

	uint32_t idctMaskBits(const uint32_t index) {
		return (index >= 8 ? 3u : 2u) << (2 * (index & 7));
	}

	uint32_t testIdct(const uint32_t cases) {
		uint32_t failures = 0;
		alignas(4) int32_t block[64], reference[64];
		alignas(4) uint8_t expected[8 * 24], actual[8 * 24];
		constexpr uint32_t STRIDE = 24;
		memset(block, 0, sizeof(block));
		for (uint32_t i = 0; i < cases; i++) {
			memset(reference, 0, sizeof(reference));
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
				reference[index] = ((random() & 3) ? small : level) * VIDEO_PREMULTIPLIER_MATRIX[index];
				masks |= idctMaskBits(index);
			}
			const bool add = random() & 1;
			for (uint32_t p = 0; p < sizeof(expected); p++) {
				expected[p] = actual[p] = static_cast<uint8_t>(random());
			}
			memcpy(block, reference, sizeof(block));
			video_idct(reference);
			for (uint32_t y = 0; y < 8; y++) {
				for (uint32_t x = 0; x < 8; x++) {
					uint8_t &pixel = expected[y * STRIDE + 8 + x];
					pixel = clampToByte(add ? pixel + reference[y * 8 + x] : reference[y * 8 + x]);
				}
			}
			if (add) {
				video_idct_add_m0(block, actual + 8, STRIDE, masks);
			} else {
				video_idct_put_m0(block, actual + 8, STRIDE, masks);
			}
			bool cleared = true;
			for (const int32_t value : block) {
				cleared = cleared && value == 0;
			}
			if (!cleared || memcmp(expected, actual, sizeof(expected)) != 0) {
				if (failures++ < 8) {
					uint32_t at = 0;
					while (at < sizeof(expected) && expected[at] == actual[at]) {
						at++;
					}
					printf("idct FAIL %s shape %lu masks %04lx%s: pixel %lu expected %u got %u\n", add ? "add" : "put", shape, masks,
					       cleared ? "" : " block not cleared", at, expected[at], actual[at]);
				}
				memset(block, 0, sizeof(block));
			}
		}
		return failures;
	}
}

int main() {
	stdio_init_all();
	while (!stdio_usb_connected()) {
		sleep_ms(50);
	}
	sleep_ms(500);
	const uint32_t predictFailures = testPredict(20000);
	const uint32_t idctFailures = testIdct(20000);
	for (;;) {
		printf("asm_test: predict %lu of 20000 failed, idct %lu of 20000 failed\n", predictFailures, idctFailures);
		printf("asm_test done\n");
		sleep_ms(2000);
	}
}
