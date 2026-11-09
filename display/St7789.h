#pragma once

// Sitronix ST7789 and ST7789V TFT LCD controller, 240x320 memory, RGB565. Used on 240x320, 240x280, 240x240,
// 172x320 and 135x240 modules, which mostly have IPS panels that need inversion on.

#include "MipiDcsDisplay.h"

namespace display {
	class St7789 final : public MipiDcsDisplay {
		static constexpr RotationTable ROTATIONS = {
			0x00,
			dcs::MADCTL_MX | dcs::MADCTL_MV,
			dcs::MADCTL_MX | dcs::MADCTL_MY,
			dcs::MADCTL_MY | dcs::MADCTL_MV
		};

		static constexpr DcsPanel panel(const uint16_t columns, const uint16_t rows, const uint16_t columnStart,
		                                const uint16_t rowStart, const Rotation rotation) {
			return {240, 320, columns, rows, columnStart, rowStart, rotationMadctl(ROTATIONS, rotation, false), true};
		}

	public:
		static constexpr DcsPanel panel240x320(const Rotation rotation) {
			return panel(240, 320, 0, 0, rotation);
		}
		// R270 is landscape with the pin header on the right
		static constexpr DcsPanel panel240x280(const Rotation rotation) {
			return panel(240, 280, 0, 20, rotation);
		}
		static constexpr DcsPanel panel240x240(const Rotation rotation) {
			return panel(240, 240, 0, 0, rotation);
		}
		static constexpr DcsPanel panel172x320(const Rotation rotation) {
			return panel(172, 320, 34, 0, rotation);
		}
		static constexpr DcsPanel panel135x240(const Rotation rotation) {
			return panel(135, 240, 52, 40, rotation);
		}

		St7789(Bus &bus, const DcsPanel &panel) : MipiDcsDisplay(bus, panel) {
		}

	protected:
		void initController() override {
			static constexpr uint8_t SEQUENCE[] = {
				dcs::SLEEP_OUT, dcs::DELAY, 120,
				dcs::PIXEL_FORMAT, 1, 0x55, // 16 bits per pixel
				dcs::GAMMA_SET, 1, 0x01, // Gamma 2.2, the panel default
			};
			runSequence(SEQUENCE);
			finishInit();
		}
	};
}
