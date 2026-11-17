#pragma once

// DVD-Video information files (.IFO): the video manager (VIDEO_TS.IFO) and the title sets (VTS_nn_0.IFO).
// Layouts follow the DVD-Video format as documented by the libdvdread project and the DVD-Video information
// pages at dvd.sourceforge.net. All values are big-endian. Sector numbers are 2048-byte sectors.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../MException.h"

namespace Dvd {
	customException(IfoException);

	constexpr uint32_t SECTOR_SIZE = 2048;

	// A navigation command, 8 bytes
	using Command = std::array<uint8_t, 8>;

	struct Cell {
		// Angle blocks: block mode 1 = first cell of a block, 2 = in a block, 3 = last cell
		uint8_t blockMode = 0;
		uint8_t blockType = 0;
		bool seamless = false;
		bool interleaved = false;
		// The SCR/PTS restart at this cell
		bool stcDiscontinuity = false;
		// Seconds to hold the last picture, 255 = until the user acts
		uint8_t stillTime = 0;
		// Cell command to run after the cell, 1-based, 0 = none
		uint8_t commandNumber = 0;
		uint32_t playbackTimeMs = 0;
		uint32_t firstSector = 0;
		uint32_t lastVobuStartSector = 0;
		uint32_t lastSector = 0;
		uint16_t vobId = 0;
		uint8_t cellId = 0;
	};

	// Program chain: cells played in order, with the commands around them
	struct Pgc {
		uint8_t programCount = 0;
		uint32_t playbackTimeMs = 0;
		uint32_t prohibitedUserOperations = 0;
		// By audio stream number: bit 15 = available, bits 8-10 = the stream's number in the VOB (sub-stream 0x80+n)
		std::array<uint16_t, 8> audioControl{};
		// By sub-picture stream number: bit 31 = available, then the stream numbers for 4:3, wide, letterbox and
		// pan-scan in the next bytes
		std::array<uint32_t, 32> subpictureControl{};
		uint16_t nextPgc = 0;
		uint16_t previousPgc = 0;
		uint16_t goUpPgc = 0;
		// Seconds to hold the last picture after all cells, 255 = until the user acts
		uint8_t stillTime = 0;
		// 0 = sequential, else random or shuffle
		uint8_t playbackMode = 0;
		// Sub-picture palette: 0x00YYCrCb
		std::array<uint32_t, 16> palette{};
		std::vector<Command> preCommands;
		std::vector<Command> postCommands;
		std::vector<Command> cellCommands;
		// First cell (1-based) of each program
		std::vector<uint8_t> programMap;
		std::vector<Cell> cells;
	};

	// A program chain in a table, with its entry flags
	struct PgcEntry {
		// Bit 7: an entry PGC (of a title, or a menu); the low 4 bits of a menu entry give the menu type
		uint8_t entryId = 0;
		Pgc pgc;

		[[nodiscard]] bool isEntry() const { return entryId & 0x80; }
		// 2 title, 3 root, 4 sub-picture, 5 audio, 6 angle, 7 chapter (part of title)
		[[nodiscard]] uint8_t menuType() const { return entryId & 0x0F; }
	};

	struct MenuLanguageUnit {
		std::string language;
		std::vector<PgcEntry> pgcs;
	};

	struct Title {
		uint8_t playbackType = 0;
		uint8_t angles = 0;
		uint16_t chapters = 0;
		uint8_t titleSet = 0;
		// Title number within its title set
		uint8_t titleSetTitle = 0;
	};

	// Chapter (part of title): its program chain and program, 1-based
	struct Chapter {
		uint16_t pgc = 0;
		uint16_t program = 0;
	};

	struct VideoAttributes {
		uint8_t mpegVersion = 0; // 0 MPEG-1, 1 MPEG-2
		uint8_t standard = 0; // 0 NTSC, 1 PAL
		uint8_t aspect = 0; // 0 4:3, 3 16:9
		uint8_t permittedDisplay = 0;
	};

	struct AudioAttributes {
		uint8_t coding = 0; // 0 A/52, 2 MPEG-1, 3 MPEG-2 ext, 4 LPCM, 6 DTS
		uint8_t channels = 0;
		std::string language;
	};

	struct SubpictureAttributes {
		std::string language;
	};

	// One .IFO file
	struct Ifo {
		bool videoManager = false;

		// Video manager only
		std::optional<Pgc> firstPlay;
		std::vector<Title> titles;
		uint16_t titleSetCount = 0;

		// Menus: the video manager's or the title set's
		std::vector<MenuLanguageUnit> menus;
		uint32_t menuVobStartSector = 0;
		VideoAttributes menuVideo;
		std::vector<AudioAttributes> menuAudio;
		std::vector<SubpictureAttributes> menuSubpictures;

		// Title set only
		std::vector<PgcEntry> titlePgcs;
		// By title set title, its chapters
		std::vector<std::vector<Chapter>> chapters;
		uint32_t titleVobStartSector = 0;
		VideoAttributes titleVideo;
		std::vector<AudioAttributes> titleAudio;
		std::vector<SubpictureAttributes> titleSubpictures;

		// The menus for a language, or the first unit if it has none
		[[nodiscard]] const MenuLanguageUnit *menuUnit(const std::string &language) const;
	};

	/**
	 * Parses an .IFO file.
	 * @throws IfoException if it isn't one or is damaged
	 */
	Ifo parseIfo(const std::vector<uint8_t> &data);

	// Reads and parses an .IFO file, falling back to its .BUP backup copy if the .IFO can't be read
	Ifo readIfo(const std::string &ifoPath);

	// BCD playback time (hours, minutes, seconds, frames with the rate in the top 2 bits) in ms
	uint32_t bcdTimeMs(const uint8_t *time);
}
