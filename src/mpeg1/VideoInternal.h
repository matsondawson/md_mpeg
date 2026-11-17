#pragma once

// Shared between the MPEG-1 (Frame.cpp) and MPEG-2 (Mpeg2Video.cpp) video decoding

#include <cstdint>

// Scale factors the IDCT expects its coefficients premultiplied by, raster order
extern const uint8_t VIDEO_PREMULTIPLIER_MATRIX[64];

// In-place 8x8 inverse DCT. block holds 64 premultiplied coefficients in raster order, and receives the samples.
void video_idct(int32_t *block);

// Predicts a SIZE (16 or 8) wide, rows high block from s into d, both with row stride dw. halfPel is the half pel
// flags, horizontal in bit 1 and vertical in bit 0. INTERPOLATE averages with what d already holds. d is word aligned.
template <uint32_t SIZE, bool INTERPOLATE>
void videoPredictBlockHalfPel(uint8_t *d, const uint8_t *s, uint32_t dw, uint32_t halfPel, uint32_t rows);

// One unsigned compare for the common in-range case. Out of range, ~n's sign bit is set for n > 255 and clear
// for n < 0, so the arithmetic shift gives 255 or 0.
static inline uint8_t clampToByte(const int32_t n) {
	if (static_cast<uint32_t>(n) > 255) [[unlikely]] {
		return static_cast<uint8_t>(~n >> 31);
	}
	return static_cast<uint8_t>(n);
}
