#pragma once

// Solomon Systech SSD1306 monochrome OLED controller, on 128x64 and 128x32 modules, over I2C or SPI.
// Parameters go as commands.

#include "MonoOled.h"

namespace display {
	class Ssd1306 final : public MonoOled {
	public:
		static constexpr MonoPanel panel128x64(const bool rotate180 = false) {
			return {128, 64, rotate180};
		}
		static constexpr MonoPanel panel128x32(const bool rotate180 = false) {
			return {128, 32, rotate180};
		}

		Ssd1306(Bus &bus, const MonoPanel &panel) : MonoOled(bus, panel) {
		}

	protected:
		void initController() override {
			const uint8_t tall = m_panel.panelHeight > 32;
			const uint8_t sequence[] = {
				0xAE, // Display off
				0xD5, 0x80, // Clock divide and oscillator frequency
				0xA8, static_cast<uint8_t>(m_panel.panelHeight - 1), // Multiplex ratio
				0xD3, 0x00, // Display offset
				0x40, // Start line 0
				0x8D, 0x14, // Charge pump on
				0x20, 0x00, // Horizontal addressing
				static_cast<uint8_t>(m_panel.rotate180 ? 0xA0 : 0xA1), // Segment remap
				static_cast<uint8_t>(m_panel.rotate180 ? 0xC0 : 0xC8), // COM scan direction
				0xDA, static_cast<uint8_t>(tall ? 0x12 : 0x02), // COM pins
				0x81, static_cast<uint8_t>(tall ? 0xCF : 0x8F), // Contrast
				0xD9, 0xF1, // Pre-charge period
				0xDB, 0x40, // VCOMH deselect level
				0xA4, // Show memory
				0xA6, // Not inverted
				0x2E, // Scrolling off
				0xAF, // Display on
			};
			m_bus.writeCommands(sequence, sizeof(sequence));
		}

		void sendPages(const int firstPage, const int lastPage) override {
			const uint8_t window[] = {
				0x21, 0x00, static_cast<uint8_t>(m_width - 1), // Columns
				0x22, static_cast<uint8_t>(firstPage), static_cast<uint8_t>(lastPage), // Pages
			};
			m_bus.writeCommands(window, sizeof(window));
			m_bus.writeData(m_buffer + firstPage * m_width, (lastPage - firstPage + 1) * m_width);
		}
	};
}
