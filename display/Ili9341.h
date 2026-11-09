#pragma once

// Ilitek ILI9341 TFT LCD controller, 240x320, RGB565. Common on 2.2 to 2.8 inch modules.

#include "MipiDcsDisplay.h"

namespace display {
	// MADCTL for each rotation, shared by the ILI9341 and the controllers modelled on it
	constexpr RotationTable ILI_ROTATIONS = {
		dcs::MADCTL_MX,
		dcs::MADCTL_MV,
		dcs::MADCTL_MY,
		dcs::MADCTL_MX | dcs::MADCTL_MY | dcs::MADCTL_MV
	};

	class Ili9341 final : public MipiDcsDisplay {
	public:
		static constexpr DcsPanel panel240x320(const Rotation rotation) {
			return {240, 320, 240, 320, 0, 0, rotationMadctl(ILI_ROTATIONS, rotation, true), false};
		}

		Ili9341(Bus &bus, const DcsPanel &panel) : MipiDcsDisplay(bus, panel) {
		}

	protected:
		void initController() override {
			static constexpr uint8_t SEQUENCE[] = {
				dcs::SOFTWARE_RESET, dcs::DELAY, 150,
				0xEF, 3, 0x03, 0x80, 0x02,
				0xCF, 3, 0x00, 0xC1, 0x30, // Power control B
				0xED, 4, 0x64, 0x03, 0x12, 0x81, // Power on sequence
				0xE8, 3, 0x85, 0x00, 0x78, // Driver timing A
				0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02, // Power control A
				0xF7, 1, 0x20, // Pump ratio
				0xEA, 2, 0x00, 0x00, // Driver timing B
				0xC0, 1, 0x23, // Power control 1
				0xC1, 1, 0x10, // Power control 2
				0xC5, 2, 0x3E, 0x28, // VCOM 1
				0xC7, 1, 0x86, // VCOM 2
				0x37, 1, 0x00, // Vertical scroll start
				dcs::PIXEL_FORMAT, 1, 0x55, // 16 bits per pixel
				0xB1, 2, 0x00, 0x18, // Frame rate, 79 Hz
				0xB6, 3, 0x08, 0x82, 0x27, // Display function
				0xF2, 1, 0x00, // 3 gamma off
				dcs::GAMMA_SET, 1, 0x01,
				0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00, // Positive gamma
				0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F, // Negative gamma
				dcs::SLEEP_OUT, dcs::DELAY, 150,
			};
			runSequence(SEQUENCE);
			finishInit();
		}
	};
}
