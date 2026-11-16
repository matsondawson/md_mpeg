#pragma once

// Four 8-bit pixels in one 32-bit word, the first pixel in the low byte, for motion compensation. Every
// operation gives the same result as doing it one pixel at a time. Without unaligned loads (Armv6-M, e.g. the
// RP2040's Cortex-M0+), a row is read as aligned words shifted together.

#include <cstdint>
#include <cstring>

#if defined(__ARM_ARCH_6M__) || defined(MD_MPEG_TEST_ALIGNED_LOADS)
#define MD_MPEG_PIXEL_WORDS_ALIGNED_LOADS 1
#else
#define MD_MPEG_PIXEL_WORDS_ALIGNED_LOADS 0
#endif

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Pixel words assume little-endian memory");

namespace PixelWords {
	inline uint32_t loadAligned(const uint8_t *p) {
		uint32_t value;
		std::memcpy(&value, __builtin_assume_aligned(p, 4), 4);
		return value;
	}

	inline void storeAligned(uint8_t *p, const uint32_t value) {
		std::memcpy(__builtin_assume_aligned(p, 4), &value, 4);
	}

	// Reads WORDS words of pixels from p, at any alignment. Frame buffers are whole words long and word
	// aligned, so the aligned words read never go past the buffer.
	template <int WORDS>
	inline void loadRow(const uint8_t *p, uint32_t *out) {
#if MD_MPEG_PIXEL_WORDS_ALIGNED_LOADS
		const uint32_t offset = reinterpret_cast<uintptr_t>(p) & 3;
		const uint8_t *aligned = p - offset;
		if (offset == 0) {
			for (int i = 0; i < WORDS; i++) {
				out[i] = loadAligned(aligned + 4 * i);
			}
			return;
		}
		const uint32_t shift = offset * 8;
		uint32_t word = loadAligned(aligned);
		for (int i = 0; i < WORDS; i++) {
			const uint32_t next = loadAligned(aligned + 4 * (i + 1));
			out[i] = (word >> shift) | (next << (32 - shift));
			word = next;
		}
#else
		std::memcpy(out, p, 4 * WORDS);
#endif
	}

	// (a + b + 1) >> 1 for each pixel
	inline uint32_t average2(const uint32_t a, const uint32_t b) {
		return (a | b) - (((a ^ b) >> 1) & 0x7F7F7F7Fu);
	}

	// (a + b + c + d + 2) >> 2 for each pixel, summed in 16-bit lanes: pixels 0 and 2, then 1 and 3
	inline uint32_t average4(const uint32_t a, const uint32_t b, const uint32_t c, const uint32_t d) {
		constexpr uint32_t mask = 0x00FF00FFu;
		const uint32_t even = (a & mask) + (b & mask) + (c & mask) + (d & mask) + 0x00020002u;
		const uint32_t odd = ((a >> 8) & mask) + ((b >> 8) & mask) + ((c >> 8) & mask) + ((d >> 8) & mask) + 0x00020002u;
		return ((even >> 2) & 0x00FF00FFu) | ((odd << 6) & 0xFF00FF00u);
	}
}
