#include "Deinterlace.h"

namespace Deinterlace {
	namespace detail {
		/**
		 * Yadif for one byte of a missing line, written without branches so the compiler can vectorise a row
		 * @param c0 the line in the current frame; up and down are the offsets to the lines above and below
		 * @param p0, n0 the line in the frames before and after
		 * @param pp, nn the line in the frames whose other field is just before and after this field
		 */
		template<bool EdgeDirected, bool SpatialCheck>
		inline uint8_t yadifByte(const uint8_t *c0, const uint8_t *p0, const uint8_t *n0, const uint8_t *pp,
		                         const uint8_t *nn, const std::ptrdiff_t up, const std::ptrdiff_t down,
		                         const std::ptrdiff_t i) {
			const int c = c0[up + i];
			const int e = c0[down + i];
			const int d = (pp[i] + nn[i]) >> 1;
			const int temporal0 = std::abs(pp[i] - nn[i]);
			const int temporal1 = (std::abs(p0[up + i] - c) + std::abs(p0[down + i] - e)) >> 1;
			const int temporal2 = (std::abs(n0[up + i] - c) + std::abs(n0[down + i] - e)) >> 1;
			int diff = std::max(std::max(temporal0 >> 1, temporal1), temporal2);

			int spatial = (c + e) >> 1;
			if constexpr (EdgeDirected) {
				// Edge directed: the pair of pixels above and below, on a slope of up to 2 pixels, that match best
				// over 3 pixels. A slope of 2 is only tried if 1 was better than none.
				const auto score = [&](const std::ptrdiff_t o) {
					return std::abs(c0[up + i - 4 + o] - c0[down + i - 4 - o]) + std::abs(c0[up + i + o] - c0[down + i - o])
					       + std::abs(c0[up + i + 4 + o] - c0[down + i + 4 - o]);
				};
				const auto along = [&](const std::ptrdiff_t o) { return (c0[up + i + o] + c0[down + i - o]) >> 1; };
				// Selects without branches (all ones mask: a, else b), which the vectoriser handles
				const auto select = [](const int mask, const int a, const int b) { return (a & mask) | (b & ~mask); };
				int best = score(0) - 1;
				const int left1 = score(-4);
				const int takeLeft1 = -static_cast<int>(left1 < best);
				best = select(takeLeft1, left1, best);
				spatial = select(takeLeft1, along(-4), spatial);
				const int left2 = score(-8);
				const int takeLeft2 = takeLeft1 & -static_cast<int>(left2 < best);
				best = select(takeLeft2, left2, best);
				spatial = select(takeLeft2, along(-8), spatial);
				const int right1 = score(4);
				const int takeRight1 = -static_cast<int>(right1 < best);
				best = select(takeRight1, right1, best);
				spatial = select(takeRight1, along(4), spatial);
				const int right2 = score(8);
				const int takeRight2 = takeRight1 & -static_cast<int>(right2 < best);
				spatial = select(takeRight2, along(8), spatial);
			}

			if constexpr (SpatialCheck) {
				const int b = (pp[2 * up + i] + nn[2 * up + i]) >> 1;
				const int f = (pp[2 * down + i] + nn[2 * down + i]) >> 1;
				const int maxDiff = std::max(std::max(d - e, d - c), std::min(b - c, f - e));
				const int minDiff = std::min(std::min(d - e, d - c), std::max(b - c, f - e));
				diff = std::max(std::max(diff, minDiff), -maxDiff);
			}

			spatial = std::min(std::max(spatial, d - diff), d + diff);
			return static_cast<uint8_t>(std::min(std::max(spatial, 0), 255));
		}

		template<bool SpatialCheck>
		inline void yadifLine(const uint8_t *c0, const uint8_t *p0, const uint8_t *n0, const uint8_t *pp,
		                      const uint8_t *nn, uint8_t *d0, const std::ptrdiff_t up, const std::ptrdiff_t down,
		                      const uint32_t width) {
			const std::ptrdiff_t rowBytes = static_cast<std::ptrdiff_t>(width) * 4;
			// The edge directed search reaches 3 pixels to each side
			const std::ptrdiff_t edgeStart = width >= 7 ? 3 * 4 : rowBytes;
			const std::ptrdiff_t edgeEnd = width >= 7 ? rowBytes - 3 * 4 : rowBytes;
			for (std::ptrdiff_t i = 0; i < edgeStart; i++) {
				d0[i] = yadifByte<false, SpatialCheck>(c0, p0, n0, pp, nn, up, down, i);
			}
			for (std::ptrdiff_t i = edgeStart; i < edgeEnd; i++) {
				d0[i] = yadifByte<true, SpatialCheck>(c0, p0, n0, pp, nn, up, down, i);
			}
			for (std::ptrdiff_t i = std::max(edgeEnd, edgeStart); i < rowBytes; i++) {
				d0[i] = yadifByte<false, SpatialCheck>(c0, p0, n0, pp, nn, up, down, i);
			}
		}
	}


	// Built for AVX2 as well where the compiler can choose the version at run time, which about halves the time
#if defined(__GNUC__) && defined(__x86_64__) && defined(__linux__)
	__attribute__((target_clones("avx2", "default")))
#endif
	void yadifField(const uint32_t *previous, const uint32_t *cur, const uint32_t *next, uint32_t *out,
	                const uint32_t width, const uint32_t height, const bool top, const bool first) {
		if (height < 3) {
			bobField(cur, out, width, height, top);
			return;
		}
		const uint32_t parity = top ? 0 : 1;
		const auto stride = static_cast<std::ptrdiff_t>(width) * 4;
		const auto *prev = reinterpret_cast<const uint8_t *>(previous);
		const auto *cu = reinterpret_cast<const uint8_t *>(cur);
		const auto *nxt = reinterpret_cast<const uint8_t *>(next);
		// The other field at the times either side of this field: for the first field, the second fields of the
		// frame before and this frame; for the second, the first fields of this frame and the next
		const uint8_t *prev2 = first ? prev : cu;
		const uint8_t *next2 = first ? cu : nxt;
		auto *dst = reinterpret_cast<uint8_t *>(out);

		for (uint32_t y = 0; y < height; y++) {
			const std::ptrdiff_t row = static_cast<std::ptrdiff_t>(y) * stride;
			if ((y & 1) == parity) {
				std::memcpy(dst + row, cu + row, static_cast<size_t>(stride));
				continue;
			}
			// Lines above and below, of this field; mirrored at the edges
			const std::ptrdiff_t up = y > 0 ? -stride : stride;
			const std::ptrdiff_t down = y + 1 < height ? stride : -stride;
			// The check against the lines two above and below needs them to exist
			if (y >= 2 && y + 2 < height) {
				detail::yadifLine<true>(cu + row, prev + row, nxt + row, prev2 + row, next2 + row, dst + row, up, down,
				                        width);
			} else {
				detail::yadifLine<false>(cu + row, prev + row, nxt + row, prev2 + row, next2 + row, dst + row, up, down,
				                         width);
			}
		}
	}
}
