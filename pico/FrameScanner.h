#pragma once

// Converts decoded frames to RGB565 lines for the screen, scaled to its height: the picture's proportions are
// kept, and it's cropped at the left and right if it's then wider than the screen. Lines come out in display
// order, which is big-endian. Used by the video player on core 1, and timed by the benchmark.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "pico/platform.h"

#include "DisplayModule.h"
#include "mpeg1/Frame.h"

namespace {
	using board::SCREEN_WIDTH;
	using board::SCREEN_HEIGHT;
	// Most source pixels converted for one line, which limits how wide the visible part of a picture can be
	constexpr uint32_t MAX_SOURCE_SPAN = 400;

	// A file that can't be played, rather than a card or decoding problem
	struct Unplayable : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	// BT.601 limited range YCbCr to RGB565. The M0+ has few registers, so red, green and blue travel together
	// in one 32-bit value: red in bits 31-22, green in 21-11, blue in 10-0. Each field is its component plus
	// 512, which keeps it positive and in range for any input, so a pixel is one addition and a range check.
	constexpr int FIELD_BIAS = 512;
	constexpr int RED_SHIFT = 22;
	constexpr int GREEN_SHIFT = 11;
	// Set if a field isn't in 512..767, i.e. its component needs clamping
	constexpr uint32_t RANGE_MASK = 0xC0380700;
	constexpr uint32_t RANGE_EXPECTED = 0x80100200;

	constexpr uint32_t packFields(const int32_t red, const int32_t green, const int32_t blue) {
		return (static_cast<uint32_t>(red) << RED_SHIFT) + (static_cast<uint32_t>(green) << GREEN_SHIFT) + static_cast<uint32_t>(blue);
	}

	// In SRAM, for speed
	uint32_t s_lumaTable[256], s_crTable[256], s_cbTable[256];

	void initTables() {
		for (int i = 0; i < 256; i++) {
			const int32_t luma = ((i - 16) * 76309 + 32768) >> 16;
			const int32_t chroma = i - 128;
			s_lumaTable[i] = packFields(luma, luma, luma);
			s_crTable[i] = packFields(FIELD_BIAS + ((chroma * 104597 + 32768) >> 16), FIELD_BIAS - ((chroma * 53278 + 32768) >> 16), FIELD_BIAS);
			s_cbTable[i] = packFields(0, -((chroma * 25674 + 32768) >> 16), (chroma * 132201 + 32768) >> 16);
		}
	}

	// RGB565
	inline uint16_t packedToPixel(const uint32_t fields) {
		// Shifts instead of masks, which would need more registers
		const uint32_t red = (fields << 2) >> 27;
		const uint32_t green = (fields << 13) >> 26;
		const uint32_t blue = (fields << 24) >> 27;
		return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
	}

	__attribute__((noinline)) uint16_t __not_in_flash_func(clampedPixel)(const uint32_t fields) {
		const int32_t red = std::clamp<int32_t>(static_cast<int32_t>(fields >> RED_SHIFT) - FIELD_BIAS, 0, 255);
		const int32_t green = std::clamp<int32_t>(static_cast<int32_t>((fields >> GREEN_SHIFT) & 0x7FF) - FIELD_BIAS, 0, 255);
		const int32_t blue = std::clamp<int32_t>(static_cast<int32_t>(fields & 0x7FF) - FIELD_BIAS, 0, 255);
		return packedToPixel(packFields(FIELD_BIAS + red, FIELD_BIAS + green, FIELD_BIAS + blue));
	}

	inline uint16_t pixel(const uint8_t luma, const uint32_t chroma) {
		const uint32_t fields = s_lumaTable[luma] + chroma;
		if ((fields & RANGE_MASK) != RANGE_EXPECTED) {
			return clampedPixel(fields);
		}
		return packedToPixel(fields);
	}

	// Ordered dither, by row and column: 16-bit colour otherwise shows flat areas as visible bands.
	// Red and blue lose 3 bits, green 2.
	constexpr uint32_t ditherFields(const uint32_t level) {
		return packFields(level * 2, level, level * 2);
	}
	constexpr uint32_t DITHER[2][2] = {
		{ditherFields(0), ditherFields(2)},
		{ditherFields(3), ditherFields(1)}
	};

	// Averages two RGB565 pixels
	inline uint16_t average(const uint16_t a, const uint16_t b) {
		return static_cast<uint16_t>(((a & 0xF7DEu) + (b & 0xF7DEu)) >> 1);
	}

	// a weighted towards b by quarters, 0 to 3
	template <uint32_t QUARTERS>
	inline uint16_t blend(const uint16_t a, const uint16_t b) {
		if constexpr (QUARTERS == 0) {
			return a;
		} else if constexpr (QUARTERS == 1) {
			return average(a, average(a, b));
		} else if constexpr (QUARTERS == 2) {
			return average(a, b);
		} else {
			return average(average(a, b), b);
		}
	}

	// How pictures map to the screen. Core 0 rebuilds it while core 1 is idle; core 1 reads it during a scan.
	// Positions are in quarters of a source pixel: the pixel, and how far towards the next one to blend.
	struct ScalePlan {
		uint32_t sourceWidth = 0, sourceHeight = 0;
		// Screen columns shown, the rest stay black
		uint32_t outputX = 0, outputWidth = 0;
		// Source columns converted for each line, starting and ending on a chroma pair
		uint32_t firstColumn = 0, columnCount = 0;
		// Relative to firstColumn
		uint16_t columns[SCREEN_WIDTH];
		uint16_t rows[SCREEN_HEIGHT];
	};

	ScalePlan s_plan;

	// Fills the screen's height, keeping the picture's proportions, and crops the sides equally if it's wider
	void buildPlan(const uint32_t width, const uint32_t height) {
		if (width == 0 || height == 0) {
			throw Unplayable("Picture has no size");
		}
		// Source pixels per screen pixel, 16.16 fixed point
		const uint32_t step = (height << 16) / SCREEN_HEIGHT;
		// Where a screen pixel's centre falls in the source, clamped to the picture
		const auto position = [step](const uint32_t screen, const uint32_t sourceSize) {
			const int64_t fixed = (static_cast<int64_t>(2 * screen + 1) * step) / 2 - 0x8000;
			const int64_t quarters = (fixed + 0x2000) >> 14;
			return static_cast<uint32_t>(std::clamp<int64_t>(quarters, 0, (sourceSize - 1) * 4));
		};

		const uint32_t scaledWidth = std::max<uint32_t>(1, (width * SCREEN_HEIGHT + height / 2) / height);
		const uint32_t outputWidth = std::min(SCREEN_WIDTH, scaledWidth);
		const uint32_t crop = (scaledWidth - outputWidth) / 2;

		const uint32_t first = position(crop, width);
		const uint32_t last = position(crop + outputWidth - 1, width);
		// Blending with the next column needs it converted too
		const uint32_t firstColumn = (first / 4) & ~1u;
		const uint32_t lastColumn = (last / 4 + (last % 4 ? 1 : 0)) | 1u;
		const uint32_t columnCount = lastColumn - firstColumn + 1;
		if (columnCount > MAX_SOURCE_SPAN) {
			throw Unplayable("Picture too wide to scale");
		}

		s_plan.sourceWidth = width;
		s_plan.sourceHeight = height;
		s_plan.outputX = (SCREEN_WIDTH - outputWidth) / 2;
		s_plan.outputWidth = outputWidth;
		s_plan.firstColumn = firstColumn;
		s_plan.columnCount = columnCount;
		for (uint32_t x = 0; x < outputWidth; x++) {
			s_plan.columns[x] = static_cast<uint16_t>(position(crop + x, width) - firstColumn * 4);
		}
		for (uint32_t y = 0; y < SCREEN_HEIGHT; y++) {
			s_plan.rows[y] = static_cast<uint16_t>(position(y, height));
		}
		printf("Picture %lux%lu, shown %lux%lu, %lu columns cropped\n", width, height, outputWidth, SCREEN_HEIGHT,
		       scaledWidth - outputWidth);
	}

	// Converts the plan's columns of a source line to RGB565
	void __not_in_flash_func(convertLine)(const Frame &frame, const uint32_t row, uint16_t *out) {
		const uint32_t firstColumn = s_plan.firstColumn;
		const uint8_t *luma = frame.m_yData + row * frame.m_frameSize.luma_width + firstColumn;
		const uint8_t *cr = frame.m_crData + (row / 2) * frame.m_frameSize.chroma_width + firstColumn / 2;
		const uint8_t *cb = frame.m_cbData + (row / 2) * frame.m_frameSize.chroma_width + firstColumn / 2;
		const uint32_t ditherEven = DITHER[row & 1][0], ditherOdd = DITHER[row & 1][1];
		for (const uint16_t *end = out + s_plan.columnCount; out != end; out += 2) {
			const uint32_t chroma = s_crTable[*cr++] + s_cbTable[*cb++];
			out[0] = pixel(luma[0], chroma + ditherEven);
			out[1] = pixel(luma[1], chroma + ditherOdd);
			luma += 2;
		}
	}

	// Scales a converted source line to the screen's width
	void __not_in_flash_func(scaleLine)(const uint16_t *in, uint16_t *out) {
		const uint16_t *columns = s_plan.columns;
		for (const uint16_t *end = out + s_plan.outputWidth; out != end; out++) {
			const uint32_t position = *columns++;
			const uint16_t *source = in + (position >> 2);
			switch (position & 3) {
				case 0: *out = source[0]; break;
				case 1: *out = blend<1>(source[0], source[1]); break;
				case 2: *out = blend<2>(source[0], source[1]); break;
				default: *out = blend<3>(source[0], source[1]); break;
			}
		}
	}

	// Blends two scaled lines into display order, which is big-endian
	template <uint32_t QUARTERS>
	void __not_in_flash_func(blendLines)(const uint16_t *a, const uint16_t *b, uint16_t *out, const uint32_t width) {
		for (uint32_t x = 0; x < width; x++) {
			out[x] = __builtin_bswap16(blend<QUARTERS>(a[x], b[x]));
		}
	}

	// Produces a frame's screen lines in order, converting and scaling each source line once
	class FrameScanner {
		// One converted source line, and the last two source lines scaled to the screen's width
		uint16_t m_source[MAX_SOURCE_SPAN];
		uint16_t m_scaled[2][SCREEN_WIDTH];
		int32_t m_scaledRow[2] = {-1, -1};
		const Frame *m_frame = nullptr;

		// A source line scaled, reusing the last two, and not replacing the slot given in keep
		int __not_in_flash_func(scaledLine)(const uint32_t row, const int keep) {
			for (int slot = 0; slot < 2; slot++) {
				if (m_scaledRow[slot] == static_cast<int32_t>(row)) {
					return slot;
				}
			}
			// Rows only go down the picture, so the older line is the one to replace
			const int slot = keep >= 0 ? keep ^ 1 : m_scaledRow[0] < m_scaledRow[1] ? 0 : 1;
			convertLine(*m_frame, row, m_source);
			scaleLine(m_source, m_scaled[slot]);
			m_scaledRow[slot] = static_cast<int32_t>(row);
			return slot;
		}

	public:
		void begin(const Frame &frame) {
			m_frame = &frame;
			m_scaledRow[0] = m_scaledRow[1] = -1;
		}

		// Screen line y, s_plan.outputWidth pixels, into out. Lines must come in order from 0.
		void __not_in_flash_func(line)(const uint32_t y, uint16_t *out) {
			const uint32_t width = s_plan.outputWidth;
			const uint32_t position = s_plan.rows[y];
			const int slotA = scaledLine(position >> 2, -1);
			const uint16_t *a = m_scaled[slotA];
			if ((position & 3) == 0) {
				blendLines<0>(a, a, out, width);
				return;
			}
			const uint16_t *b = m_scaled[scaledLine((position >> 2) + 1, slotA)];
			switch (position & 3) {
				case 1: blendLines<1>(a, b, out, width); break;
				case 2: blendLines<2>(a, b, out, width); break;
				default: blendLines<3>(a, b, out, width); break;
			}
		}
	};
}
