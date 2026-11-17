#pragma once

// Shared between the MPEG-1 (Frame.cpp) and MPEG-2 (Mpeg2Video.cpp) video decoding

#include <cstdint>

// Scale factors the IDCT expects its coefficients premultiplied by, raster order, with 8 extra bits of precision
extern const uint16_t VIDEO_PREMULTIPLIER_MATRIX[64];

// The IDCT works at 2^VIDEO_IDCT_SHIFT times the samples' scale, and rounds by adding VIDEO_IDCT_ROUNDING to the
// first coefficient
constexpr int32_t VIDEO_IDCT_SHIFT = 9;
constexpr int32_t VIDEO_IDCT_ROUNDING = 1 << (VIDEO_IDCT_SHIFT - 1);

// The first (DC) coefficient's premultiplier is exactly 1 << VIDEO_DC_PREMULTIPLY_SHIFT, so intra DC values are
// premultiplied by a shift
constexpr int32_t VIDEO_DC_PREMULTIPLY_SHIFT = 6;

// A dequantised coefficient (-2048..2047) at the given raster position, premultiplied for the IDCT. Rounding the
// product, rather than premultiplying by a rounded table, is what meets IEEE 1180's accuracy.
static inline int32_t videoPremultiply(const int32_t level, const uint32_t position) {
	return (level * VIDEO_PREMULTIPLIER_MATRIX[position] + 128) >> 8;
}

// The sample value of a block whose only coefficient is the premultiplied first one; the same as the IDCT gives
static inline int32_t videoDcValue(const int32_t premultipliedDc) {
	return (premultipliedDc + VIDEO_IDCT_ROUNDING) >> VIDEO_IDCT_SHIFT;
}

// In-place 8x8 inverse DCT, accurate to IEEE 1180-1990. block holds 64 premultiplied coefficients in raster order,
// and receives the samples.
void video_idct(int32_t *block);

// Whether a macroblock's prediction at (mbCol, mbRow), with a motion vector in half pels, reads only inside a
// reference plane mbWidth x mbHeight blocks of blockSize pixels, counting the extra column and row half pel
// prediction averages in. Valid MPEG-1 streams never point outside (11172-2 has no unrestricted vectors). The
// decoder checks luma only (blockSize 16): the chroma vector is the luma one halved, truncated, and is then
// always inside the chroma plane too, which the tests check exhaustively.
static inline bool videoMotionInRange(const uint32_t mbCol, const uint32_t mbRow, const uint32_t mbWidth,
                                      const uint32_t mbHeight, const int32_t motionH, const int32_t motionV,
                                      const uint32_t blockSize = 16) {
	if (mbCol >= mbWidth || mbRow >= mbHeight) {
		return false;
	}
	const auto size = static_cast<int32_t>(blockSize);
	const int32_t x = static_cast<int32_t>(mbCol) * size + (motionH >> 1);
	const int32_t y = static_cast<int32_t>(mbRow) * size + (motionV >> 1);
	return x >= 0 && y >= 0 && x + size + (motionH & 1) <= static_cast<int32_t>(mbWidth) * size &&
	       y + size + (motionV & 1) <= static_cast<int32_t>(mbHeight) * size;
}

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
