#pragma once

#include "../MException.h"
#include "MStream.h"

#include <algorithm>
#include <fstream>
#include <filesystem>

namespace MStream {
	class FileStreamRead final : public IReadByteStream {
		std::ifstream m_file;
		size_t m_length;
		size_t m_readIndex;

	public:
		bool m_endOfData;

		explicit FileStreamRead(const char* filepath): m_file(filepath, std::ios::in | std::ios::binary), m_endOfData(false), m_readIndex(0) {
			if (!m_file.is_open()) {
				cthrow(StringException, "Cannot open file for reading: "
					<< std::filesystem::current_path() << " " << filepath);
			}

			m_file.seekg(0, std::ifstream::end);
			m_length = m_file.tellg();
			m_file.seekg(0, std::ifstream::beg);
		}

		FileStreamRead(const FileStreamRead& cfileBuffer) = delete;

		[[nodiscard]] size_t size() const {
			return m_length;
		}

		// Moves the read position, clamped to the file
		void seek(const size_t position) {
			m_readIndex = std::min(position, m_length);
			m_endOfData = m_readIndex == m_length;
			m_file.clear();
			m_file.seekg(static_cast<std::streamoff>(m_readIndex), std::ifstream::beg);
		}

		bool isEmpty() override {
			return m_readIndex == m_length;
		}

		uint8_t nextByte() override {
			uint8_t temp;
			m_file.read(reinterpret_cast<char *>(&temp), 1);
			// std::cout << streamHex(2) << (uint32_t)temp << " ";
			if (m_file.gcount() != 1) {
				m_endOfData = true;
				throw EndOfData();
			}

			m_readIndex++;
			if (m_readIndex == m_length) {
				m_endOfData = true;
			}

			return temp;
		}
	};
}
