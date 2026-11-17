#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Disc.h"
#include "Navigator.h"
#include "../stream/MStream.h"

namespace Dvd {
	/**
	 * Reads a range of sectors of a VOB set: a menu .VOB, or a title set's title .VOB files, which together hold
	 * one sequence of sectors split into files of up to 1 GB.
	 */
	class VobReader final : public MStream::IReadByteStream {
		struct File {
			std::string path;
			uint64_t firstSector;
			uint64_t sectors;
		};

		std::vector<File> m_files;
		std::ifstream m_stream;
		int m_openFile = -1;
		uint64_t m_position = 0; // byte position in the set
		uint64_t m_end = 0; // byte after the range
		std::vector<uint8_t> m_buffer;
		size_t m_bufferIndex = 0;

		void fill() {
			m_buffer.clear();
			m_bufferIndex = 0;
			const uint64_t sector = m_position / SECTOR_SIZE;
			for (size_t i = 0; i < m_files.size(); i++) {
				const File &f = m_files[i];
				if (sector < f.firstSector || sector >= f.firstSector + f.sectors) {
					continue;
				}
				if (m_openFile != static_cast<int>(i)) {
					m_stream = std::ifstream(f.path, std::ios::binary);
					m_openFile = static_cast<int>(i);
				}
				const uint64_t offset = m_position - f.firstSector * SECTOR_SIZE;
				const uint64_t available = std::min<uint64_t>((f.firstSector + f.sectors) * SECTOR_SIZE, m_end) - m_position;
				m_buffer.resize(static_cast<size_t>(std::min<uint64_t>(available, 64 * 1024)));
				m_stream.clear();
				m_stream.seekg(static_cast<std::streamoff>(offset));
				m_stream.read(reinterpret_cast<char *>(m_buffer.data()), static_cast<std::streamsize>(m_buffer.size()));
				m_buffer.resize(static_cast<size_t>(m_stream.gcount()));
				return;
			}
		}

	public:
		/**
		 * Opens the VOB set a cell is in.
		 * @return false if its files are missing
		 */
		bool open(Disc &disc, const Domain domain, const int titleSet) {
			m_files.clear();
			m_openFile = -1;
			m_stream = std::ifstream();
			const auto add = [&](const std::string &name) {
				const std::string path = disc.file(name);
				if (path.empty()) {
					return false;
				}
				std::ifstream in(path, std::ios::binary | std::ios::ate);
				const uint64_t bytes = static_cast<uint64_t>(in.tellg());
				const uint64_t first = m_files.empty() ? 0 : m_files.back().firstSector + m_files.back().sectors;
				m_files.push_back({path, first, bytes / SECTOR_SIZE});
				return true;
			};
			char name[16];
			if (domain == Domain::Title) {
				for (int part = 1; part <= 9; part++) {
					std::snprintf(name, sizeof(name), "VTS_%02d_%d.VOB", titleSet, part);
					if (!add(name)) {
						break;
					}
				}
			} else if (titleSet == 0) {
				add("VIDEO_TS.VOB");
			} else {
				std::snprintf(name, sizeof(name), "VTS_%02d_0.VOB", titleSet);
				add(name);
			}
			return !m_files.empty();
		}

		// Reads sectors firstSector to lastSector inclusive
		void setRange(const uint32_t firstSector, const uint32_t lastSector) {
			m_position = static_cast<uint64_t>(firstSector) * SECTOR_SIZE;
			m_end = (static_cast<uint64_t>(lastSector) + 1) * SECTOR_SIZE;
			uint64_t total = 0;
			for (const auto &f: m_files) {
				total = std::max(total, (f.firstSector + f.sectors) * SECTOR_SIZE);
			}
			m_end = std::min(m_end, total);
			m_buffer.clear();
			m_bufferIndex = 0;
		}

		[[nodiscard]] uint64_t sector() const {
			return m_position / SECTOR_SIZE;
		}

		uint8_t nextByte() override {
			if (m_bufferIndex >= m_buffer.size()) {
				if (m_position >= m_end) {
					throw MStream::EndOfData();
				}
				fill();
				if (m_buffer.empty()) {
					m_position = m_end; // unreadable: treat as the end of the range
					throw MStream::EndOfData();
				}
			}
			m_position++;
			return m_buffer[m_bufferIndex++];
		}

		bool isEmpty() override {
			return m_bufferIndex >= m_buffer.size() && m_position >= m_end;
		}
	};
}
