#include "Ifo.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace Dvd {
	namespace {
		// Bounds-checked big-endian reads
		class Reader {
			const std::vector<uint8_t> &m_data;

		public:
			explicit Reader(const std::vector<uint8_t> &data) : m_data(data) {
			}

			[[nodiscard]] size_t size() const {
				return m_data.size();
			}

			void need(const size_t offset, const size_t bytes) const {
				if (offset > m_data.size() || bytes > m_data.size() - offset) {
					cthrow(IfoException, "IFO data out of range at " << offset);
				}
			}

			[[nodiscard]] uint8_t u8(const size_t offset) const {
				need(offset, 1);
				return m_data[offset];
			}

			[[nodiscard]] uint16_t u16(const size_t offset) const {
				need(offset, 2);
				return static_cast<uint16_t>(m_data[offset] << 8 | m_data[offset + 1]);
			}

			[[nodiscard]] uint32_t u32(const size_t offset) const {
				need(offset, 4);
				return static_cast<uint32_t>(m_data[offset]) << 24 | m_data[offset + 1] << 16 | m_data[offset + 2] << 8
				       | m_data[offset + 3];
			}

			[[nodiscard]] const uint8_t *at(const size_t offset, const size_t bytes) const {
				need(offset, bytes);
				return m_data.data() + offset;
			}

			[[nodiscard]] std::string text(const size_t offset, const size_t bytes) const {
				const auto *p = at(offset, bytes);
				std::string result;
				for (size_t i = 0; i < bytes && p[i]; i++) {
					result += static_cast<char>(p[i]);
				}
				return result;
			}
		};

		Command readCommand(const Reader &r, const size_t offset) {
			Command command;
			std::memcpy(command.data(), r.at(offset, 8), 8);
			return command;
		}

		Pgc parsePgc(const Reader &r, const size_t start) {
			Pgc pgc;
			pgc.programCount = r.u8(start + 2);
			const uint8_t cellCount = r.u8(start + 3);
			pgc.playbackTimeMs = bcdTimeMs(r.at(start + 4, 4));
			pgc.prohibitedUserOperations = r.u32(start + 8);
			for (int i = 0; i < 8; i++) {
				pgc.audioControl[i] = r.u16(start + 0x0C + i * 2);
			}
			for (int i = 0; i < 32; i++) {
				pgc.subpictureControl[i] = r.u32(start + 0x1C + i * 4);
			}
			pgc.nextPgc = r.u16(start + 0x9C);
			pgc.previousPgc = r.u16(start + 0x9E);
			pgc.goUpPgc = r.u16(start + 0xA0);
			pgc.stillTime = r.u8(start + 0xA2);
			pgc.playbackMode = r.u8(start + 0xA3);
			for (int i = 0; i < 16; i++) {
				pgc.palette[i] = r.u32(start + 0xA4 + i * 4) & 0xFFFFFF;
			}
			const uint16_t commandOffset = r.u16(start + 0xE4);
			const uint16_t programMapOffset = r.u16(start + 0xE6);
			const uint16_t cellPlaybackOffset = r.u16(start + 0xE8);
			const uint16_t cellPositionOffset = r.u16(start + 0xEA);

			if (commandOffset) {
				const size_t table = start + commandOffset;
				const uint16_t pre = r.u16(table);
				const uint16_t post = r.u16(table + 2);
				const uint16_t cell = r.u16(table + 4);
				if (pre + post + cell > 128) {
					cthrow(IfoException, "Too many PGC commands");
				}
				size_t offset = table + 8;
				for (int i = 0; i < pre; i++, offset += 8) pgc.preCommands.push_back(readCommand(r, offset));
				for (int i = 0; i < post; i++, offset += 8) pgc.postCommands.push_back(readCommand(r, offset));
				for (int i = 0; i < cell; i++, offset += 8) pgc.cellCommands.push_back(readCommand(r, offset));
			}
			if (programMapOffset && pgc.programCount) {
				const auto *map = r.at(start + programMapOffset, pgc.programCount);
				pgc.programMap.assign(map, map + pgc.programCount);
			}
			if (cellPlaybackOffset && cellCount) {
				for (int i = 0; i < cellCount; i++) {
					const size_t c = start + cellPlaybackOffset + i * 24;
					Cell cell;
					const uint8_t flags = r.u8(c);
					cell.blockMode = flags >> 6;
					cell.blockType = (flags >> 4) & 3;
					cell.seamless = flags & 8;
					cell.interleaved = flags & 4;
					cell.stcDiscontinuity = flags & 2;
					cell.stillTime = r.u8(c + 2);
					cell.commandNumber = r.u8(c + 3);
					cell.playbackTimeMs = bcdTimeMs(r.at(c + 4, 4));
					cell.firstSector = r.u32(c + 8);
					cell.lastVobuStartSector = r.u32(c + 16);
					cell.lastSector = r.u32(c + 20);
					if (cellPositionOffset) {
						const size_t p = start + cellPositionOffset + i * 4;
						cell.vobId = r.u16(p);
						cell.cellId = r.u8(p + 3);
					}
					pgc.cells.push_back(cell);
				}
			}
			return pgc;
		}

		// A table of program chains: count, then 8-byte entries (entry id, block, parental mask, offset from the
		// table's start)
		std::vector<PgcEntry> parsePgcTable(const Reader &r, const size_t table) {
			std::vector<PgcEntry> entries;
			const uint16_t count = r.u16(table);
			for (int i = 0; i < count; i++) {
				const size_t e = table + 8 + i * 8;
				PgcEntry entry;
				entry.entryId = r.u8(e);
				entry.pgc = parsePgc(r, table + r.u32(e + 4));
				entries.push_back(std::move(entry));
			}
			return entries;
		}

		std::vector<MenuLanguageUnit> parseMenuUnits(const Reader &r, const uint32_t sector) {
			std::vector<MenuLanguageUnit> units;
			if (!sector) {
				return units;
			}
			const size_t table = static_cast<size_t>(sector) * SECTOR_SIZE;
			const uint16_t count = r.u16(table);
			for (int i = 0; i < count; i++) {
				const size_t e = table + 8 + i * 8;
				MenuLanguageUnit unit;
				unit.language = r.text(e, 2);
				unit.pgcs = parsePgcTable(r, table + r.u32(e + 4));
				units.push_back(std::move(unit));
			}
			return units;
		}

		VideoAttributes parseVideo(const Reader &r, const size_t offset) {
			const uint8_t a = r.u8(offset);
			return {static_cast<uint8_t>(a >> 6), static_cast<uint8_t>((a >> 4) & 3), static_cast<uint8_t>((a >> 2) & 3),
			        static_cast<uint8_t>(a & 3)};
		}

		std::vector<AudioAttributes> parseAudio(const Reader &r, const size_t offset, const int maximum) {
			std::vector<AudioAttributes> streams;
			const int count = std::min<int>(r.u16(offset), maximum);
			for (int i = 0; i < count; i++) {
				const size_t a = offset + 2 + i * 8;
				AudioAttributes audio;
				audio.coding = r.u8(a) >> 5;
				audio.channels = (r.u8(a + 1) & 7) + 1;
				if ((r.u8(a) >> 2 & 3) == 1) {
					audio.language = r.text(a + 2, 2);
				}
				streams.push_back(audio);
			}
			return streams;
		}

		std::vector<SubpictureAttributes> parseSubpictures(const Reader &r, const size_t offset, const int maximum) {
			std::vector<SubpictureAttributes> streams;
			const int count = std::min<int>(r.u16(offset), maximum);
			for (int i = 0; i < count; i++) {
				const size_t s = offset + 2 + i * 6;
				SubpictureAttributes subpicture;
				if ((r.u8(s) & 3) == 1) {
					subpicture.language = r.text(s + 2, 2);
				}
				streams.push_back(subpicture);
			}
			return streams;
		}
	}

	uint32_t bcdTimeMs(const uint8_t *time) {
		const auto bcd = [](const uint8_t v) { return (v >> 4) * 10 + (v & 15); };
		const uint32_t frames = bcd(time[3] & 0x3F);
		const uint32_t rate = (time[3] >> 6) == 1 ? 25 : 30;
		return ((bcd(time[0]) * 60 + bcd(time[1])) * 60 + bcd(time[2])) * 1000 + frames * 1000 / rate;
	}

	const MenuLanguageUnit *Ifo::menuUnit(const std::string &language) const {
		for (const auto &unit: menus) {
			if (unit.language == language) {
				return &unit;
			}
		}
		return menus.empty() ? nullptr : &menus.front();
	}

	Ifo parseIfo(const std::vector<uint8_t> &data) {
		const Reader r(data);
		Ifo ifo;
		const std::string id = r.text(0, 12);
		if (id == "DVDVIDEO-VMG") {
			ifo.videoManager = true;
			ifo.titleSetCount = r.u16(0x3E);
			if (const uint32_t firstPlay = r.u32(0x84)) {
				ifo.firstPlay = parsePgc(r, firstPlay);
			}
			ifo.menuVobStartSector = r.u32(0xC0);
			if (const uint32_t titleTable = r.u32(0xC4)) {
				const size_t table = static_cast<size_t>(titleTable) * SECTOR_SIZE;
				const uint16_t count = r.u16(table);
				for (int i = 0; i < count; i++) {
					const size_t t = table + 8 + i * 12;
					Title title;
					title.playbackType = r.u8(t);
					title.angles = r.u8(t + 1);
					title.chapters = r.u16(t + 2);
					title.titleSet = r.u8(t + 6);
					title.titleSetTitle = r.u8(t + 7);
					ifo.titles.push_back(title);
				}
			}
			ifo.menus = parseMenuUnits(r, r.u32(0xC8));
			ifo.menuVideo = parseVideo(r, 0x100);
			ifo.menuAudio = parseAudio(r, 0x102, 1);
			ifo.menuSubpictures = parseSubpictures(r, 0x154, 1);
		} else if (id == "DVDVIDEO-VTS") {
			ifo.menuVobStartSector = r.u32(0xC0);
			ifo.titleVobStartSector = r.u32(0xC4);
			if (const uint32_t pttTable = r.u32(0xC8)) {
				const size_t table = static_cast<size_t>(pttTable) * SECTOR_SIZE;
				const uint16_t count = r.u16(table);
				const uint32_t lastByte = r.u32(table + 4);
				for (int i = 0; i < count; i++) {
					const uint32_t start = r.u32(table + 8 + i * 4);
					const uint32_t end = i + 1 < count ? r.u32(table + 8 + (i + 1) * 4) : lastByte + 1;
					std::vector<Chapter> chapters;
					for (uint32_t c = start; c + 4 <= end; c += 4) {
						chapters.push_back({r.u16(table + c), r.u16(table + c + 2)});
					}
					ifo.chapters.push_back(std::move(chapters));
				}
			}
			if (const uint32_t pgcTable = r.u32(0xCC)) {
				ifo.titlePgcs = parsePgcTable(r, static_cast<size_t>(pgcTable) * SECTOR_SIZE);
			}
			ifo.menus = parseMenuUnits(r, r.u32(0xD0));
			ifo.menuVideo = parseVideo(r, 0x100);
			ifo.menuAudio = parseAudio(r, 0x102, 1);
			ifo.menuSubpictures = parseSubpictures(r, 0x154, 1);
			ifo.titleVideo = parseVideo(r, 0x200);
			ifo.titleAudio = parseAudio(r, 0x202, 8);
			ifo.titleSubpictures = parseSubpictures(r, 0x254, 32);
		} else {
			cthrow(IfoException, "Not a DVD-Video IFO file");
		}
		return ifo;
	}

	Ifo readIfo(const std::string &ifoPath) {
		auto read = [](const std::string &path) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				cthrow(IfoException, "Cannot open " << path);
			}
			return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		};
		try {
			return parseIfo(read(ifoPath));
		} catch (const IfoException &) {
			std::string backup = ifoPath;
			if (backup.size() > 4) {
				const bool lower = backup.back() == 'o';
				backup.replace(backup.size() - 3, 3, lower ? "bup" : "BUP");
			}
			return parseIfo(read(backup));
		}
	}
}
