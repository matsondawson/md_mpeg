#include "NavPack.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace Dvd {
	namespace {
		uint32_t u32(const uint8_t *p) {
			return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
		}

		void colors(const uint32_t value, Highlight::Colors &out) {
			// Palette indices then contrasts, nibbles for pixel values 3, 2, 1, 0
			for (int i = 0; i < 4; i++) {
				out.color[i] = static_cast<uint8_t>(value >> (16 + i * 4) & 15);
				out.contrast[i] = static_cast<uint8_t>(value >> (i * 4) & 15);
			}
		}
	}

	bool parsePci(const uint8_t *data, const size_t size, Pci &out) {
		constexpr size_t BUTTONS = 0x8E;
		if (size < BUTTONS + 36 * 18) {
			return false;
		}
		out.sector = u32(data);
		out.startPtsMs = u32(data + 0x0C) / 90;
		out.endPtsMs = u32(data + 0x10) / 90;

		Highlight &h = out.highlight;
		h.status = data[0x61] & 3;
		h.startPtsMs = u32(data + 0x62) / 90;
		h.endPtsMs = u32(data + 0x66) / 90;
		const int buttonCount = std::min<int>(data[0x71], 36);
		h.forcedSelect = data[0x74];
		h.forcedActivate = data[0x75];
		for (int table = 0; table < 3; table++) {
			for (int state = 0; state < 2; state++) {
				colors(u32(data + 0x76 + table * 8 + state * 4), h.colors[table][state]);
			}
		}
		h.buttons.clear();
		for (int i = 0; i < buttonCount; i++) {
			const uint8_t *b = data + BUTTONS + i * 18;
			Button button;
			button.colorTable = b[0] >> 6;
			button.x1 = (b[0] & 0x3F) << 4 | b[1] >> 4;
			button.x2 = (b[1] & 3) << 8 | b[2];
			button.autoAction = b[3] >> 6;
			button.y1 = (b[3] & 0x3F) << 4 | b[4] >> 4;
			button.y2 = (b[4] & 3) << 8 | b[5];
			button.up = b[6] & 0x3F;
			button.down = b[7] & 0x3F;
			button.left = b[8] & 0x3F;
			button.right = b[9] & 0x3F;
			std::memcpy(button.command.data(), b + 10, 8);
			h.buttons.push_back(button);
		}
		return true;
	}

	void NavPackDecoder::startPacket(const uint32_t length) {
		m_packet.clear();
		m_length = length;
	}

	void NavPackDecoder::writeByte(const uint8_t data) {
		if (m_packet.size() >= m_length) {
			return;
		}
		m_packet.push_back(data);
		if (m_packet.size() == m_length && m_packet[0] == 0x00) {
			Pci pci;
			if (parsePci(m_packet.data() + 1, m_packet.size() - 1, pci)) {
				m_decoded.push_back(std::move(pci));
			}
		}
	}

	std::vector<Pci> NavPackDecoder::take() {
		return std::exchange(m_decoded, {});
	}
}
