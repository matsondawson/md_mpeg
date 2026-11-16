#pragma once

// Read-only FAT32: files in the root directory, by their 8.3 names. Long names and subdirectories are ignored.
// Needs no Pico SDK, so it can be tested on a desktop against a disk image.
//
// Device needs: bool readSector(uint32_t lba, uint8_t *buffer), reading 512 bytes.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

template <class Device>
class Fat32 {
public:
	struct Entry {
		// "NAME.EXT"
		char name[13];
		uint32_t firstCluster;
		uint32_t size;
	};

private:
	static constexpr uint32_t SECTOR_SIZE = 512;
	static constexpr uint32_t DIRECTORY_ENTRY_SIZE = 32;
	static constexpr uint32_t END_OF_CHAIN = 0x0FFFFFF8;

	Device &m_device;
	uint32_t m_fatStart = 0;
	uint32_t m_dataStart = 0;
	uint32_t m_sectorsPerCluster = 0;
	uint32_t m_rootCluster = 0;
	uint32_t m_clusterCount = 0;

	// One sector of the FAT, as the next cluster is usually in the same one
	uint8_t m_fatSector[SECTOR_SIZE];
	uint32_t m_fatSectorLba = 0xFFFFFFFF;

	static uint16_t read16(const uint8_t *data) {
		return static_cast<uint16_t>(data[0] | data[1] << 8);
	}

	static uint32_t read32(const uint8_t *data) {
		return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8
		       | static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
	}

	void readSector(const uint32_t lba, uint8_t *buffer) {
		if (!m_device.readSector(lba, buffer)) {
			throw std::runtime_error("Card read failed");
		}
	}

	[[nodiscard]] bool validCluster(const uint32_t cluster) const {
		return cluster >= 2 && cluster < m_clusterCount + 2;
	}

	[[nodiscard]] uint32_t clusterLba(const uint32_t cluster) const {
		return m_dataStart + (cluster - 2) * m_sectorsPerCluster;
	}

	// @return the cluster after this one, or a value >= END_OF_CHAIN at the end of the chain
	uint32_t nextCluster(const uint32_t cluster) {
		const uint32_t lba = m_fatStart + cluster / (SECTOR_SIZE / 4);
		if (lba != m_fatSectorLba) {
			m_fatSectorLba = 0xFFFFFFFF;
			readSector(lba, m_fatSector);
			m_fatSectorLba = lba;
		}
		const uint32_t next = read32(m_fatSector + (cluster % (SECTOR_SIZE / 4)) * 4) & 0x0FFFFFFF;
		if (next < END_OF_CHAIN && !validCluster(next)) {
			throw std::runtime_error("Corrupt cluster chain");
		}
		return next;
	}

	static void entryName(const uint8_t *entry, char *name) {
		int length = 0;
		for (int i = 0; i < 8 && entry[i] != ' '; i++) {
			name[length++] = static_cast<char>(entry[i]);
		}
		if (entry[8] != ' ') {
			name[length++] = '.';
			for (int i = 8; i < 11 && entry[i] != ' '; i++) {
				name[length++] = static_cast<char>(entry[i]);
			}
		}
		name[length] = 0;
		// 0x05 stands for a real first byte of 0xE5, which marks deleted entries
		if (entry[0] == 0x05) {
			name[0] = static_cast<char>(0xE5);
		}
	}

public:
	explicit Fat32(Device &device) : m_device(device) {
	}

	/**
	 * Finds the volume, either in the first partition or filling the whole card.
	 * @return false, with why in error, if the card doesn't have a FAT32 volume
	 */
	bool mount(const char *&error) {
		m_fatSectorLba = 0xFFFFFFFF;
		uint8_t sector[SECTOR_SIZE];
		readSector(0, sector);
		if (sector[510] != 0x55 || sector[511] != 0xAA) {
			error = "No boot signature";
			return false;
		}

		const auto isFat32BootSector = [](const uint8_t *data) {
			return (data[0] == 0xEB || data[0] == 0xE9) && read16(data + 11) == SECTOR_SIZE
			       && read16(data + 17) == 0 && read16(data + 22) == 0 && read32(data + 36) != 0;
		};

		uint32_t volumeStart = 0;
		if (!isFat32BootSector(sector)) {
			volumeStart = read32(sector + 0x1BE + 8);
			if (sector[0x1BE + 4] == 0 || volumeStart == 0) {
				error = "No partition";
				return false;
			}
			readSector(volumeStart, sector);
			if (!isFat32BootSector(sector) || sector[510] != 0x55 || sector[511] != 0xAA) {
				error = "First partition is not FAT32 with 512 byte sectors";
				return false;
			}
		}

		m_sectorsPerCluster = sector[13];
		const uint32_t reservedSectors = read16(sector + 14);
		const uint32_t fatCount = sector[16];
		const uint32_t totalSectors = read32(sector + 32);
		const uint32_t fatSectors = read32(sector + 36);
		m_rootCluster = read32(sector + 44);

		if (m_sectorsPerCluster == 0 || (m_sectorsPerCluster & (m_sectorsPerCluster - 1)) != 0
		    || reservedSectors == 0 || fatCount == 0) {
			error = "Corrupt boot sector";
			return false;
		}

		m_fatStart = volumeStart + reservedSectors;
		const uint64_t metadataSectors = reservedSectors + static_cast<uint64_t>(fatCount) * fatSectors;
		if (totalSectors <= metadataSectors) {
			error = "Corrupt boot sector";
			return false;
		}
		m_dataStart = volumeStart + static_cast<uint32_t>(metadataSectors);
		m_clusterCount = (totalSectors - static_cast<uint32_t>(metadataSectors)) / m_sectorsPerCluster;

		// The FAT must be able to describe every cluster it claims to
		m_clusterCount = static_cast<uint32_t>(
			std::min<uint64_t>(m_clusterCount, static_cast<uint64_t>(fatSectors) * (SECTOR_SIZE / 4) - 2));

		if (!validCluster(m_rootCluster)) {
			error = "Corrupt boot sector";
			return false;
		}
		return true;
	}

	/**
	 * Calls callback(const Entry &) for each file in the root directory, in directory order.
	 * The callback returns false to stop.
	 */
	template <class Callback>
	void forEachFile(Callback callback) {
		uint8_t sector[SECTOR_SIZE];
		uint32_t cluster = m_rootCluster;
		// A chain longer than the volume has a loop
		for (uint32_t clusters = 0; cluster < END_OF_CHAIN && clusters <= m_clusterCount; clusters++) {
			const uint32_t lba = clusterLba(cluster);
			for (uint32_t s = 0; s < m_sectorsPerCluster; s++) {
				readSector(lba + s, sector);
				for (uint32_t offset = 0; offset < SECTOR_SIZE; offset += DIRECTORY_ENTRY_SIZE) {
					const uint8_t *entry = sector + offset;
					if (entry[0] == 0x00) {
						return; // No more entries
					}
					// Skips deleted entries, then long name parts, directories and the volume label
					if (entry[0] == 0xE5 || (entry[11] & 0x18) != 0) {
						continue;
					}
					Entry file{};
					entryName(entry, file.name);
					file.firstCluster = static_cast<uint32_t>(read16(entry + 20)) << 16 | read16(entry + 26);
					file.size = read32(entry + 28);
					if (!callback(static_cast<const Entry &>(file))) {
						return;
					}
				}
			}
			cluster = nextCluster(cluster);
		}
	}

	// A file being read in order
	class File {
		Fat32 &m_volume;
		uint32_t m_size;
		uint32_t m_position = 0;
		uint32_t m_cluster;
		uint32_t m_sectorInCluster = 0;
		uint8_t m_sector[SECTOR_SIZE];
		// Offset in m_sector, SECTOR_SIZE when the next sector is needed
		uint32_t m_offset = SECTOR_SIZE;

		void loadNextSector() {
			if (m_sectorInCluster == m_volume.m_sectorsPerCluster) {
				m_cluster = m_volume.nextCluster(m_cluster);
				m_sectorInCluster = 0;
			}
			if (!m_volume.validCluster(m_cluster)) {
				throw std::runtime_error("File is shorter than its size");
			}
			m_volume.readSector(m_volume.clusterLba(m_cluster) + m_sectorInCluster, m_sector);
			m_sectorInCluster++;
			m_offset = 0;
		}

	public:
		File(Fat32 &volume, const Entry &entry) : m_volume(volume), m_size(entry.size), m_cluster(entry.firstCluster) {
		}

		[[nodiscard]] uint32_t size() const { return m_size; }
		[[nodiscard]] uint32_t position() const { return m_position; }
		[[nodiscard]] bool atEnd() const { return m_position >= m_size; }

		// The next byte. Only call when not atEnd().
		uint8_t nextByte() {
			if (m_offset == SECTOR_SIZE) {
				loadNextSector();
			}
			m_position++;
			return m_sector[m_offset++];
		}
	};
};
