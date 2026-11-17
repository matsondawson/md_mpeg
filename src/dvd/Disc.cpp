#include "Disc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace Dvd {
	namespace {
		std::string upper(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return std::toupper(c); });
			return text;
		}

		// The VIDEO_TS folder for a path, or "" if there isn't one
		std::string findDirectory(const std::string &path) {
			std::error_code error;
			fs::path p(path);
			if (fs::is_regular_file(p, error)) {
				p = p.parent_path();
			}
			if (!fs::is_directory(p, error)) {
				return "";
			}
			const auto hasVideoManager = [](const fs::path &directory) {
				std::error_code e;
				for (const auto &entry: fs::directory_iterator(directory, e)) {
					if (upper(entry.path().filename().string()) == "VIDEO_TS.IFO") {
						return true;
					}
				}
				return false;
			};
			if (hasVideoManager(p)) {
				return p.string();
			}
			for (const auto &entry: fs::directory_iterator(p, error)) {
				if (entry.is_directory() && upper(entry.path().filename().string()) == "VIDEO_TS" && hasVideoManager(entry.path())) {
					return entry.path().string();
				}
			}
			return "";
		}
	}

	bool Disc::isDvd(const std::string &path) {
		std::error_code error;
		const fs::path p(path);
		// A .VOB on its own plays as a program stream
		if (fs::is_regular_file(p, error) && upper(p.extension().string()) != ".IFO") {
			return false;
		}
		return !findDirectory(path).empty();
	}

	Disc::Disc(const std::string &path) : m_directory(findDirectory(path)) {
		if (m_directory.empty()) {
			cthrow(IfoException, "No VIDEO_TS.IFO found at " << path);
		}
		m_videoManager = readIfo(file("VIDEO_TS.IFO"));
		if (!m_videoManager.videoManager) {
			cthrow(IfoException, "VIDEO_TS.IFO isn't a video manager");
		}
	}

	const Ifo *Disc::titleSet(const int number) {
		if (number < 1 || number > 99) {
			return nullptr;
		}
		auto &slot = m_titleSets[number];
		if (!slot) {
			char name[16];
			std::snprintf(name, sizeof(name), "VTS_%02d_0.IFO", number);
			try {
				auto ifo = std::make_unique<Ifo>(readIfo(file(name)));
				if (ifo->videoManager) {
					return nullptr;
				}
				slot = std::move(ifo);
			} catch (const std::exception &ex) {
				std::cout << "Can't read title set " << number << ": " << ex.what() << std::endl;
				return nullptr;
			}
		}
		return slot.get();
	}

	std::string Disc::file(const std::string &name) const {
		std::error_code error;
		const std::string wanted = upper(name);
		for (const auto &entry: fs::directory_iterator(m_directory, error)) {
			if (upper(entry.path().filename().string()) == wanted) {
				return entry.path().string();
			}
		}
		return "";
	}
}
