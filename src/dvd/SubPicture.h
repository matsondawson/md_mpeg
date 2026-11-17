#pragma once

// DVD sub-pictures: the 2-bit run-length coded overlays used for subtitles and menu buttons, carried in private
// stream 1 sub-streams 0x20-0x3F. Format as documented at dvd.sourceforge.net (Sub-Picture Unit).

#include <array>
#include <cstdint>
#include <vector>

#include "../stream/MStream.h"

namespace Dvd {
	struct SubPicture {
		// Display area, in picture pixels, inclusive
		int x = 0, y = 0, width = 0, height = 0;
		// 2-bit pixel values, width * height
		std::vector<uint8_t> pixels;
		// For pixel values 0-3: palette index, and contrast 0 (transparent) to 15 (opaque)
		std::array<uint8_t, 4> colors{};
		std::array<uint8_t, 4> contrast{};
		// Shown from the unit's PTS plus startMs until its PTS plus stopMs (-1 = until the next unit)
		int64_t ptsMs = -1;
		int32_t startMs = 0;
		int32_t stopMs = -1;
		// Shown even when sub-pictures are switched off (e.g. menu buttons, forced subtitles)
		bool forced = false;

		[[nodiscard]] bool empty() const { return pixels.empty(); }
	};

	/**
	 * Decodes one sub-picture unit (SPU).
	 * @return false if it is damaged
	 */
	bool decodeSubPicture(const std::vector<uint8_t> &unit, SubPicture &out);

	// Collects a sub-picture stream's packets into units and decodes them
	class SubPictureDecoder final : public MStream::IWriteByteStream {
		std::vector<uint8_t> m_unit;
		uint32_t m_unitSize = 0;
		int32_t m_ptsMillis = -1;
		int32_t m_unitPtsMillis = -1;
		std::vector<SubPicture> m_decoded;

	public:
		int32_t getPts(int32_t) override { return m_ptsMillis; }
		void setPts(const int32_t ptsMillis) override { m_ptsMillis = ptsMillis; }
		void writeByte(uint8_t data) override;
		[[nodiscard]] bool isFull() override { return false; }
		void executeCallbacks() override {}
		void flush() override {}
		void discontinuity() override { reset(); }

		// Takes the units decoded since the last call
		std::vector<SubPicture> take();
		void reset();
	};
}
