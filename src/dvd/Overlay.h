#pragma once

#include <array>
#include <cstdint>

#include "NavPack.h"
#include "SubPicture.h"

namespace Dvd {
	// A sub-picture palette entry (0x00YYCrCb) as 0x00RRGGBB
	uint32_t paletteRgb(uint32_t yCrCb);

	/**
	 * Draws a sub-picture over an RGB picture (0x00RRGGBB, width * height). Inside a highlighted button's area its
	 * highlight colours replace the sub-picture's own.
	 * @param highlight The menu's buttons, or nullptr
	 * @param button The highlighted button, 1-based, 0 for none
	 * @param activated The button is being activated (its action colours)
	 */
	void drawSubPicture(uint32_t *rgb, int width, int height, const SubPicture &picture,
	                    const std::array<uint32_t, 16> &palette, const Highlight *highlight, int button, bool activated);
}
