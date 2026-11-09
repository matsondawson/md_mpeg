#pragma once

// Common code for monochrome OLED controllers, SSD1306 and SH1106, up to 128x64.
//
// Their memory is in pages: each byte is a column of 8 pixels, the lowest bit at the top. Pixels can't be
// written one at a time, so the driver keeps a frame buffer, turns incoming pixels to on or off with an
// ordered dither on their luma, and sends the pages a window touched when it ends.

#include <algorithm>
#include <cstdint>

#include "Display.h"

namespace display {
	struct MonoPanel {
		uint16_t panelWidth, panelHeight;
		// Upside down, for modules mounted the other way. Quarter turns aren't supported.
		bool rotate180;

		constexpr uint16_t width() const {
			return panelWidth;
		}
		constexpr uint16_t height() const {
			return panelHeight;
		}
	};

	class MonoOled : public Display {
	public:
		static constexpr int MAX_WIDTH = 128;
		static constexpr int MAX_HEIGHT = 64;

	protected:
		const MonoPanel m_panel;
		uint8_t m_buffer[MAX_WIDTH * MAX_HEIGHT / 8] = {};

	private:
		// The window being written, and the next pixel in it
		int m_windowX = 0, m_windowY = 0, m_windowRight = 0, m_windowBottom = 0;
		int m_x = 0, m_y = 0;

		// 4x4 Bayer thresholds, so flat greys come out as even patterns
		static constexpr uint8_t THRESHOLDS[4][4] = {
			{8, 136, 40, 168},
			{200, 72, 232, 104},
			{56, 184, 24, 152},
			{248, 120, 216, 88}
		};

	protected:
		MonoOled(Bus &bus, const MonoPanel &panel) : Display(bus, panel.width(), panel.height()), m_panel(panel) {
		}

		// Sends pages firstPage to lastPage of the frame buffer
		virtual void sendPages(int firstPage, int lastPage) = 0;

	public:
		void setPixel(const int x, const int y, const bool on) {
			uint8_t &byte = m_buffer[(y >> 3) * m_width + x];
			const uint8_t bit = static_cast<uint8_t>(1 << (y & 7));
			byte = on ? byte | bit : byte & ~bit;
		}

		bool pixel(const int x, const int y) const {
			return m_buffer[(y >> 3) * m_width + x] & (1 << (y & 7));
		}

		void beginPixels(const int x, const int y, const int width, const int height) override {
			m_windowX = m_x = x;
			m_windowY = m_y = y;
			m_windowRight = x + width;
			m_windowBottom = y + height;
		}

		void sendPixels(const uint8_t *pixels, const uint32_t bytes) override {
			for (const uint8_t *end = pixels + bytes; pixels != end && m_y < m_windowBottom; pixels += 2) {
				const uint16_t color = static_cast<uint16_t>((pixels[0] << 8) | pixels[1]);
				setPixel(m_x, m_y, luma8(color) > THRESHOLDS[m_y & 3][m_x & 3]);
				if (++m_x == m_windowRight) {
					m_x = m_windowX;
					m_y++;
				}
			}
		}

		void endPixels() override {
			if (m_windowBottom > m_windowY) {
				sendPages(m_windowY >> 3, (m_windowBottom - 1) >> 3);
			}
		}
	};
}
