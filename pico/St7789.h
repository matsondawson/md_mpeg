#pragma once

// ST7789V 240x280 panel, in landscape with the pin header on the right.
//
// Wiring: SCL GP18, SDA GP19, CS GP17, DC GP20, RES GP21.

#include <cstddef>
#include <cstdint>

#include "hardware/dma.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

class St7789 {
	static constexpr uint PIN_CS = 17;
	static constexpr uint PIN_SCK = 18;
	static constexpr uint PIN_MOSI = 19;
	static constexpr uint PIN_DC = 20;
	static constexpr uint PIN_RST = 21;
	// The controller has 320 rows of memory and the panel shows the middle 280, which in landscape are columns
	static constexpr int X_OFFSET = 20;

	int m_dmaChannel = -1;

	void pinOutput(const uint pin) {
		gpio_init(pin);
		gpio_set_dir(pin, GPIO_OUT);
		gpio_put(pin, true);
	}

	void command(const uint8_t code, const uint8_t *data = nullptr, const size_t size = 0) {
		gpio_put(PIN_CS, false);
		gpio_put(PIN_DC, false);
		spi_write_blocking(spi0, &code, 1);
		if (size) {
			gpio_put(PIN_DC, true);
			spi_write_blocking(spi0, data, size);
		}
		gpio_put(PIN_CS, true);
	}

	void setWindow(const int x, const int y, const int width, const int height) {
		const int x0 = x + X_OFFSET, x1 = x0 + width - 1, y1 = y + height - 1;
		const uint8_t columns[] = {
			static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
			static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)
		};
		const uint8_t rows[] = {
			static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
			static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)
		};
		command(0x2A, columns, sizeof(columns));
		command(0x2B, rows, sizeof(rows));
		command(0x2C);
	}

public:
	static constexpr int WIDTH = 280;
	static constexpr int HEIGHT = 240;

	// Call after setting the system clock, which the SPI clock is divided from
	// @return actual SPI clock in Hz
	uint init(const uint32_t spiHz) {
		pinOutput(PIN_CS);
		pinOutput(PIN_DC);
		pinOutput(PIN_RST);
		const uint actualHz = spi_init(spi0, spiHz);
		spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
		gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
		gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

		gpio_put(PIN_RST, false);
		sleep_ms(20);
		gpio_put(PIN_RST, true);
		sleep_ms(150);

		command(0x11); // Sleep out
		sleep_ms(120);
		const uint8_t colorMode = 0x55; // 16 bits per pixel
		command(0x3A, &colorMode, 1);
		const uint8_t memoryAccess = 0xA0; // Rows and columns exchanged, rows reversed: landscape
		command(0x36, &memoryAccess, 1);
		command(0x21); // Inversion on, IPS panels need it for correct colours
		const uint8_t gammaCurve = 0x01; // Gamma 2.2, the panel default
		command(0x26, &gammaCurve, 1);
		command(0x13); // Normal display mode
		command(0x29); // Display on
		sleep_ms(20);

		m_dmaChannel = dma_claim_unused_channel(true);
		dma_channel_config config = dma_channel_get_default_config(m_dmaChannel);
		channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
		channel_config_set_dreq(&config, spi_get_dreq(spi0, true));
		dma_channel_configure(m_dmaChannel, &config, &spi_get_hw(spi0)->dr, nullptr, 0, false);
		return actualHz;
	}

	void fillRect(const int x, const int y, const int width, const int height, const uint16_t color) {
		static uint8_t line[WIDTH * 2];
		for (int i = 0; i < width; i++) {
			line[i * 2] = color >> 8;
			line[i * 2 + 1] = color & 0xFF;
		}
		setWindow(x, y, width, height);
		gpio_put(PIN_CS, false);
		gpio_put(PIN_DC, true);
		for (int row = 0; row < height; row++) {
			spi_write_blocking(spi0, line, width * 2);
		}
		gpio_put(PIN_CS, true);
	}

	// Starts sending big-endian RGB565 pixels to a window, in pieces with sendPixels()
	void beginPixels(const int x, const int y, const int width, const int height) {
		setWindow(x, y, width, height);
		gpio_put(PIN_CS, false);
		gpio_put(PIN_DC, true);
	}

	// Waits for the previous piece, then sends this one in the background. pixels must stay valid until the
	// next sendPixels() or endPixels() returns.
	void sendPixels(const uint8_t *pixels, const uint32_t bytes) {
		dma_channel_wait_for_finish_blocking(m_dmaChannel);
		dma_channel_transfer_from_buffer_now(m_dmaChannel, pixels, bytes);
	}

	void endPixels() {
		dma_channel_wait_for_finish_blocking(m_dmaChannel);
		while (spi_is_busy(spi0)) {
		}
		// DMA doesn't read what comes back, clear it and the overrun flag for the next blocking write
		while (spi_is_readable(spi0)) {
			(void) spi_get_hw(spi0)->dr;
		}
		spi_get_hw(spi0)->icr = SPI_SSPICR_RORIC_BITS;
		gpio_put(PIN_CS, true);
	}
};
