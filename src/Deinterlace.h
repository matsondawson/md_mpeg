#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/**
 * Deinterlacing of RGB frames whose two fields are woven together (even lines the top field, odd lines the
 * bottom field). Each function makes one field into a full-height picture, to be shown at that field's time, so
 * interlaced video plays at twice its frame rate.
 */
namespace Deinterlace {
	enum class Mode {
		// Both fields shown at once, as decoded
		Off,
		// Each field on its own, the other field's lines interpolated from the lines above and below
		Bob,
		// Motion adaptive: still areas keep the other field's lines, moving areas are interpolated
		Yadif
	};

	inline const char *name(const Mode mode) {
		switch (mode) {
			case Mode::Off: return "off";
			case Mode::Bob: return "bob";
			default: return "yadif";
		}
	}

	/**
	 * One field at full height: its lines, and between them the average of the lines above and below
	 * @param top the top field (even lines), else the bottom field (odd lines)
	 */
	inline void bobField(const uint32_t *woven, uint32_t *out, const uint32_t width, const uint32_t height,
	                     const bool top) {
		const uint32_t parity = top ? 0 : 1;
		for (uint32_t y = 0; y < height; y++) {
			uint32_t *row = out + static_cast<size_t>(y) * width;
			// The nearest lines of the field, the same one at the edges
			const uint32_t above = y > 0 ? y - 1 : y + 1;
			const uint32_t below = y + 1 < height ? y + 1 : y - 1;
			if ((y & 1) == parity || above >= height || below >= height) {
				// A line of the field, or a one line picture
				std::memcpy(row, woven + static_cast<size_t>(y) * width, width * sizeof(uint32_t));
				continue;
			}
			const uint32_t *a = woven + static_cast<size_t>(above) * width;
			const uint32_t *b = woven + static_cast<size_t>(below) * width;
			for (uint32_t x = 0; x < width; x++) {
				// Average of each byte, rounding up
				row[x] = (a[x] | b[x]) - (((a[x] ^ b[x]) >> 1) & 0x7F7F7F7F);
			}
		}
	}

	/**
	 * One field at full height by yadif ("yet another deinterlacing filter", as in FFmpeg's vf_yadif by Michael
	 * Niedermayer, mode 1): each missing line is predicted spatially from the lines above and below, along the
	 * direction of the best matching edge, then limited by how much that pixel changes between the other field
	 * before and after this one. Where nothing moves the other field's line is kept, so still detail stays sharp
	 * and steady; where it moves the spatial prediction is used, so there is no combing. Each byte of a pixel
	 * is filtered on its own.
	 *
	 * @param previous the frame before, cur itself if there is none (e.g. after a seek)
	 * @param next the frame after, cur itself if it hasn't been decoded
	 * @param top the field to show: the top field (even lines), else the bottom
	 * @param first the field shown is the frame's first field
	 */
	void yadifField(const uint32_t *previous, const uint32_t *cur, const uint32_t *next, uint32_t *out,
	                uint32_t width, uint32_t height, bool top, bool first);
}
