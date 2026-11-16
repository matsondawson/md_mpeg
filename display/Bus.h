#pragma once

// The link between a display driver and its controller, implemented for each microcontroller.
//
// Drivers only see commands and data. On a 4-wire SPI bus, commands go with the DC pin low and data with it
// high. On I2C, the bytes after the address start with a control byte: 0x00 for commands, 0x40 for data.

#include <cstddef>
#include <cstdint>

namespace display {
	class Bus {
	public:
		virtual ~Bus() = default;

		// Sets up the pins and peripheral, and pulses the controller's reset pin if it has one.
		// Call after setting the system clock, if the bus clock is divided from it.
		virtual void init() = 0;

		// The bus clock actually in use, in Hz, once init() has run
		virtual uint32_t clockHz() const = 0;

		// Bytes that are all commands, including any parameters the controller takes as commands
		virtual void writeCommands(const uint8_t *bytes, size_t size) = 0;

		// A command byte, then its parameters as data, in one transaction
		virtual void writeCommandWithData(uint8_t command, const uint8_t *data, size_t size) = 0;

		virtual void writeData(const uint8_t *bytes, size_t size) = 0;

		// Starts a run of data, sent in pieces with sendData()
		virtual void beginData() = 0;

		// Waits for the previous piece, then sends this one, in the background where the bus can. bytes must
		// stay valid until the next sendData() or endData() returns.
		virtual void sendData(const uint8_t *bytes, size_t size) = 0;

		// Waits for the last piece and ends the run
		virtual void endData() = 0;

		virtual void delayMs(uint32_t ms) = 0;

		void writeCommand(const uint8_t command) {
			writeCommands(&command, 1);
		}
	};
}
