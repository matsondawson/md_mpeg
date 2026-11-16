#pragma once

// Sino Wealth SH1106 monochrome OLED controller, on 1.3 inch 128x64 modules, over I2C or SPI. Its memory is
// 132 columns wide with the panel in the middle 128, and it only addresses one page at a time. Parameters go
// as commands.

#include "MonoOled.h"

namespace display {
	class Sh1106 final : public MonoOled {
		static constexpr int MEMORY_COLUMNS = 132;

	public:
		static constexpr MonoPanel panel128x64(const bool rotate180 = false) {
			return {128, 64, rotate180};
		}

		Sh1106(Bus &bus, const MonoPanel &panel) : MonoOled(bus, panel) {
		}

	protected:
		void initController() override {
			const uint8_t sequence[] = {
				0xAE, // Display off
				0xD5, 0x80, // Clock divide and oscillator frequency
				0xA8, static_cast<uint8_t>(m_panel.panelHeight - 1), // Multiplex ratio
				0xD3, 0x00, // Display offset
				0x40, // Start line 0
				0xAD, 0x8B, // DC-DC converter on
				0x32, // Pump voltage 8 V
				static_cast<uint8_t>(m_panel.rotate180 ? 0xA0 : 0xA1), // Segment remap
				static_cast<uint8_t>(m_panel.rotate180 ? 0xC0 : 0xC8), // COM scan direction
				0xDA, 0x12, // COM pins
				0x81, 0x80, // Contrast
				0xD9, 0x1F, // Pre-charge period
				0xDB, 0x40, // VCOM deselect level
				0xA4, // Show memory
				0xA6, // Not inverted
				0xAF, // Display on
			};
			m_bus.writeCommands(sequence, sizeof(sequence));
		}

		void sendPages(const int firstPage, const int lastPage) override {
			// Centred in memory, which is the same either way round
			const int column = (MEMORY_COLUMNS - m_width) / 2;
			for (int page = firstPage; page <= lastPage; page++) {
				const uint8_t address[] = {
					static_cast<uint8_t>(0xB0 | page),
					static_cast<uint8_t>(column & 0x0F),
					static_cast<uint8_t>(0x10 | (column >> 4)),
				};
				m_bus.writeCommands(address, sizeof(address));
				m_bus.writeData(m_buffer + page * m_width, m_width);
			}
		}
	};
}
