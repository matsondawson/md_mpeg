#pragma once

// SD card in SPI mode on SPI1, read only.
//
// Sectors are read with a multiple block read left open, so reading the sector after the last one is just
// the next block of the stream. Reading any other sector stops the stream and starts a new one.
//
// Wiring: SCK GP10, MOSI GP11, MISO GP12, CS GP13.

#include <cstdint>

#include "hardware/spi.h"
#include "pico/stdlib.h"

class SdCard {
	static constexpr uint PIN_SCK = 10;
	static constexpr uint PIN_MOSI = 11;
	static constexpr uint PIN_MISO = 12;
	static constexpr uint PIN_CS = 13;

	static constexpr uint32_t INIT_HZ = 400000;
	// The most a card has to support before switching to high speed mode. SPI mode has no CRC checks by
	// default, so a clock the wiring can carry is the protection against corrupt reads. Tested on jumper wires:
	// 25 and 33 MHz read 8184 sectors with no CRC errors, 50 MHz failed every read.
	static constexpr uint32_t READ_HZ = 25000000;

	static constexpr uint8_t CMD0_GO_IDLE = 0;
	static constexpr uint8_t CMD8_SEND_IF_COND = 8;
	static constexpr uint8_t CMD16_SET_BLOCKLEN = 16;
	static constexpr uint8_t CMD12_STOP_TRANSMISSION = 12;
	static constexpr uint8_t CMD18_READ_MULTIPLE_BLOCK = 18;
	static constexpr uint8_t CMD55_APP_CMD = 55;
	static constexpr uint8_t CMD58_READ_OCR = 58;
	static constexpr uint8_t ACMD41_SEND_OP_COND = 41;

	static constexpr uint8_t R1_IDLE = 0x01;
	static constexpr uint8_t R1_ILLEGAL_COMMAND = 0x04;
	static constexpr uint8_t DATA_START_TOKEN = 0xFE;

	bool m_ready = false;
	// Block addressed (SDHC and SDXC) rather than byte addressed
	bool m_blockAddressed = false;
	// A multiple block read is open, with the card selected, and its next block is m_nextLba
	bool m_streaming = false;
	uint32_t m_nextLba = 0;

	static uint8_t transfer(const uint8_t out = 0xFF) {
		uint8_t in;
		spi_write_read_blocking(spi1, &out, &in, 1);
		return in;
	}

	static void select() {
		gpio_put(PIN_CS, false);
	}

	static void deselect() {
		gpio_put(PIN_CS, true);
		// The card only releases MISO after a clock with CS high
		transfer();
	}

	// The card holds MISO low while busy
	static bool waitReady(const uint32_t timeoutMs) {
		const auto deadline = make_timeout_time_ms(timeoutMs);
		while (transfer() != 0xFF) {
			if (time_reached(deadline)) {
				return false;
			}
		}
		return true;
	}

	// Sends a command with the card selected, leaving it selected to read any more of the response
	// @return R1 response, with bit 7 set if there was none
	static uint8_t command(const uint8_t index, const uint32_t argument) {
		select();
		if (!waitReady(500)) {
			return 0xFF;
		}
		// The CRC is only checked on these two until CRC checking is enabled, which it never is here
		const uint8_t crc = index == CMD0_GO_IDLE ? 0x95 : index == CMD8_SEND_IF_COND ? 0x87 : 0x01;
		const uint8_t frame[] = {
			static_cast<uint8_t>(0x40 | index),
			static_cast<uint8_t>(argument >> 24), static_cast<uint8_t>(argument >> 16),
			static_cast<uint8_t>(argument >> 8), static_cast<uint8_t>(argument),
			crc
		};
		spi_write_blocking(spi1, frame, sizeof(frame));
		uint8_t response = 0xFF;
		for (int i = 0; i < 10 && (response & 0x80); i++) {
			response = transfer();
		}
		return response;
	}

	// Stops a multiple block read and deselects the card
	// @return whether the card stopped
	bool stopStream() {
		m_streaming = false;
		// Sent straight away: the card is streaming, so it isn't busy
		const uint8_t frame[] = {0x40 | CMD12_STOP_TRANSMISSION, 0, 0, 0, 0, 0x01};
		spi_write_blocking(spi1, frame, sizeof(frame));
		// The byte after the command is still from the stream
		transfer();
		uint8_t response = 0xFF;
		for (int i = 0; i < 10 && (response & 0x80); i++) {
			response = transfer();
		}
		const bool stopped = response == 0 && waitReady(500);
		deselect();
		return stopped;
	}

	// Reads the next block of a read command
	static bool readBlock(uint8_t *buffer) {
		const auto deadline = make_timeout_time_ms(300);
		uint8_t token;
		while ((token = transfer()) == 0xFF) {
			if (time_reached(deadline)) {
				return false;
			}
		}
		if (token != DATA_START_TOKEN) {
			return false;
		}
		spi_read_blocking(spi1, 0xFF, buffer, 512);
		// The CRC, not checked
		transfer();
		transfer();
		return true;
	}

	static uint8_t appCommand(const uint8_t index, const uint32_t argument) {
		command(CMD55_APP_CMD, 0);
		deselect();
		return command(index, argument);
	}

public:
	/**
	 * Starts the card. Can be called again to retry or after the card is swapped.
	 * @return nullptr, or why it failed
	 */
	const char *init() {
		m_ready = false;
		m_streaming = false;
		gpio_init(PIN_CS);
		gpio_set_dir(PIN_CS, GPIO_OUT);
		gpio_put(PIN_CS, true);
		spi_init(spi1, INIT_HZ);
		spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
		gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
		gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
		gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
		// MISO floats when no card drives it
		gpio_pull_up(PIN_MISO);

		// At least 74 clocks with CS high, to enter native mode
		for (int i = 0; i < 10; i++) {
			transfer();
		}

		uint8_t response = 0xFF;
		for (int attempt = 0; attempt < 20 && response != R1_IDLE; attempt++) {
			response = command(CMD0_GO_IDLE, 0);
			deselect();
		}
		if (response != R1_IDLE) {
			return "No card responding (CMD0)";
		}

		// Version 2 cards echo the check pattern, older ones reject the command
		response = command(CMD8_SEND_IF_COND, 0x1AA);
		bool version2 = false;
		if (!(response & R1_ILLEGAL_COMMAND)) {
			uint32_t echo = 0;
			for (int i = 0; i < 4; i++) {
				echo = echo << 8 | transfer();
			}
			if ((echo & 0xFFF) != 0x1AA) {
				deselect();
				return "Card failed its voltage check (CMD8)";
			}
			version2 = true;
		}
		deselect();

		// Asks version 2 cards to allow block addressing, and waits for the card to finish starting
		const auto deadline = make_timeout_time_ms(2000);
		do {
			response = appCommand(ACMD41_SEND_OP_COND, version2 ? 0x40000000 : 0);
			deselect();
			if (response == 0) {
				break;
			}
			if (time_reached(deadline)) {
				return "Card didn't finish starting (ACMD41)";
			}
			sleep_ms(10);
		} while (true);

		m_blockAddressed = false;
		if (version2) {
			response = command(CMD58_READ_OCR, 0);
			uint32_t ocr = 0;
			for (int i = 0; i < 4; i++) {
				ocr = ocr << 8 | transfer();
			}
			deselect();
			if (response != 0) {
				return "Couldn't read the card's capacity type (CMD58)";
			}
			m_blockAddressed = (ocr & 0x40000000) != 0;
		}
		if (!m_blockAddressed) {
			response = command(CMD16_SET_BLOCKLEN, 512);
			deselect();
			if (response != 0) {
				return "Couldn't set the block size (CMD16)";
			}
		}

		spi_set_baudrate(spi1, READ_HZ);
		m_ready = true;
		return nullptr;
	}

	// Reads 512 bytes
	bool readSector(const uint32_t lba, uint8_t *buffer) {
		if (!m_ready) {
			return false;
		}
		if (m_streaming && lba != m_nextLba && !stopStream()) {
			return false;
		}
		if (!m_streaming) {
			const uint8_t response = command(CMD18_READ_MULTIPLE_BLOCK, m_blockAddressed ? lba : lba * 512);
			if (response != 0) {
				deselect();
				return false;
			}
			m_streaming = true;
		}
		if (!readBlock(buffer)) {
			stopStream();
			return false;
		}
		m_nextLba = lba + 1;
		return true;
	}
};
