#include "Navigator.h"

#include <algorithm>
#include <iostream>

#include "../Time.h"

namespace Dvd {
	namespace {
		// count bits of a command, from bit start (63 is the first byte's top bit) down
		uint32_t bits(const Command &command, const int start, const int count) {
			uint64_t value = 0;
			for (const uint8_t b: command) {
				value = value << 8 | b;
			}
			return static_cast<uint32_t>((value >> (start - count + 1)) & ((uint64_t{1} << count) - 1));
		}

		bool compare(const uint32_t operation, const uint16_t a, const uint16_t b) {
			switch (operation) {
				case 1: return a & b;
				case 2: return a == b;
				case 3: return a != b;
				case 4: return a >= b;
				case 5: return a > b;
				case 6: return a <= b;
				case 7: return a < b;
				default: return true;
			}
		}

		constexpr int MAX_LINK_DEPTH = 64;
	}

	Navigator::Navigator(Disc &disc) : m_disc(disc) {
	}

	void Navigator::start() {
		m_sprm = {};
		m_gprm = {};
		m_gprmCounter = {};
		m_resume = {};
		// Player settings (SPRM 0 and 12-20) and the defaults of the rest
		m_sprm[0] = 'e' << 8 | 'n'; // menu language
		m_sprm[1] = 15; // audio stream: the first available
		m_sprm[2] = 62; // sub-picture stream: none
		m_sprm[3] = 1; // angle
		m_sprm[4] = 1; // title
		m_sprm[5] = 1; // title set title
		m_sprm[7] = 1; // chapter
		m_sprm[8] = 1 << 10; // highlighted button
		m_sprm[12] = 'U' << 8 | 'S'; // parental country
		m_sprm[13] = 15; // parental level
		m_sprm[14] = 0x0C00; // video: 16:9 display, normal
		m_sprm[15] = 0x7CFC; // audio capabilities
		m_sprm[16] = 'e' << 8 | 'n'; // preferred audio language
		m_sprm[18] = 'e' << 8 | 'n'; // preferred sub-picture language
		m_sprm[20] = 1; // region
		m_titleSet = 0;
		m_domain = Domain::FirstPlay;
		m_depth = 0;
		if (m_disc.videoManager().firstPlay) {
			playPgc(0);
		} else {
			// No First Play program chain: the title menu, else the first title
			m_domain = Domain::VideoManagerMenu;
			if (!setMenu(TITLE_MENU)) {
				follow({Link::JumpTitle, 1});
			}
		}
	}

	std::optional<CellLocation> Navigator::cell() const {
		if (m_domain == Domain::Stopped || !m_pgc || m_cell < 1 || m_cell > static_cast<int>(m_pgc->cells.size())) {
			return std::nullopt;
		}
		const Cell &c = m_pgc->cells[m_cell - 1];
		CellLocation location;
		location.domain = m_domain == Domain::FirstPlay ? Domain::VideoManagerMenu : m_domain;
		location.titleSet = location.domain == Domain::VideoManagerMenu ? 0 : m_titleSet;
		location.firstSector = c.firstSector;
		location.lastSector = c.lastSector;
		location.stillTime = c.stillTime;
		return location;
	}

	// ---------------------------------------------------------------- registers

	uint16_t Navigator::gprmValue(const int index) const {
		if (m_gprmCounter[index]) {
			return static_cast<uint16_t>(m_gprm[index] + (now() - m_gprmCounterStartMs[index]) / 1000);
		}
		return m_gprm[index];
	}

	void Navigator::setGprm(const int index, const uint16_t value) {
		m_gprm[index] = value;
		if (m_gprmCounter[index]) {
			m_gprmCounterStartMs[index] = now();
		}
	}

	uint16_t Navigator::reg(const uint8_t r) const {
		if (r & 0x80) {
			const int index = r & 0x1F;
			return index < 24 ? m_sprm[index] : 0;
		}
		return gprmValue(r & 0x0F);
	}

	void Navigator::setHighlightedButton(const int button) {
		if (button >= 1 && button <= 36) {
			m_sprm[8] = static_cast<uint16_t>(button << 10);
		}
	}

	// ---------------------------------------------------------------- commands

	bool Navigator::execute(const Command &c, Link &link, int &gotoLine) {
		// Register or immediate value: immediate 16 bits at start, else a register in the 8 bits before
		const auto regOrData = [&](const bool immediate, const int start) -> uint16_t {
			return immediate ? static_cast<uint16_t>(bits(c, start, 16)) : reg(static_cast<uint8_t>(bits(c, start - 8, 8)));
		};
		// Immediate 7 bits, or a GPRM in the low 4 bits
		const auto regOrData2 = [&](const bool immediate, const int start) -> uint16_t {
			return immediate ? static_cast<uint16_t>(bits(c, start - 1, 7)) : gprmValue(static_cast<int>(bits(c, start - 4, 4)));
		};
		const uint32_t compareOp = bits(c, 54, 3);
		const bool compareImmediate = bits(c, 55, 1);
		// The comparisons, by where their operands are
		const auto if1 = [&] {
			return !compareOp || compare(compareOp, reg(static_cast<uint8_t>(bits(c, 39, 8))), regOrData(compareImmediate, 31));
		};
		const auto if2 = [&] {
			return !compareOp || compare(compareOp, reg(static_cast<uint8_t>(bits(c, 15, 8))), reg(static_cast<uint8_t>(bits(c, 7, 8))));
		};
		const auto if3 = [&] {
			return !compareOp || compare(compareOp, reg(static_cast<uint8_t>(bits(c, 47, 8))), regOrData(compareImmediate, 15));
		};
		const auto if4 = [&] {
			return !compareOp || compare(compareOp, reg(static_cast<uint8_t>(bits(c, 51, 4))), regOrData(compareImmediate, 31));
		};
		const auto if5 = [&] {
			if (!compareOp) {
				return true;
			}
			if (bits(c, 60, 1)) {
				return compare(compareOp, reg(static_cast<uint8_t>(bits(c, 31, 8))), reg(static_cast<uint8_t>(bits(c, 23, 8))));
			}
			return compare(compareOp, reg(static_cast<uint8_t>(bits(c, 39, 8))), regOrData(compareImmediate, 31));
		};
		const auto setOp = [&](const uint32_t op, const int r, const int r2, const uint16_t data) {
			const int32_t value = gprmValue(r);
			switch (op) {
				case 1: setGprm(r, data); break;
				case 2: // swap
					setGprm(r2, static_cast<uint16_t>(value));
					setGprm(r, data);
					break;
				case 3: setGprm(r, static_cast<uint16_t>(std::min(value + data, 0xFFFF))); break;
				case 4: setGprm(r, static_cast<uint16_t>(std::max(value - data, 0))); break;
				case 5: setGprm(r, static_cast<uint16_t>(std::min(value * data, 0xFFFF))); break;
				case 6: setGprm(r, data ? static_cast<uint16_t>(value / data) : 0xFFFF); break;
				case 7: setGprm(r, data ? static_cast<uint16_t>(value % data) : 0xFFFF); break;
				case 8: // random 1..data
					setGprm(r, static_cast<uint16_t>(data ? m_random() % data + 1 : 1));
					break;
				case 9: setGprm(r, static_cast<uint16_t>(value & data)); break;
				case 10: setGprm(r, static_cast<uint16_t>(value | data)); break;
				case 11: setGprm(r, static_cast<uint16_t>(value ^ data)); break;
				default: break;
			}
		};
		const auto linkSubInstruction = [&](const bool condition) {
			const uint32_t op = bits(c, 4, 5);
			if (op > 0x10) {
				return false;
			}
			link = {static_cast<Link::Kind>(op), static_cast<uint16_t>(bits(c, 15, 6))};
			return condition;
		};
		const auto linkInstruction = [&](const bool condition) {
			switch (bits(c, 51, 4)) {
				case 1: return linkSubInstruction(condition);
				case 4: link = {Link::PgcNumber, static_cast<uint16_t>(bits(c, 14, 15))}; return condition;
				case 5: link = {Link::Chapter, static_cast<uint16_t>(bits(c, 9, 10)), static_cast<uint16_t>(bits(c, 15, 6))}; return condition;
				case 6: link = {Link::Program, static_cast<uint16_t>(bits(c, 6, 7)), static_cast<uint16_t>(bits(c, 15, 6))}; return condition;
				case 7: link = {Link::CellNumber, static_cast<uint16_t>(bits(c, 7, 8)), static_cast<uint16_t>(bits(c, 15, 6))}; return condition;
				default: return false;
			}
		};
		const auto jumpInstruction = [&](const bool condition) {
			switch (bits(c, 51, 4)) {
				case 1: link = {Link::Exit}; return condition;
				case 2: link = {Link::JumpTitle, static_cast<uint16_t>(bits(c, 22, 7))}; return condition;
				case 3: link = {Link::JumpTitleSetTitle, static_cast<uint16_t>(bits(c, 22, 7))}; return condition;
				case 5: link = {Link::JumpChapter, static_cast<uint16_t>(bits(c, 22, 7)), static_cast<uint16_t>(bits(c, 41, 10))}; return condition;
				case 6:
					switch (bits(c, 23, 2)) {
						case 0: link = {Link::JumpFirstPlay}; return condition;
						case 1: link = {Link::JumpVideoManagerMenu, static_cast<uint16_t>(bits(c, 19, 4))}; return condition;
						case 2:
							link = {Link::JumpTitleSetMenu, static_cast<uint16_t>(bits(c, 31, 8)), static_cast<uint16_t>(bits(c, 39, 8)),
							        static_cast<uint16_t>(bits(c, 19, 4))};
							return condition;
						default: link = {Link::JumpVideoManagerPgc, static_cast<uint16_t>(bits(c, 46, 15))}; return condition;
					}
				case 8:
					switch (bits(c, 23, 2)) {
						case 0: link = {Link::CallFirstPlay, static_cast<uint16_t>(bits(c, 31, 8))}; return condition;
						case 1:
							link = {Link::CallVideoManagerMenu, static_cast<uint16_t>(bits(c, 19, 4)), static_cast<uint16_t>(bits(c, 31, 8))};
							return condition;
						case 2:
							link = {Link::CallTitleSetMenu, static_cast<uint16_t>(bits(c, 19, 4)), static_cast<uint16_t>(bits(c, 31, 8))};
							return condition;
						default:
							link = {Link::CallVideoManagerPgc, static_cast<uint16_t>(bits(c, 46, 15)), static_cast<uint16_t>(bits(c, 31, 8))};
							return condition;
					}
				default: return false;
			}
		};

		switch (bits(c, 63, 3)) {
			case 0: { // special: nop, goto, break, set parental level and goto
				const bool condition = if1();
				switch (bits(c, 51, 4)) {
					case 1:
						if (condition) gotoLine = static_cast<int>(bits(c, 7, 8));
						break;
					case 2:
						if (condition) gotoLine = 256;
						break;
					case 3:
						if (condition) {
							m_sprm[13] = static_cast<uint16_t>(bits(c, 11, 4));
							gotoLine = static_cast<int>(bits(c, 7, 8));
						}
						break;
					default: break;
				}
				return false;
			}
			case 1: // link or jump
				if (bits(c, 60, 1)) {
					return jumpInstruction(if2());
				}
				return linkInstruction(if1());
			case 2: { // set system registers, then maybe link
				const bool condition = if2();
				const bool immediate = bits(c, 60, 1);
				switch (bits(c, 59, 4)) {
					case 1: // audio, sub-picture, angle streams
						for (int i = 1; i <= 3; i++) {
							if (bits(c, 63 - (2 + i) * 8, 1)) {
								const uint16_t data = regOrData2(immediate, 47 - i * 8);
								if (condition) m_sprm[i] = data;
							}
						}
						break;
					case 2: { // navigation timer and its PGC
						const uint16_t data = regOrData(immediate, 47);
						if (condition) {
							m_sprm[9] = data;
							m_sprm[10] = static_cast<uint16_t>(bits(c, 23, 8));
						}
						break;
					}
					case 3: { // GPRM mode (register or counter) and value
						const uint16_t data = regOrData(immediate, 47);
						const int r = static_cast<int>(bits(c, 19, 4));
						m_gprmCounter[r] = bits(c, 23, 1);
						if (condition) setGprm(r, data);
						break;
					}
					case 6: { // highlighted button
						const uint16_t data = regOrData(immediate, 31);
						if (condition) m_sprm[8] = data;
						break;
					}
					default: break;
				}
				return bits(c, 51, 4) && linkInstruction(condition);
			}
			case 3: { // set, with compare or link
				const bool condition = if3();
				if (condition) {
					setOp(bits(c, 59, 4), static_cast<int>(bits(c, 35, 4)), static_cast<int>(bits(c, 19, 4)),
					      regOrData(bits(c, 60, 1), 31));
				}
				return bits(c, 51, 4) && linkInstruction(condition);
			}
			case 4: { // set, compare, then link sub-instruction
				setOp(bits(c, 59, 4), static_cast<int>(bits(c, 51, 4)), static_cast<int>(bits(c, 35, 4)),
				      regOrData(bits(c, 60, 1), 47));
				return if4() && linkSubInstruction(true);
			}
			case 5: // compare, then set and link sub-instruction
			case 6: { // compare, then set; always link sub-instruction
				const bool condition = bits(c, 51, 1) ? if5() : if1();
				if (condition) {
					setOp(bits(c, 59, 4), static_cast<int>(bits(c, 51, 4)), static_cast<int>(bits(c, 35, 4)),
					      regOrData(bits(c, 60, 1), 47));
				}
				return linkSubInstruction(bits(c, 63, 3) == 6 || condition);
			}
			default:
				return false;
		}
	}

	std::optional<Navigator::Link> Navigator::run(const std::vector<Command> &commands) {
		int line = 1;
		// Bounded, as a Goto can loop
		for (int executed = 0; line >= 1 && line <= static_cast<int>(commands.size()) && executed < 10000; executed++) {
			Link link;
			int gotoLine = 0;
			if (execute(commands[line - 1], link, gotoLine)) {
				return link;
			}
			if (gotoLine == 256) {
				break;
			}
			line = gotoLine ? gotoLine : line + 1;
		}
		return std::nullopt;
	}

	// ---------------------------------------------------------------- navigation

	const std::vector<PgcEntry> *Navigator::pgcTable() const {
		const std::string language{static_cast<char>(m_sprm[0] >> 8), static_cast<char>(m_sprm[0] & 0xFF)};
		const Ifo *ifo = nullptr;
		switch (m_domain) {
			case Domain::VideoManagerMenu:
				ifo = &m_disc.videoManager();
				break;
			case Domain::TitleSetMenu:
			case Domain::Title:
				ifo = m_disc.titleSet(m_titleSet);
				break;
			default:
				return nullptr;
		}
		if (!ifo) {
			return nullptr;
		}
		if (m_domain == Domain::Title) {
			return &ifo->titlePgcs;
		}
		const MenuLanguageUnit *unit = ifo->menuUnit(language);
		return unit ? &unit->pgcs : nullptr;
	}

	int Navigator::programOfCell(const int cell) const {
		int program = 1;
		for (size_t i = 0; i < m_pgc->programMap.size(); i++) {
			if (m_pgc->programMap[i] <= cell) {
				program = static_cast<int>(i) + 1;
			}
		}
		return program;
	}

	void Navigator::stop() {
		m_domain = Domain::Stopped;
		m_pgc = nullptr;
		m_cell = 0;
		m_jumpCount++;
	}

	void Navigator::playPgc(const int pgcNumber, const int program, const int cell) {
		m_jumpCount++;
		m_pgcStill = false;
		if (m_domain == Domain::FirstPlay) {
			m_pgc = &*m_disc.videoManager().firstPlay;
			m_pgcNumber = 0;
		} else {
			const auto *table = pgcTable();
			if (!table || pgcNumber < 1 || pgcNumber > static_cast<int>(table->size())) {
				std::cout << "DVD: no program chain " << pgcNumber << std::endl;
				stop();
				return;
			}
			m_pgc = &(*table)[pgcNumber - 1].pgc;
			m_pgcNumber = pgcNumber;
			if (m_domain == Domain::Title) {
				m_sprm[6] = static_cast<uint16_t>(pgcNumber);
			}
		}
		if (const auto link = run(m_pgc->preCommands); link && follow(*link)) {
			return;
		}
		if (cell) {
			playCell(cell);
		} else {
			playProgram(program);
		}
	}

	void Navigator::playProgram(const int program) {
		if (program < 1 || program > static_cast<int>(m_pgc->programMap.size())) {
			playPgcPost();
			return;
		}
		playCell(m_pgc->programMap[program - 1]);
	}

	void Navigator::playCell(int cell) {
		if (cell < 1 || cell > static_cast<int>(m_pgc->cells.size())) {
			playPgcPost();
			return;
		}
		// The first cell of an angle block: play the cell for the selected angle
		const Cell &first = m_pgc->cells[cell - 1];
		if (first.blockMode == 1 && first.blockType == 1) {
			const int angle = std::max(1, static_cast<int>(m_sprm[3]));
			int candidate = cell;
			for (int a = 1; a < angle && candidate < static_cast<int>(m_pgc->cells.size())
			                && m_pgc->cells[candidate].blockMode >= 2; a++) {
				candidate++;
			}
			cell = candidate;
		}
		m_cell = cell;
		m_program = programOfCell(cell);
		if (m_domain == Domain::Title) {
			m_sprm[7] = static_cast<uint16_t>(m_program);
		}
	}

	void Navigator::cellFinished() {
		if (!m_pgc || m_domain == Domain::Stopped) {
			return;
		}
		afterCell();
	}

	void Navigator::afterCell() {
		const Cell &cell = m_pgc->cells[m_cell - 1];
		if (cell.commandNumber && cell.commandNumber <= m_pgc->cellCommands.size()) {
			if (const auto link = run({m_pgc->cellCommands[cell.commandNumber - 1]}); link && follow(*link)) {
				return;
			}
		}
		int next = m_cell + 1;
		if (cell.blockMode != 0 && cell.blockType == 1) {
			// Skip the other angles
			while (next <= static_cast<int>(m_pgc->cells.size()) && m_pgc->cells[next - 1].blockMode >= 2) {
				next++;
			}
		}
		playCell(next);
	}

	void Navigator::playPgcPost() {
		m_cell = 0;
		if (m_pgc->stillTime && !m_pgcStill) {
			m_pgcStill = true;
			return;
		}
		pgcStillFinished();
	}

	void Navigator::pgcStillFinished() {
		m_pgcStill = false;
		if (const auto link = run(m_pgc->postCommands); link && follow(*link)) {
			return;
		}
		if (m_domain == Domain::FirstPlay) {
			// First Play ended without saying where to go: the title menu, else the first title
			m_domain = Domain::VideoManagerMenu;
			if (!setMenu(TITLE_MENU)) {
				follow({Link::JumpTitle, 1});
			}
			return;
		}
		if (m_pgc->nextPgc) {
			playPgc(m_pgc->nextPgc);
			return;
		}
		stop();
	}

	bool Navigator::setMenu(const MenuType menu) {
		const auto *table = pgcTable();
		if (!table) {
			return false;
		}
		for (size_t i = 0; i < table->size(); i++) {
			if ((*table)[i].isEntry() && (*table)[i].menuType() == menu) {
				playPgc(static_cast<int>(i) + 1);
				return true;
			}
		}
		return false;
	}

	bool Navigator::setTitle(const int titleSet, const int titleSetTitle, const int chapter) {
		const Ifo *ifo = m_disc.titleSet(titleSet);
		if (!ifo || titleSetTitle < 1 || titleSetTitle > static_cast<int>(ifo->chapters.size())) {
			return false;
		}
		const auto &chapters = ifo->chapters[titleSetTitle - 1];
		if (chapter < 1 || chapter > static_cast<int>(chapters.size())) {
			return false;
		}
		m_domain = Domain::Title;
		m_titleSet = titleSet;
		const auto &titles = m_disc.videoManager().titles;
		for (size_t i = 0; i < titles.size(); i++) {
			if (titles[i].titleSet == titleSet && titles[i].titleSetTitle == titleSetTitle) {
				m_sprm[4] = static_cast<uint16_t>(i + 1);
			}
		}
		m_sprm[5] = static_cast<uint16_t>(titleSetTitle);
		m_sprm[7] = static_cast<uint16_t>(chapter);
		playPgc(chapters[chapter - 1].pgc, chapters[chapter - 1].program);
		return true;
	}

	void Navigator::saveResume(const int resumeCell) {
		if (m_domain != Domain::Title) {
			return;
		}
		m_resume.valid = true;
		m_resume.titleSet = m_titleSet;
		m_resume.pgc = m_pgcNumber;
		m_resume.cell = resumeCell ? resumeCell : m_cell;
		for (int i = 0; i < 5; i++) {
			m_resume.sprm[i] = m_sprm[4 + i];
		}
	}

	bool Navigator::follow(Link link) {
		if (m_depth >= MAX_LINK_DEPTH) {
			std::cout << "DVD: navigation commands loop, stopping" << std::endl;
			stop();
			return true;
		}
		m_depth++;
		struct DepthGuard {
			int &depth;
			~DepthGuard() { depth--; }
		} guard{m_depth};

		// Link instructions can also select a button
		if (link.kind <= Link::Resume && link.data1) {
			setHighlightedButton(link.data1);
		}
		if ((link.kind == Link::Chapter || link.kind == Link::Program || link.kind == Link::CellNumber) && link.data2) {
			setHighlightedButton(link.data2);
		}

		switch (link.kind) {
			case Link::None:
				return false;
			case Link::TopCell: playCell(m_cell); m_jumpCount++; break;
			case Link::NextCell: playCell(m_cell + 1); m_jumpCount++; break;
			case Link::PreviousCell: playCell(std::max(1, m_cell - 1)); m_jumpCount++; break;
			case Link::TopProgram: playProgram(m_program); m_jumpCount++; break;
			case Link::NextProgram: playProgram(m_program + 1); m_jumpCount++; break;
			case Link::PreviousProgram: playProgram(std::max(1, m_program - 1)); m_jumpCount++; break;
			case Link::TopPgc: playPgc(m_pgcNumber); break;
			case Link::NextPgc:
				if (!m_pgc->nextPgc) return false;
				playPgc(m_pgc->nextPgc);
				break;
			case Link::PreviousPgc:
				if (!m_pgc->previousPgc) return false;
				playPgc(m_pgc->previousPgc);
				break;
			case Link::GoUpPgc:
				if (!m_pgc->goUpPgc) return false;
				playPgc(m_pgc->goUpPgc);
				break;
			case Link::TailPgc: m_jumpCount++; pgcStillFinished(); break;
			case Link::Resume: {
				if (!m_resume.valid) {
					return false;
				}
				m_domain = Domain::Title;
				m_titleSet = m_resume.titleSet;
				for (int i = 0; i < 5; i++) {
					m_sprm[4 + i] = m_resume.sprm[i];
				}
				const auto *table = pgcTable();
				if (!table || m_resume.pgc < 1 || m_resume.pgc > static_cast<int>(table->size())) {
					stop();
					break;
				}
				m_pgcNumber = m_resume.pgc;
				m_pgc = &(*table)[m_pgcNumber - 1].pgc;
				m_pgcStill = false;
				m_jumpCount++;
				playCell(m_resume.cell);
				break;
			}
			case Link::PgcNumber: playPgc(link.data1); break;
			case Link::Chapter:
				if (!setTitle(m_titleSet, m_sprm[5], link.data1)) return false;
				break;
			case Link::Program: playProgram(link.data1); m_jumpCount++; break;
			case Link::CellNumber: playCell(link.data1); m_jumpCount++; break;
			case Link::Exit: stop(); break;
			case Link::JumpTitle: {
				const auto &titles = m_disc.videoManager().titles;
				if (link.data1 < 1 || link.data1 > titles.size()
				    || !setTitle(titles[link.data1 - 1].titleSet, titles[link.data1 - 1].titleSetTitle, 1)) {
					std::cout << "DVD: no title " << link.data1 << std::endl;
					stop();
				}
				break;
			}
			case Link::JumpTitleSetTitle:
				if (!setTitle(m_titleSet, link.data1, 1)) stop();
				break;
			case Link::JumpChapter:
				if (!setTitle(m_titleSet, link.data1, link.data2)) stop();
				break;
			case Link::JumpFirstPlay:
				m_domain = Domain::FirstPlay;
				playPgc(0);
				break;
			case Link::CallFirstPlay:
				saveResume(link.data1);
				m_domain = Domain::FirstPlay;
				playPgc(0);
				break;
			case Link::CallVideoManagerMenu:
				saveResume(link.data2);
				[[fallthrough]];
			case Link::JumpVideoManagerMenu:
				m_domain = Domain::VideoManagerMenu;
				if (!setMenu(static_cast<MenuType>(link.data1))) {
					std::cout << "DVD: no video manager menu " << link.data1 << std::endl;
					stop();
				}
				break;
			case Link::CallTitleSetMenu:
				saveResume(link.data2);
				m_domain = Domain::TitleSetMenu;
				if (!setMenu(static_cast<MenuType>(link.data1))) {
					std::cout << "DVD: no title set menu " << link.data1 << std::endl;
					stop();
				}
				break;
			case Link::JumpTitleSetMenu:
				if (link.data1) {
					m_titleSet = link.data1;
				}
				m_sprm[5] = link.data2;
				m_domain = Domain::TitleSetMenu;
				if (!setMenu(static_cast<MenuType>(link.data3))) {
					std::cout << "DVD: no title set menu " << link.data3 << std::endl;
					stop();
				}
				break;
			case Link::CallVideoManagerPgc:
				saveResume(link.data2);
				[[fallthrough]];
			case Link::JumpVideoManagerPgc:
				m_domain = Domain::VideoManagerMenu;
				playPgc(link.data1);
				break;
		}
		return true;
	}

	// ---------------------------------------------------------------- user actions

	void Navigator::buttonActivated(const Command &command) {
		m_depth = 0;
		if (const auto link = run({command})) {
			follow(*link);
		}
	}

	void Navigator::menuCall(const MenuType menu) {
		m_depth = 0;
		if (m_domain == Domain::Stopped) {
			return;
		}
		saveResume(0);
		if (menu == TITLE_MENU || m_titleSet == 0) {
			m_domain = Domain::VideoManagerMenu;
			if (setMenu(TITLE_MENU)) {
				return;
			}
		}
		m_domain = Domain::TitleSetMenu;
		if (m_titleSet && setMenu(menu)) {
			return;
		}
		m_domain = Domain::VideoManagerMenu;
		if (!setMenu(TITLE_MENU)) {
			std::cout << "DVD: no menu" << std::endl;
			stop();
		}
	}

	void Navigator::nextChapter() {
		m_depth = 0;
		if (m_domain == Domain::Title && m_pgc) {
			follow({Link::NextProgram});
		}
	}

	void Navigator::previousChapter() {
		m_depth = 0;
		if (m_domain == Domain::Title && m_pgc) {
			follow({Link::PreviousProgram});
		}
	}

	void Navigator::skipProgramChain() {
		m_depth = 0;
		const Pgc *pgc = m_pgc;
		if (!pgc || m_domain == Domain::Stopped) {
			return;
		}
		const uint64_t jumpCount = m_jumpCount;
		// Bounded, for a program chain whose commands loop back into itself
		for (int i = 0; i < 1000 && m_pgc == pgc && m_domain != Domain::Stopped; i++) {
			if (m_pgcStill) {
				pgcStillFinished();
			} else if (m_cell) {
				afterCell();
			} else {
				break;
			}
		}
		if (m_jumpCount == jumpCount) {
			// Still in the same program chain: the cell being played is abandoned all the same
			m_jumpCount++;
		}
	}

	bool Navigator::inMenuEntryPgc() const {
		if ((m_domain != Domain::VideoManagerMenu && m_domain != Domain::TitleSetMenu) || m_pgcNumber < 1) {
			return false;
		}
		const auto *table = pgcTable();
		return table && m_pgcNumber <= static_cast<int>(table->size()) && (*table)[m_pgcNumber - 1].isEntry()
		       && (*table)[m_pgcNumber - 1].menuType() >= TITLE_MENU;
	}

	void Navigator::jumpToCell(const int cell) {
		m_depth = 0;
		if (m_pgc && m_domain != Domain::Stopped) {
			follow({Link::CellNumber, static_cast<uint16_t>(cell)});
		}
	}

	// ---------------------------------------------------------------- streams

	bool Navigator::wideScreen() const {
		if (m_domain == Domain::Title) {
			const Ifo *ifo = m_disc.titleSet(m_titleSet);
			return ifo && ifo->titleVideo.aspect == 3;
		}
		if (m_domain == Domain::TitleSetMenu) {
			const Ifo *ifo = m_disc.titleSet(m_titleSet);
			return ifo && ifo->menuVideo.aspect == 3;
		}
		return m_disc.videoManager().menuVideo.aspect == 3;
	}

	int Navigator::audioStreamId() const {
		if (!m_pgc) {
			return -1;
		}
		const Ifo *ifo = m_domain == Domain::Title || m_domain == Domain::TitleSetMenu ? m_disc.titleSet(m_titleSet) : &m_disc.videoManager();
		const auto &attributes = !ifo ? std::vector<AudioAttributes>{}
			                         : m_domain == Domain::Title ? ifo->titleAudio : ifo->menuAudio;
		int stream = m_domain == Domain::Title ? m_sprm[1] & 7 : 0;
		if (m_domain == Domain::Title && (m_sprm[1] > 7 || !(m_pgc->audioControl[stream] & 0x8000))) {
			// The first available
			stream = -1;
			for (int i = 0; i < 8; i++) {
				if (m_pgc->audioControl[i] & 0x8000) {
					stream = i;
					break;
				}
			}
			if (stream < 0) {
				return -1;
			}
		}
		const int id = m_pgc->audioControl[stream] & 0x8000 ? (m_pgc->audioControl[stream] >> 8) & 7 : stream;
		const int coding = stream < static_cast<int>(attributes.size()) ? attributes[stream].coding : 0;
		switch (coding) {
			case 2:
			case 3: return 0xC0 + id; // MPEG audio, a stream of its own
			case 4: return 0xA0 + id; // LPCM
			case 6: return 0x88 + id; // DTS
			default: return 0x80 + id; // A/52
		}
	}

	int Navigator::subpictureStreamId(const bool wide, bool &displayAll) const {
		if (!m_pgc) {
			return -1;
		}
		int stream = 0;
		displayAll = true;
		if (m_domain == Domain::Title) {
			stream = m_sprm[2] & 0x3F;
			displayAll = m_sprm[2] & 0x40;
			if (stream >= 32) {
				// None chosen: forced sub-pictures of the first stream still show
				stream = 0;
				displayAll = false;
			}
		}
		const uint32_t control = m_pgc->subpictureControl[stream];
		if (!(control & 0x80000000)) {
			return m_domain == Domain::Title ? -1 : 0x20;
		}
		return 0x20 + static_cast<int>(wide ? (control >> 16) & 0x1F : (control >> 24) & 0x1F);
	}

	const std::array<uint32_t, 16> *Navigator::palette() const {
		return m_pgc ? &m_pgc->palette : nullptr;
	}
}
