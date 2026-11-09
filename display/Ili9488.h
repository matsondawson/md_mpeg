#pragma once

// Ilitek ILI9488 TFT LCD controller, 320x480. Over SPI it only takes 18-bit colour, three bytes a pixel, so
// RGB565 is converted as it's sent, into two buffers taking turns.

#include <algorithm>

#include "Ili9341.h"

namespace display {
	class Ili9488 final : public MipiDcsDisplay {
		static constexpr uint32_t BUFFER_PIXELS = 480;
		uint8_t m_buffers[2][BUFFER_PIXELS * 3];
		int m_nextBuffer = 0;

	public:
		static constexpr DcsPanel panel320x480(const Rotation rotation) {
			return {320, 480, 320, 480, 0, 0, rotationMadctl(ILI_ROTATIONS, rotation, true), false};
		}

		Ili9488(Bus &bus, const DcsPanel &panel) : MipiDcsDisplay(bus, panel) {
		}

		void sendPixels(const uint8_t *pixels, const uint32_t bytes) override {
			for (uint32_t left = bytes / 2; left;) {
				const uint32_t count = std::min(left, BUFFER_PIXELS);
				// The other buffer may still be sending, this one has finished
				uint8_t *out = m_buffers[m_nextBuffer];
				m_nextBuffer ^= 1;
				for (uint32_t i = 0; i < count; i++) {
					const uint16_t pixel = static_cast<uint16_t>((pixels[0] << 8) | pixels[1]);
					pixels += 2;
					out[i * 3] = static_cast<uint8_t>((pixel >> 8) & 0xF8);
					out[i * 3 + 1] = static_cast<uint8_t>((pixel >> 3) & 0xFC);
					out[i * 3 + 2] = static_cast<uint8_t>(pixel << 3);
				}
				m_bus.sendData(out, count * 3);
				left -= count;
			}
		}

	protected:
		void initController() override {
			static constexpr uint8_t SEQUENCE[] = {
				dcs::SOFTWARE_RESET, dcs::DELAY, 120,
				0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F, // Positive gamma
				0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F, // Negative gamma
				0xC0, 2, 0x17, 0x15, // Power control 1
				0xC1, 1, 0x41, // Power control 2
				0xC5, 3, 0x00, 0x12, 0x80, // VCOM
				dcs::PIXEL_FORMAT, 1, 0x66, // 18 bits per pixel, the only colour format over SPI
				0xB0, 1, 0x00, // Interface mode
				0xB1, 1, 0xA0, // Frame rate, 60 Hz
				0xB4, 1, 0x02, // 2-dot inversion
				0xB6, 3, 0x02, 0x02, 0x3B, // Display function
				0xB7, 1, 0xC6, // Entry mode
				0xF7, 4, 0xA9, 0x51, 0x2C, 0x82, // Adjust control 3
				dcs::SLEEP_OUT, dcs::DELAY, 120,
			};
			runSequence(SEQUENCE);
			finishInit();
		}
	};
}
