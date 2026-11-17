#pragma once

#include <cstdint>

#include "Ifo.h"

namespace Dvd {
	/**
	 * Decides which program chains to skip from the start of a disc until its first menu: copyright and piracy
	 * warnings, studio logos and trailers.
	 *
	 * Each program chain is judged once, from the first navigation pack read from it. One without menu buttons
	 * that is shorter than MAX_INTRO_MS is skipped. A menu entry (root menu, title menu, ...) is never skipped,
	 * since its buttons may only appear after an animated opening. Skipping ends for good at the first program
	 * chain with buttons, or the first long one, so a disc that starts straight into its feature still plays it.
	 */
	class IntroSkipper {
	public:
		static constexpr uint32_t MAX_INTRO_MS = 3 * 60 * 1000;

		explicit IntroSkipper(const bool enabled) : m_active(enabled) {
		}

		// Still looking for the end of the introduction
		[[nodiscard]] bool active() const {
			return m_active;
		}

		/**
		 * @param pgc the program chain the navigation pack was read from
		 * @param menuEntry it is a menu's entry program chain
		 * @param hasButtons the navigation pack has menu buttons
		 * @return true to skip the program chain
		 */
		bool shouldSkip(const Pgc *pgc, const bool menuEntry, const bool hasButtons) {
			if (!m_active || !pgc || pgc == m_judged) {
				return false;
			}
			m_judged = pgc;
			if (hasButtons || pgc->playbackTimeMs >= MAX_INTRO_MS) {
				m_active = false;
				return false;
			}
			return !menuEntry;
		}

	private:
		bool m_active;
		const Pgc *m_judged = nullptr;
	};
}
