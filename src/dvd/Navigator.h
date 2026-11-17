#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "Disc.h"

namespace Dvd {
	enum class Domain {
		FirstPlay,
		VideoManagerMenu,
		TitleSetMenu,
		Title,
		Stopped
	};

	// Menu types, as in a menu PGC's entry id
	enum MenuType : uint8_t {
		TITLE_MENU = 2,
		ROOT_MENU = 3,
		SUBPICTURE_MENU = 4,
		AUDIO_MENU = 5,
		ANGLE_MENU = 6,
		CHAPTER_MENU = 7
	};

	// Where a cell's sectors are
	struct CellLocation {
		// The video manager's menus (VIDEO_TS.VOB), or a title set's menus (VTS_nn_0.VOB) or titles (VTS_nn_1.VOB...)
		Domain domain = Domain::Stopped;
		int titleSet = 0;
		uint32_t firstSector = 0;
		uint32_t lastSector = 0;
		// Seconds to hold the last picture, 255 = until the user acts
		uint8_t stillTime = 0;
	};

	/**
	 * The DVD-Video navigation virtual machine: runs a disc's program chains and their commands, from First Play
	 * through menus and titles, and says which cell to play. The player plays the cell, then calls cellFinished(),
	 * or buttonActivated() when the user picks a menu button.
	 *
	 * Command encodings follow the DVD-Video format as documented by the libdvdnav project.
	 */
	class Navigator {
	public:
		explicit Navigator(Disc &disc);

		// Starts from the First Play program chain
		void start();

		[[nodiscard]] Domain domain() const { return m_domain; }
		[[nodiscard]] bool stopped() const { return m_domain == Domain::Stopped; }
		// The cell to play now, nullopt when stopped
		[[nodiscard]] std::optional<CellLocation> cell() const;
		// Increments whenever playback moves somewhere other than the next cell, so a cell being played is abandoned
		[[nodiscard]] uint64_t jumpCount() const { return m_jumpCount; }
		[[nodiscard]] const Pgc *pgc() const { return m_pgc; }
		[[nodiscard]] int cellNumber() const { return m_cell; }

		// The cell has played, including its still time
		void cellFinished();
		// The PGC's still time after its last cell (seconds, 255 = until the user acts) is due; call
		// pgcStillFinished() after it
		[[nodiscard]] bool inPgcStill() const { return m_pgcStill; }
		void pgcStillFinished();

		// Runs a menu button's command
		void buttonActivated(const Command &command);
		// The user asked for a menu (TITLE_MENU or ROOT_MENU, ...)
		void menuCall(MenuType menu);
		// The next or previous chapter (program) of a title
		void nextChapter();
		void previousChapter();
		// Plays a cell of the current program chain, e.g. to seek
		void jumpToCell(int cell);

		// Highlighted button, 1-36
		[[nodiscard]] int highlightedButton() const { return m_sprm[8] >> 10; }
		void setHighlightedButton(int button);

		// Sub-stream id (0x80-0x87 for A/52) of the audio stream to play, -1 for none
		[[nodiscard]] int audioStreamId() const;
		// Sub-stream id (0x20-0x3F) of the sub-picture stream, -1 for none; displayAll is false when only forced
		// sub-pictures (e.g. menus' buttons) should show
		[[nodiscard]] int subpictureStreamId(bool wide, bool &displayAll) const;
		// Sub-picture palette (0x00YYCrCb) of the current program chain
		[[nodiscard]] const std::array<uint32_t, 16> *palette() const;
		[[nodiscard]] bool wideScreen() const;

		[[nodiscard]] uint16_t sprm(const int index) const { return m_sprm[index]; }
		[[nodiscard]] uint16_t gprm(const int index) const { return m_gprm[index]; }

	private:
		// A link to follow, from a command
		struct Link {
			enum Kind : uint8_t {
				// Link sub-instructions keep the format's numbers
				None = 0,
				TopCell = 1,
				NextCell = 2,
				PreviousCell = 3,
				TopProgram = 5,
				NextProgram = 6,
				PreviousProgram = 7,
				TopPgc = 9,
				NextPgc = 10,
				PreviousPgc = 11,
				GoUpPgc = 12,
				TailPgc = 13,
				Resume = 16,
				// The rest are numbered here
				PgcNumber = 32,
				Chapter,
				Program,
				CellNumber,
				Exit,
				JumpTitle,
				JumpTitleSetTitle,
				JumpChapter,
				JumpFirstPlay,
				JumpVideoManagerMenu,
				JumpTitleSetMenu,
				JumpVideoManagerPgc,
				CallFirstPlay,
				CallVideoManagerMenu,
				CallTitleSetMenu,
				CallVideoManagerPgc
			};

			Kind kind = None;
			uint16_t data1 = 0;
			uint16_t data2 = 0;
			uint16_t data3 = 0;
		};

		// Where a menu call returns to
		struct ResumePoint {
			bool valid = false;
			int titleSet = 0;
			int pgc = 0;
			int cell = 0;
			std::array<uint16_t, 5> sprm{}; // SPRM 4-8
		};

		// --- commands
		[[nodiscard]] uint16_t reg(uint8_t r) const;
		uint16_t gprmValue(int index) const;
		void setGprm(int index, uint16_t value);
		// Runs commands from the first, returns a link to follow if one was taken
		std::optional<Link> run(const std::vector<Command> &commands);
		// Runs one command: returns a link, or the line to go to (0 = next, 256 = break)
		bool execute(const Command &command, Link &link, int &gotoLine);

		// --- navigation, each continuing until a cell is ready to play or playback stops
		// Returns false for a link that goes nowhere (it may only select a button)
		bool follow(Link link);
		void playPgc(int pgcNumber, int program = 1, int cell = 0);
		void playProgram(int program);
		void playCell(int cell);
		void afterCell();
		void playPgcPost();
		bool setMenu(MenuType menu);
		bool setTitle(int titleSet, int titleSetTitle, int chapter);
		void saveResume(int resumeCell);
		void stop();

		[[nodiscard]] const std::vector<PgcEntry> *pgcTable() const;
		[[nodiscard]] int programOfCell(int cell) const;

		Disc &m_disc;
		Domain m_domain = Domain::Stopped;
		int m_titleSet = 0;
		int m_pgcNumber = 0;
		const Pgc *m_pgc = nullptr;
		int m_program = 0;
		int m_cell = 0;
		bool m_pgcStill = false;
		uint64_t m_jumpCount = 0;
		// Follow() depth, to stop command loops that never reach a cell
		int m_depth = 0;

		std::array<uint16_t, 24> m_sprm{};
		std::array<uint16_t, 16> m_gprm{};
		// Counter mode GPRMs count seconds up from when they were set
		std::array<bool, 16> m_gprmCounter{};
		std::array<int64_t, 16> m_gprmCounterStartMs{};
		ResumePoint m_resume;
		std::minstd_rand m_random{1};
	};
}
