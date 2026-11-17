#pragma once

#include <map>
#include <memory>
#include <string>

#include "Ifo.h"

namespace Dvd {
	/**
	 * A DVD-Video folder (VIDEO_TS): its information files, read when first needed, and the paths of its .VOB
	 * files. File names may be upper or lower case.
	 */
	class Disc {
		std::string m_directory;
		Ifo m_videoManager;
		std::map<int, std::unique_ptr<Ifo>> m_titleSets;

	public:
		/**
		 * @param path The VIDEO_TS folder, the folder holding it, or a file in it
		 * @throws IfoException if there is no readable VIDEO_TS.IFO
		 */
		explicit Disc(const std::string &path);

		// True if path looks like a DVD-Video folder (or a file or parent folder of one)
		static bool isDvd(const std::string &path);

		[[nodiscard]] const Ifo &videoManager() const { return m_videoManager; }
		// Title set 1..titleSetCount, nullptr if it can't be read
		const Ifo *titleSet(int number);

		// Path of a file in the folder, matching its case, or "" if missing
		[[nodiscard]] std::string file(const std::string &name) const;
		[[nodiscard]] const std::string &directory() const { return m_directory; }
	};
}
