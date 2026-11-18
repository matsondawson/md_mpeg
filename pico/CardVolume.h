#pragma once

// The SD card's FAT volume, read through the fat-fs library (https://github.com/matsondawson/fat-fs):
// files in the root directory, by their 8.3 names, read in order a byte at a time.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>

#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Partition.h"

class CardVolume {
public:
	struct Entry {
		// "NAME.EXT"
		char name[13];
		uint32_t size;
	};

private:
	static constexpr uint32_t SECTOR_SIZE = 512;

	Mfs::SectorDevice &m_device;
	std::optional<Mfs::Partition> m_partition;
	std::optional<Mfs::Fat> m_fat;

	[[noreturn]] static void fail(const char *what, const Mfs::md_error_t error) {
		char message[64];
		snprintf(message, sizeof(message), "%s (fat-fs error %lu)", what, static_cast<unsigned long>(error));
		throw std::runtime_error(message);
	}

	// Mounts the volume on the partition opened as partition
	Mfs::md_error_t mountOn(const uint32_t partition) {
		m_fat.reset();
		m_partition.emplace(m_device);
		if (const Mfs::md_error_t error = m_partition->open(partition)) {
			return error;
		}
		m_fat.emplace(*m_partition);
		const Mfs::md_error_t error = m_fat->mount();
		if (error) {
			m_fat.reset();
		}
		return error;
	}

public:
	explicit CardVolume(Mfs::SectorDevice &device) : m_device(device) {
	}

	/**
	 * Finds the volume, either filling the whole card or in the first partition.
	 * @return nullptr, or why the card doesn't have a FAT32 volume
	 */
	const char *mount() {
		if (mountOn(Mfs::Partition::WHOLE_DEVICE) == 0 || mountOn(Mfs::Partition::primary(0)) == 0) {
			return nullptr;
		}
		return "No FAT32 volume on the card, or in its first partition";
	}

	/**
	 * Calls callback(const Entry &) for each file in the root directory, in directory order.
	 * The callback returns false to stop.
	 */
	template <class Callback>
	void forEachFile(Callback callback) {
		Mfs::DirectoryListing listing(*m_fat);
		if (const Mfs::md_error_t error = listing.open("/")) {
			fail("Can't read the root directory", error);
		}
		while (listing.next()) {
			const Mfs::DirectoryEntry &entry = listing.directoryEntry();
			if (entry.DIR_Attr & Mfs::ATTR_DIRECTORY) {
				continue;
			}
			Entry file{};
			// Upper case, ignoring the lower case flags Windows sets
			Mfs::formatShortName(entry.DIR_Name, 0, file.name, sizeof(file.name));
			file.size = entry.DIR_FileSize;
			if (!callback(static_cast<const Entry &>(file))) {
				return;
			}
		}
		if (const Mfs::md_error_t error = listing.error()) {
			fail("Can't read the root directory", error);
		}
	}

	// A file being read in order
	class File {
		Mfs::File m_file;
		uint32_t m_size;
		uint32_t m_position = 0;
		uint8_t m_buffer[SECTOR_SIZE];
		uint32_t m_offset = 0;
		uint32_t m_length = 0;

		void refill() {
			// A whole sector at a time, which fat-fs reads straight into the buffer
			if (const Mfs::md_error_t error = m_file.read(m_buffer, SECTOR_SIZE, m_length)) {
				fail("Card read failed", error);
			}
			if (m_length == 0) {
				throw std::runtime_error("File is shorter than its size");
			}
			m_offset = 0;
		}

	public:
		File(CardVolume &volume, const Entry &entry) : m_file(*volume.m_fat) {
			if (const Mfs::md_error_t error = m_file.open("/", entry.name)) {
				fail("Can't open the file", error);
			}
			m_size = m_file.length();
		}

		[[nodiscard]] uint32_t size() const { return m_size; }
		[[nodiscard]] uint32_t position() const { return m_position; }
		[[nodiscard]] bool atEnd() const { return m_position >= m_size; }

		// The next byte. Only call when not atEnd().
		uint8_t nextByte() {
			if (m_offset == m_length) {
				refill();
			}
			m_position++;
			return m_buffer[m_offset++];
		}
	};
};
