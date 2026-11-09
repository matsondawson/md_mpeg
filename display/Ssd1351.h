#pragma once

// Solomon Systech SSD1351 colour OLED controller, on 1.5 inch 128x128 and 1.27 inch 128x96 modules, RGB565
// over SPI. Commands go with their parameters as data.

#include <utility>

#include "Display.h"

namespace display {
	struct Ssd1351Panel {
		uint16_t panelWidth, panelHeight;
		Rotation rotation;

		constexpr uint16_t width() const {
			return isQuarterTurn(rotation) ? panelHeight : panelWidth;
		}
		constexpr uint16_t height() const {
			return isQuarterTurn(rotation) ? panelWidth : panelHeight;
		}
	};

	class Ssd1351 final : public Display {
		const Ssd1351Panel m_panel;

		void command(const uint8_t code) {
			m_bus.writeCommand(code);
		}

		template <typename... Parameters>
		void command(const uint8_t code, const Parameters... parameters) {
			const uint8_t data[] = {static_cast<uint8_t>(parameters)...};
			m_bus.writeCommandWithData(code, data, sizeof(data));
		}

	public:
		static constexpr Ssd1351Panel panel128x128(const Rotation rotation) {
			return {128, 128, rotation};
		}
		static constexpr Ssd1351Panel panel128x96(const Rotation rotation) {
			return {128, 96, rotation};
		}

		Ssd1351(Bus &bus, const Ssd1351Panel &panel) : Display(bus, panel.width(), panel.height()), m_panel(panel) {
		}

		void beginPixels(int x, int y, int width, int height) override {
			// Quarter turns write down the columns, so the window is given the other way round
			if (isQuarterTurn(m_panel.rotation)) {
				std::swap(x, y);
				std::swap(width, height);
			}
			command(0x15, x, x + width - 1); // Columns
			command(0x75, y, y + height - 1); // Rows
			command(0x5C); // Write memory
			m_bus.beginData();
		}

		void sendPixels(const uint8_t *pixels, const uint32_t bytes) override {
			m_bus.sendData(pixels, bytes);
		}

		void endPixels() override {
			m_bus.endData();
		}

	protected:
		void initController() override {
			// 65k colours, COM split odd and even, colour order C B A, plus each rotation's scan directions
			static constexpr uint8_t REMAP[] = {0x74, 0x77, 0x66, 0x65};
			const int rotation = static_cast<int>(m_panel.rotation);
			command(0xFD, 0x12); // Unlock
			command(0xFD, 0xB1); // Unlock the commands below that are otherwise locked
			command(0xAE); // Display off
			command(0xB3, 0xF1); // Clock divide and oscillator frequency
			command(0xCA, 0x7F); // Multiplex ratio
			command(0xA2, 0x00); // Display offset
			// A panel shorter than memory starts lower when the rows are scanned from the bottom
			command(0xA1, rotation < 2 ? m_panel.panelHeight & 0x7F : 0); // Start line
			command(0xA0, REMAP[rotation]);
			command(0xB5, 0x00); // GPIO off
			command(0xAB, 0x01); // Internal VDD regulator
			command(0xB1, 0x32); // Pre-charge
			command(0xBE, 0x05); // VCOMH
			command(0xA6); // Normal display
			command(0xC1, 0xC8, 0x80, 0xC8); // Contrast for A, B and C
			command(0xC7, 0x0F); // Master contrast
			command(0xB4, 0xA0, 0xB5, 0x55); // Segment low voltage
			command(0xB6, 0x01); // Second pre-charge
			command(0xAF); // Display on
		}
	};
}
