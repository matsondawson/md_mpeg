#pragma once

// Sitronix ST7735R and ST7735S TFT LCD controller, RGB565. Used on 128x160, 128x128 and 80x160 modules.
//
// Modules differ in where the panel sits in memory and in colour order, so the offsets here may need
// adjusting: display_test shows any misplaced rows or columns as noise at an edge.

#include "MipiDcsDisplay.h"

namespace display {
	class St7735 final : public MipiDcsDisplay {
		static constexpr RotationTable ROTATIONS = {
			dcs::MADCTL_MX | dcs::MADCTL_MY,
			dcs::MADCTL_MY | dcs::MADCTL_MV,
			0x00,
			dcs::MADCTL_MX | dcs::MADCTL_MV
		};

	public:
		// 1.8 inch modules whose panel fills memory, often sold as "black tab"
		static constexpr DcsPanel panel128x160(const Rotation rotation) {
			return {128, 160, 128, 160, 0, 0, rotationMadctl(ROTATIONS, rotation, false), false};
		}
		// 1.44 inch modules
		static constexpr DcsPanel panel128x128(const Rotation rotation) {
			return {132, 162, 128, 128, 2, 31, rotationMadctl(ROTATIONS, rotation, true), false};
		}
		// 0.96 inch IPS modules
		static constexpr DcsPanel panel80x160(const Rotation rotation) {
			return {132, 162, 80, 160, 26, 1, rotationMadctl(ROTATIONS, rotation, true), true};
		}

		St7735(Bus &bus, const DcsPanel &panel) : MipiDcsDisplay(bus, panel) {
		}

	protected:
		void initController() override {
			static constexpr uint8_t SEQUENCE[] = {
				dcs::SOFTWARE_RESET, dcs::DELAY, 150,
				dcs::SLEEP_OUT, dcs::DELAY, 255,
				0xB1, 3, 0x01, 0x2C, 0x2D, // Frame rate, normal mode
				0xB2, 3, 0x01, 0x2C, 0x2D, // Frame rate, idle mode
				0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D, // Frame rate, partial mode
				0xB4, 1, 0x07, // No line inversion
				0xC0, 3, 0xA2, 0x02, 0x84, // Power control
				0xC1, 1, 0xC5,
				0xC2, 2, 0x0A, 0x00,
				0xC3, 2, 0x8A, 0x2A,
				0xC4, 2, 0x8A, 0xEE,
				0xC5, 1, 0x0E, // VCOM
				dcs::PIXEL_FORMAT, 1, 0x05, // 16 bits per pixel
				0xE0, 16, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10, // Positive gamma
				0xE1, 16, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10, // Negative gamma
			};
			runSequence(SEQUENCE);
			finishInit();
		}
	};
}
