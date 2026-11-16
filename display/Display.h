#pragma once

// A display module: a controller and its panel, reached through a Bus.
//
// Pixels go in as big-endian RGB565, whatever the controller stores. Drivers for other formats convert them.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "Bus.h"

namespace display {
	// Clockwise from the controller's own orientation
	enum class Rotation : uint8_t {
		R0,
		R90,
		R180,
		R270
	};

	constexpr bool isQuarterTurn(const Rotation rotation) {
		return rotation == Rotation::R90 || rotation == Rotation::R270;
	}

	// RGB565 to 8-bit components, filling the low bits so full intensity is 255
	constexpr uint8_t red8(const uint16_t pixel) {
		const uint32_t red = pixel >> 11;
		return static_cast<uint8_t>((red << 3) | (red >> 2));
	}
	constexpr uint8_t green8(const uint16_t pixel) {
		const uint32_t green = (pixel >> 5) & 0x3F;
		return static_cast<uint8_t>((green << 2) | (green >> 4));
	}
	constexpr uint8_t blue8(const uint16_t pixel) {
		const uint32_t blue = pixel & 0x1F;
		return static_cast<uint8_t>((blue << 3) | (blue >> 2));
	}

	// BT.601 luma, 0 to 255
	constexpr uint8_t luma8(const uint16_t pixel) {
		return static_cast<uint8_t>((77 * red8(pixel) + 150 * green8(pixel) + 29 * blue8(pixel)) >> 8);
	}

	class Display {
	protected:
		Bus &m_bus;
		const uint16_t m_width;
		const uint16_t m_height;

		Display(Bus &bus, const uint16_t width, const uint16_t height) : m_bus(bus), m_width(width), m_height(height) {
		}

		// Sends the controller's setup, after the bus is up and the controller reset
		virtual void initController() = 0;

	public:
		virtual ~Display() = default;

		Display(const Display &) = delete;
		Display &operator=(const Display &) = delete;

		// Call after setting the system clock, if the bus clock is divided from it
		void init() {
			m_bus.init();
			initController();
		}

		Bus &bus() const {
			return m_bus;
		}

		// As shown, after rotation
		uint16_t width() const {
			return m_width;
		}
		uint16_t height() const {
			return m_height;
		}

		// Starts sending big-endian RGB565 pixels to a window, left to right and top to bottom, in pieces with
		// sendPixels(). The window must be on the screen.
		virtual void beginPixels(int x, int y, int width, int height) = 0;

		// Waits for the previous piece, then sends this one, in the background where the bus can. Pieces are a
		// whole number of pixels. pixels must stay valid until the next sendPixels() or endPixels() returns.
		virtual void sendPixels(const uint8_t *pixels, uint32_t bytes) = 0;

		// Waits for the last piece. Drivers that keep a frame buffer show the window now.
		virtual void endPixels() = 0;

		// Fills a rectangle, clipped to the screen
		void fillRect(int x, int y, int width, int height, const uint16_t color) {
			const int x1 = std::min<int>(x + width, m_width), y1 = std::min<int>(y + height, m_height);
			x = std::max(x, 0);
			y = std::max(y, 0);
			width = x1 - x;
			height = y1 - y;
			if (width <= 0 || height <= 0) {
				return;
			}
			// Sent over and over. It doesn't change, so a send in progress can read it.
			constexpr uint32_t CHUNK_PIXELS = 64;
			static uint8_t chunk[CHUNK_PIXELS * 2];
			for (uint32_t i = 0; i < CHUNK_PIXELS; i++) {
				chunk[i * 2] = static_cast<uint8_t>(color >> 8);
				chunk[i * 2 + 1] = static_cast<uint8_t>(color);
			}
			beginPixels(x, y, width, height);
			for (uint32_t left = static_cast<uint32_t>(width) * static_cast<uint32_t>(height); left;) {
				const uint32_t pixels = std::min(left, CHUNK_PIXELS);
				sendPixels(chunk, pixels * 2);
				left -= pixels;
			}
			endPixels();
		}

		void fill(const uint16_t color) {
			fillRect(0, 0, m_width, m_height, color);
		}
	};
}
