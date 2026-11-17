#include "SubPicture.h"

#include <algorithm>
#include <utility>

namespace Dvd {
	namespace {
		class NibbleReader {
			const std::vector<uint8_t> &m_data;
			size_t m_position; // in nibbles
			size_t m_end;

		public:
			NibbleReader(const std::vector<uint8_t> &data, const size_t byteOffset, const size_t byteEnd)
				: m_data(data), m_position(byteOffset * 2), m_end(byteEnd * 2) {
			}

			bool next(uint32_t &value) {
				if (m_position >= m_end) {
					return false;
				}
				const uint8_t byte = m_data[m_position / 2];
				value = m_position & 1 ? byte & 15 : byte >> 4;
				m_position++;
				return true;
			}

			void alignByte() {
				m_position = (m_position + 1) & ~size_t{1};
			}
		};

		// Decodes the lines of one field (every other line from first) starting at a byte offset
		bool decodeField(const std::vector<uint8_t> &unit, const size_t offset, const size_t end, SubPicture &out,
		                 const int first) {
			NibbleReader reader(unit, offset, end);
			for (int line = first; line < out.height; line += 2) {
				int x = 0;
				while (x < out.width) {
					uint32_t code, n;
					if (!reader.next(code)) return false;
					if (code < 0x4) {
						if (!reader.next(n)) return false;
						code = code << 4 | n;
						if (code < 0x10) {
							if (!reader.next(n)) return false;
							code = code << 4 | n;
							if (code < 0x40) {
								if (!reader.next(n)) return false;
								code = code << 4 | n;
							}
						}
					}
					int run = static_cast<int>(code >> 2);
					// A run of 0 fills the rest of the line
					if (run == 0 || x + run > out.width) {
						run = out.width - x;
					}
					std::fill_n(out.pixels.begin() + line * out.width + x, run, static_cast<uint8_t>(code & 3));
					x += run;
				}
				reader.alignByte();
			}
			return true;
		}
	}

	bool decodeSubPicture(const std::vector<uint8_t> &unit, SubPicture &out) {
		if (unit.size() < 4) {
			return false;
		}
		const size_t size = std::min<size_t>(unit.size(), unit[0] << 8 | unit[1]);
		size_t sequence = unit[2] << 8 | unit[3];
		size_t topField = 0, bottomField = 0;
		int x1 = 0, x2 = -1, y1 = 0, y2 = -1;
		out.startMs = 0;
		out.stopMs = -1;
		out.forced = false;
		// Control sequences, each with a delay and the offset of the next; the last points to itself
		for (int count = 0; count < 64 && sequence + 4 <= size; count++) {
			const int32_t delayMs = (unit[sequence] << 8 | unit[sequence + 1]) * 1024 / 90;
			const size_t next = unit[sequence + 2] << 8 | unit[sequence + 3];
			size_t p = sequence + 4;
			bool end = false;
			while (p < size && !end) {
				switch (unit[p++]) {
					case 0x00: // forced start display
						out.forced = true;
						out.startMs = delayMs;
						break;
					case 0x01: // start display
						out.startMs = delayMs;
						break;
					case 0x02: // stop display
						out.stopMs = delayMs;
						break;
					case 0x03: // palette entries, for pixel values 3, 2, 1, 0
						if (p + 2 > size) return false;
						out.colors = {static_cast<uint8_t>(unit[p + 1] & 15), static_cast<uint8_t>(unit[p + 1] >> 4),
						              static_cast<uint8_t>(unit[p] & 15), static_cast<uint8_t>(unit[p] >> 4)};
						p += 2;
						break;
					case 0x04: // contrast, the same layout
						if (p + 2 > size) return false;
						out.contrast = {static_cast<uint8_t>(unit[p + 1] & 15), static_cast<uint8_t>(unit[p + 1] >> 4),
						                static_cast<uint8_t>(unit[p] & 15), static_cast<uint8_t>(unit[p] >> 4)};
						p += 2;
						break;
					case 0x05: // display area: x1, x2, y1, y2 in 12 bits each
						if (p + 6 > size) return false;
						x1 = unit[p] << 4 | unit[p + 1] >> 4;
						x2 = (unit[p + 1] & 15) << 8 | unit[p + 2];
						y1 = unit[p + 3] << 4 | unit[p + 4] >> 4;
						y2 = (unit[p + 4] & 15) << 8 | unit[p + 5];
						p += 6;
						break;
					case 0x06: // pixel data of the top and bottom fields
						if (p + 4 > size) return false;
						topField = unit[p] << 8 | unit[p + 1];
						bottomField = unit[p + 2] << 8 | unit[p + 3];
						p += 4;
						break;
					case 0x07: { // colour and contrast changes within the area: not supported, skipped
						if (p + 2 > size) return false;
						p += unit[p] << 8 | unit[p + 1];
						break;
					}
					case 0xFF:
						end = true;
						break;
					default:
						return false;
				}
			}
			if (next == sequence) {
				break;
			}
			sequence = next;
		}
		out.x = x1;
		out.y = y1;
		out.width = x2 - x1 + 1;
		out.height = y2 - y1 + 1;
		if (out.width <= 0 || out.height <= 0 || out.width > 1920 || out.height > 1080 || !topField || !bottomField) {
			out.pixels.clear();
			return out.width >= 0;
		}
		out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);
		const size_t dataEnd = unit[2] << 8 | unit[3];
		return decodeField(unit, topField, std::min(dataEnd, size), out, 0)
		       && decodeField(unit, bottomField, std::min(dataEnd, size), out, 1);
	}

	void SubPictureDecoder::writeByte(const uint8_t data) {
		if (m_unit.empty()) {
			// A new unit takes the PTS of the packet it starts in
			m_unitPtsMillis = m_ptsMillis;
		}
		m_unit.push_back(data);
		if (m_unit.size() == 2) {
			m_unitSize = m_unit[0] << 8 | m_unit[1];
		}
		if (m_unit.size() >= 2 && m_unit.size() >= m_unitSize) {
			SubPicture picture;
			if (m_unitSize >= 4 && decodeSubPicture(m_unit, picture)) {
				picture.ptsMs = m_unitPtsMillis;
				m_decoded.push_back(std::move(picture));
			}
			m_unit.clear();
			m_unitSize = 0;
		}
	}

	std::vector<SubPicture> SubPictureDecoder::take() {
		return std::exchange(m_decoded, {});
	}

	void SubPictureDecoder::reset() {
		m_unit.clear();
		m_unitSize = 0;
		m_decoded.clear();
	}
}
