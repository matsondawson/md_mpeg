#include "Overlay.h"

#include <algorithm>

namespace Dvd {
	uint32_t paletteRgb(const uint32_t yCrCb) {
		// BT.601 limited range, as the video
		const double y = (static_cast<int>(yCrCb >> 16 & 255) - 16) * 1.164;
		const int cr = static_cast<int>(yCrCb >> 8 & 255) - 128;
		const int cb = static_cast<int>(yCrCb & 255) - 128;
		const auto clamp = [](const double v) { return static_cast<uint32_t>(std::clamp(v + 0.5, 0.0, 255.0)); };
		return clamp(y + 1.596 * cr) << 16 | clamp(y - 0.392 * cb - 0.813 * cr) << 8 | clamp(y + 2.017 * cb);
	}

	void drawSubPicture(uint32_t *rgb, const int width, const int height, const SubPicture &picture,
	                    const std::array<uint32_t, 16> &palette, const Highlight *highlight, const int button,
	                    const bool activated) {
		if (picture.empty()) {
			return;
		}
		std::array<uint32_t, 16> colors{};
		for (int i = 0; i < 16; i++) {
			colors[i] = paletteRgb(palette[i]);
		}
		const Button *highlighted = nullptr;
		const Highlight::Colors *buttonColors = nullptr;
		if (highlight && button >= 1 && button <= static_cast<int>(highlight->buttons.size())) {
			highlighted = &highlight->buttons[button - 1];
			if (highlighted->colorTable >= 1) {
				buttonColors = &highlight->colors[highlighted->colorTable - 1][activated ? 1 : 0];
			}
		}
		// Contrast 0 is transparent, 15 opaque, in 16ths
		const auto alpha = [](const uint8_t contrast) { return contrast ? contrast + 1 : 0; };

		for (int row = 0; row < picture.height; row++) {
			const int y = picture.y + row;
			if (y < 0 || y >= height) {
				continue;
			}
			const bool rowInButton = buttonColors && y >= highlighted->y1 && y <= highlighted->y2;
			for (int column = 0; column < picture.width; column++) {
				const int x = picture.x + column;
				if (x < 0 || x >= width) {
					continue;
				}
				const uint8_t value = picture.pixels[static_cast<size_t>(row) * picture.width + column];
				uint8_t color = picture.colors[value];
				int a = alpha(picture.contrast[value]);
				if (rowInButton && x >= highlighted->x1 && x <= highlighted->x2) {
					color = buttonColors->color[value];
					a = alpha(buttonColors->contrast[value]);
				}
				if (!a) {
					continue;
				}
				uint32_t &out = rgb[static_cast<size_t>(y) * width + x];
				const uint32_t c = colors[color];
				const auto mix = [a](const uint32_t over, const uint32_t under) { return (over * a + under * (16 - a)) / 16; };
				out = mix(c >> 16 & 255, out >> 16 & 255) << 16 | mix(c >> 8 & 255, out >> 8 & 255) << 8 | mix(c & 255, out & 255);
			}
		}
	}
}
