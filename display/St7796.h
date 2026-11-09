#pragma once

// Sitronix ST7796S TFT LCD controller, 320x480, RGB565. Common on 3.5 and 4 inch modules.

#include "Ili9341.h"

namespace display {
	class St7796 final : public MipiDcsDisplay {
	public:
		static constexpr DcsPanel panel320x480(const Rotation rotation) {
			return {320, 480, 320, 480, 0, 0, rotationMadctl(ILI_ROTATIONS, rotation, true), false};
		}

		St7796(Bus &bus, const DcsPanel &panel) : MipiDcsDisplay(bus, panel) {
		}

	protected:
		void initController() override {
			static constexpr uint8_t SEQUENCE[] = {
				dcs::SOFTWARE_RESET, dcs::DELAY, 120,
				dcs::SLEEP_OUT, dcs::DELAY, 120,
				0xF0, 1, 0xC3, // Unlock command set 2, part 1
				0xF0, 1, 0x96, // Part 2
				dcs::PIXEL_FORMAT, 1, 0x55, // 16 bits per pixel
				0xB4, 1, 0x01, // 1-dot inversion
				0xB6, 3, 0x80, 0x02, 0x3B, // Display function
				0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33, // Output timing
				0xC1, 1, 0x06, // Power control 2
				0xC2, 1, 0xA7, // Power control 3
				0xC5, 1 | dcs::DELAY, 0x18, 120, // VCOM
				0xE0, 14, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B, // Positive gamma
				0xE1, 14 | dcs::DELAY, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B, 120, // Negative gamma
				0xF0, 1, 0x3C, // Lock command set 2
				0xF0, 1 | dcs::DELAY, 0x69, 120,
			};
			runSequence(SEQUENCE);
			finishInit();
		}
	};
}
