#pragma once

// DVD navigation packs: the first pack of each VOBU carries, in private stream 2, the presentation control
// information (PCI: menu buttons and their highlight) and the data search information (DSI). Layouts as documented
// by the libdvdread project.

#include <array>
#include <cstdint>
#include <vector>

#include "Ifo.h"
#include "../stream/MStream.h"

namespace Dvd {
	struct Button {
		// Area in picture pixels, inclusive
		int x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		// Highlight colours: 1-3, an entry of Highlight::colors, 0 = none
		uint8_t colorTable = 0;
		// 1: activated as soon as it is selected
		uint8_t autoAction = 0;
		// Buttons selected by the arrow keys, 1-based
		uint8_t up = 0, down = 0, left = 0, right = 0;
		Command command{};
	};

	struct Highlight {
		// 0: no buttons; 1: new information; 2, 3: the same as before
		uint8_t status = 0;
		int64_t startPtsMs = 0;
		int64_t endPtsMs = 0;
		// Buttons from 1; button n is buttons[n - 1]
		std::vector<Button> buttons;
		// Button to select, and to activate, as the highlight starts (0 = none)
		uint8_t forcedSelect = 0;
		uint8_t forcedActivate = 0;
		// By colour table and state (0 selected, 1 activated): palette indices and contrasts for pixel values 0-3
		struct Colors {
			std::array<uint8_t, 4> color{};
			std::array<uint8_t, 4> contrast{};
		};
		std::array<std::array<Colors, 2>, 3> colors{};
	};

	// Presentation control information of a VOBU
	struct Pci {
		uint32_t sector = 0;
		int64_t startPtsMs = 0;
		int64_t endPtsMs = 0;
		Highlight highlight;
	};

	/**
	 * Parses a PCI packet's data, after its sub-stream id 0x00.
	 * @return false if there's too little
	 */
	bool parsePci(const uint8_t *data, size_t size, Pci &out);

	// Receives private stream 2 packets and keeps the latest PCI
	class NavPackDecoder final : public MStream::IWriteByteStream {
		std::vector<uint8_t> m_packet;
		std::vector<Pci> m_decoded;

	public:
		int32_t getPts(int32_t) override { return -1; }
		void setPts(int32_t) override {}
		// Private stream 2 packets have no PES header extension, so the demuxer marks each packet's start
		void startPacket(uint32_t length) override;
		void writeByte(uint8_t data) override;
		[[nodiscard]] bool isFull() override { return false; }
		void executeCallbacks() override {}
		void flush() override {}
		void discontinuity() override { m_packet.clear(); }

		// Takes the PCI decoded since the last call
		std::vector<Pci> take();

	private:
		uint32_t m_length = 0;
	};
}
