#pragma once

// Common code for TFT LCD controllers that use the MIPI DCS command set over SPI: ST7789, ST7735, ILI9341,
// ST7796, ILI9488 and GC9A01. They share the window (CASET, RASET, RAMWR), orientation (MADCTL), colour
// format (COLMOD) and inversion commands, and differ in their power and gamma setup.
//
// Commands go with their parameters as data.

#include <cstddef>
#include <cstdint>

#include "Display.h"

namespace display {
	namespace dcs {
		constexpr uint8_t SOFTWARE_RESET = 0x01;
		constexpr uint8_t SLEEP_OUT = 0x11;
		constexpr uint8_t NORMAL_MODE = 0x13;
		constexpr uint8_t INVERSION_OFF = 0x20;
		constexpr uint8_t INVERSION_ON = 0x21;
		constexpr uint8_t GAMMA_SET = 0x26;
		constexpr uint8_t DISPLAY_ON = 0x29;
		constexpr uint8_t COLUMN_ADDRESS_SET = 0x2A;
		constexpr uint8_t ROW_ADDRESS_SET = 0x2B;
		constexpr uint8_t MEMORY_WRITE = 0x2C;
		constexpr uint8_t MEMORY_ACCESS_CONTROL = 0x36;
		constexpr uint8_t PIXEL_FORMAT = 0x3A;

		// MADCTL bits. MY reverses the memory's rows and MX its columns, whatever MV does; MV makes x run
		// down the rows.
		constexpr uint8_t MADCTL_MY = 0x80;
		constexpr uint8_t MADCTL_MX = 0x40;
		constexpr uint8_t MADCTL_MV = 0x20;
		constexpr uint8_t MADCTL_BGR = 0x08;

		// In an init sequence, after a command's parameter count: a delay in ms follows the parameters
		constexpr uint8_t DELAY = 0x80;
	}

	// Where a panel sits in its controller's memory, and how it's shown
	struct DcsPanel {
		// The controller's memory and the panel, in columns and rows as addressed with MADCTL 0
		uint16_t memoryColumns, memoryRows;
		uint16_t panelColumns, panelRows;
		// Where the panel starts in memory, with MADCTL 0
		uint16_t columnStart, rowStart;
		// Rotation and colour order
		uint8_t madctl;
		// IPS panels mostly need inversion on for correct colours
		bool invert;

		constexpr bool exchanged() const {
			return madctl & dcs::MADCTL_MV;
		}
		constexpr uint16_t width() const {
			return exchanged() ? panelRows : panelColumns;
		}
		constexpr uint16_t height() const {
			return exchanged() ? panelColumns : panelRows;
		}
		// A reversed axis addresses the panel from the other end of memory
		constexpr uint16_t columnOffset() const {
			return madctl & dcs::MADCTL_MX ? memoryColumns - panelColumns - columnStart : columnStart;
		}
		constexpr uint16_t rowOffset() const {
			return madctl & dcs::MADCTL_MY ? memoryRows - panelRows - rowStart : rowStart;
		}
		constexpr uint16_t xOffset() const {
			return exchanged() ? rowOffset() : columnOffset();
		}
		constexpr uint16_t yOffset() const {
			return exchanged() ? columnOffset() : rowOffset();
		}
	};

	// MADCTL for each rotation, R0 to R270, as each controller's modules are usually mounted
	using RotationTable = uint8_t[4];

	constexpr uint8_t rotationMadctl(const RotationTable &table, const Rotation rotation, const bool bgr) {
		return static_cast<uint8_t>(table[static_cast<int>(rotation)] | (bgr ? dcs::MADCTL_BGR : 0));
	}

	class MipiDcsDisplay : public Display {
	protected:
		const DcsPanel m_panel;

		MipiDcsDisplay(Bus &bus, const DcsPanel &panel) : Display(bus, panel.width(), panel.height()), m_panel(panel) {
		}

		void command(const uint8_t code) {
			m_bus.writeCommand(code);
		}

		void command(const uint8_t code, const uint8_t parameter) {
			m_bus.writeCommandWithData(code, &parameter, 1);
		}

		/**
		 * Sends an init sequence: for each command, its code, its parameter count (with dcs::DELAY set if a
		 * delay follows), the parameters, then the delay in ms.
		 */
		template <size_t SIZE>
		void runSequence(const uint8_t (&sequence)[SIZE]) {
			for (size_t i = 0; i < SIZE;) {
				const uint8_t code = sequence[i++];
				const uint8_t count = sequence[i] & ~dcs::DELAY;
				const bool delay = sequence[i++] & dcs::DELAY;
				m_bus.writeCommandWithData(code, sequence + i, count);
				i += count;
				if (delay) {
					m_bus.delayMs(sequence[i++]);
				}
			}
		}

		// Orientation, inversion and display on, after the controller's own setup and sleep out
		void finishInit() {
			command(dcs::MEMORY_ACCESS_CONTROL, m_panel.madctl);
			command(m_panel.invert ? dcs::INVERSION_ON : dcs::INVERSION_OFF);
			command(dcs::NORMAL_MODE);
			command(dcs::DISPLAY_ON);
			m_bus.delayMs(20);
		}

		void setWindow(const int x, const int y, const int width, const int height) {
			const int x0 = x + m_panel.xOffset(), x1 = x0 + width - 1;
			const int y0 = y + m_panel.yOffset(), y1 = y0 + height - 1;
			const uint8_t columns[] = {
				static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
				static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)
			};
			const uint8_t rows[] = {
				static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
				static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)
			};
			m_bus.writeCommandWithData(dcs::COLUMN_ADDRESS_SET, columns, sizeof(columns));
			m_bus.writeCommandWithData(dcs::ROW_ADDRESS_SET, rows, sizeof(rows));
			command(dcs::MEMORY_WRITE);
		}

	public:
		const DcsPanel &panel() const {
			return m_panel;
		}

		void beginPixels(const int x, const int y, const int width, const int height) override {
			setWindow(x, y, width, height);
			m_bus.beginData();
		}

		void sendPixels(const uint8_t *pixels, const uint32_t bytes) override {
			m_bus.sendData(pixels, bytes);
		}

		void endPixels() override {
			m_bus.endData();
		}
	};
}
