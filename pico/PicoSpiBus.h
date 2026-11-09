#pragma once

// display::Bus over 4-wire SPI on the RP2040 and RP2350: clock, data out, chip select (optional), data or
// command, and reset (optional). Runs of data go by DMA.

#include <cstddef>
#include <cstdint>

#include "Bus.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

class PicoSpiBus final : public display::Bus {
public:
	struct Config {
		spi_inst_t *spi;
		uint pinSck, pinMosi;
		// -1 where the module has no such pin
		int pinCs, pinDc, pinReset;
		uint32_t hz;
		// Mode 3 works for every controller here, and is the only mode for modules without chip select
		spi_cpol_t cpol = SPI_CPOL_1;
		spi_cpha_t cpha = SPI_CPHA_1;
	};

private:
	const Config m_config;
	uint32_t m_actualHz = 0;
	int m_dmaChannel = -1;

	static void pinOutput(const int pin) {
		if (pin >= 0) {
			gpio_init(pin);
			gpio_set_dir(pin, GPIO_OUT);
			gpio_put(pin, true);
		}
	}

	void select(const bool data) const {
		if (m_config.pinCs >= 0) {
			gpio_put(m_config.pinCs, false);
		}
		gpio_put(m_config.pinDc, data);
	}

	void deselect() const {
		if (m_config.pinCs >= 0) {
			gpio_put(m_config.pinCs, true);
		}
	}

public:
	explicit PicoSpiBus(const Config &config) : m_config(config) {
	}

	void init() override {
		pinOutput(m_config.pinCs);
		pinOutput(m_config.pinDc);
		pinOutput(m_config.pinReset);
		m_actualHz = spi_init(m_config.spi, m_config.hz);
		spi_set_format(m_config.spi, 8, m_config.cpol, m_config.cpha, SPI_MSB_FIRST);
		gpio_set_function(m_config.pinSck, GPIO_FUNC_SPI);
		gpio_set_function(m_config.pinMosi, GPIO_FUNC_SPI);

		if (m_config.pinReset >= 0) {
			gpio_put(m_config.pinReset, false);
			sleep_ms(20);
			gpio_put(m_config.pinReset, true);
			sleep_ms(150);
		}

		if (m_dmaChannel < 0) {
			m_dmaChannel = dma_claim_unused_channel(true);
		}
		dma_channel_config config = dma_channel_get_default_config(m_dmaChannel);
		channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
		channel_config_set_dreq(&config, spi_get_dreq(m_config.spi, true));
		dma_channel_configure(m_dmaChannel, &config, &spi_get_hw(m_config.spi)->dr, nullptr, 0, false);
	}

	uint32_t clockHz() const override {
		return m_actualHz;
	}

	void writeCommands(const uint8_t *bytes, const size_t size) override {
		select(false);
		spi_write_blocking(m_config.spi, bytes, size);
		deselect();
	}

	void writeCommandWithData(const uint8_t command, const uint8_t *data, const size_t size) override {
		select(false);
		spi_write_blocking(m_config.spi, &command, 1);
		if (size) {
			gpio_put(m_config.pinDc, true);
			spi_write_blocking(m_config.spi, data, size);
		}
		deselect();
	}

	void writeData(const uint8_t *bytes, const size_t size) override {
		select(true);
		spi_write_blocking(m_config.spi, bytes, size);
		deselect();
	}

	void beginData() override {
		select(true);
	}

	void sendData(const uint8_t *bytes, const size_t size) override {
		dma_channel_wait_for_finish_blocking(m_dmaChannel);
		dma_channel_transfer_from_buffer_now(m_dmaChannel, bytes, size);
	}

	void endData() override {
		dma_channel_wait_for_finish_blocking(m_dmaChannel);
		while (spi_is_busy(m_config.spi)) {
		}
		// DMA doesn't read what comes back, clear it and the overrun flag for the next blocking write
		while (spi_is_readable(m_config.spi)) {
			(void) spi_get_hw(m_config.spi)->dr;
		}
		spi_get_hw(m_config.spi)->icr = SPI_SSPICR_RORIC_BITS;
		deselect();
	}

	void delayMs(const uint32_t ms) override {
		sleep_ms(ms);
	}
};
