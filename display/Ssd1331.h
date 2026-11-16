#pragma once

// Solomon Systech SSD1331 colour OLED controller, on 0.95 inch 96x64 modules, RGB565 over SPI. Parameters go
// as commands, and pixel data follows the window with no write command.

#include <utility>

#include "Display.h"

namespace display {
	struct Ssd1331Panel {
		uint16_t panelWidth, panelHeight;
		Rotation rotation;

		constexpr uint16_t width() const {
			return isQuarterTurn(rotation) ? panelHeight : panelWidth;
		}
		constexpr uint16_t height() const {
			return isQuarterTurn(rotation) ? panelWidth : panelHeight;
		}
	};

	class Ssd1331 final : public Display {
		const Ssd1331Panel m_panel;

	public:
		static constexpr Ssd1331Panel panel96x64(const Rotation rotation) {
			return {96, 64, rotation};
		}

		Ssd1331(Bus &bus, const Ssd1331Panel &panel) : Display(bus, panel.width(), panel.height()), m_panel(panel) {
		}

		void beginPixels(int x, int y, int width, int height) override {
			// Quarter turns write down the columns, so the window is given the other way round
			if (isQuarterTurn(m_panel.rotation)) {
				std::swap(x, y);
				std::swap(width, height);
			}
			const uint8_t window[] = {
				0x15, static_cast<uint8_t>(x), static_cast<uint8_t>(x + width - 1), // Columns
				0x75, static_cast<uint8_t>(y), static_cast<uint8_t>(y + height - 1), // Rows
			};
			m_bus.writeCommands(window, sizeof(window));
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
			// 65k colours, colour order A B C (RGB), plus each rotation's scan directions
			static constexpr uint8_t REMAP[] = {0x72, 0x63, 0x60, 0x71};
			const uint8_t sequence[] = {
				0xAE, // Display off
				0xA0, REMAP[static_cast<int>(m_panel.rotation)],
				0xA1, 0x00, // Start line
				0xA2, 0x00, // Display offset
				0xA4, // Normal display
				0xA8, 0x3F, // Multiplex ratio, 64 rows
				0xAD, 0x8E, // External VCC supply
				0xB0, 0x0B, // Power saving off
				0xB1, 0x31, // Phase period adjustment
				0xB3, 0xF0, // Clock divide and oscillator frequency
				0x8A, 0x64, // Second pre-charge speed for A, B and C
				0x8B, 0x78,
				0x8C, 0x64,
				0xBB, 0x3A, // Pre-charge voltage
				0xBE, 0x3E, // VCOMH
				0x87, 0x06, // Master current
				0x81, 0x91, // Contrast for A, B and C
				0x82, 0x50,
				0x83, 0x7D,
				0xAF, // Display on
			};
			m_bus.writeCommands(sequence, sizeof(sequence));
		}
	};
}
