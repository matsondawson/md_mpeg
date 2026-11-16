#pragma once

// display::Bus over I2C on the RP2040 and RP2350, for OLED modules: data, clock and reset (optional).
//
// Each write is a transaction of the address, a control byte (0x00 commands, 0x40 data) and up to
// CHUNK bytes, so a run of data is a series of transactions; the controllers carry on where the last one
// stopped. Writes block, there is no DMA.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Bus.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

class PicoI2cBus final : public display::Bus {
public:
	struct Config {
		i2c_inst_t *i2c;
		uint pinSda, pinScl;
		// -1 where the module has no such pin
		int pinReset;
		// 7-bit, 0x3C on most modules, 0x3D with the address jumper moved
		uint8_t address;
		// 400 kHz is the controllers' rated speed; many modules work at 1 MHz
		uint32_t hz = 400000;
	};

private:
	static constexpr uint8_t CONTROL_COMMANDS = 0x00;
	static constexpr uint8_t CONTROL_DATA = 0x40;
	// A command byte, then data in the same transaction
	static constexpr uint8_t CONTROL_ONE_COMMAND = 0x80;
	// A whole SSD1306 page fits in one transaction
	static constexpr size_t CHUNK = 128;

	const Config m_config;
	uint32_t m_actualHz = 0;
	uint8_t m_transaction[CHUNK + 3];

	void write(const uint8_t *bytes, const size_t size) const {
		// A missing or unpowered module doesn't acknowledge; there is nothing to do but carry on
		i2c_write_blocking(m_config.i2c, m_config.address, bytes, size, false);
	}

	// Splits bytes into transactions, each starting with the control byte
	void writeChunks(const uint8_t control, const uint8_t *bytes, size_t size) {
		while (size) {
			const size_t count = std::min(size, CHUNK);
			m_transaction[0] = control;
			memcpy(m_transaction + 1, bytes, count);
			write(m_transaction, count + 1);
			bytes += count;
			size -= count;
		}
	}

public:
	explicit PicoI2cBus(const Config &config) : m_config(config) {
	}

	void init() override {
		m_actualHz = i2c_init(m_config.i2c, m_config.hz);
		gpio_set_function(m_config.pinSda, GPIO_FUNC_I2C);
		gpio_set_function(m_config.pinScl, GPIO_FUNC_I2C);
		// Modules mostly have their own pull-ups, these help those that don't
		gpio_pull_up(m_config.pinSda);
		gpio_pull_up(m_config.pinScl);

		if (m_config.pinReset >= 0) {
			gpio_init(m_config.pinReset);
			gpio_set_dir(m_config.pinReset, GPIO_OUT);
			gpio_put(m_config.pinReset, false);
			sleep_ms(10);
			gpio_put(m_config.pinReset, true);
		}
		// The controller's supply settles
		sleep_ms(100);
	}

	uint32_t clockHz() const override {
		return m_actualHz;
	}

	void writeCommands(const uint8_t *bytes, const size_t size) override {
		writeChunks(CONTROL_COMMANDS, bytes, size);
	}

	void writeCommandWithData(const uint8_t command, const uint8_t *data, const size_t size) override {
		if (size > CHUNK) {
			writeCommand(command);
			writeData(data, size);
			return;
		}
		m_transaction[0] = CONTROL_ONE_COMMAND;
		m_transaction[1] = command;
		m_transaction[2] = CONTROL_DATA;
		memcpy(m_transaction + 3, data, size);
		write(m_transaction, size ? size + 3 : 2);
	}

	void writeData(const uint8_t *bytes, const size_t size) override {
		writeChunks(CONTROL_DATA, bytes, size);
	}

	void beginData() override {
	}

	void sendData(const uint8_t *bytes, const size_t size) override {
		writeData(bytes, size);
	}

	void endData() override {
	}

	void delayMs(const uint32_t ms) override {
		sleep_ms(ms);
	}
};
