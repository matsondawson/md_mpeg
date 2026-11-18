// Desktop test of CardVolume.h against FAT images made by pico/make_fat32_images.sh (run by pico/pico.sh test).
//
// Usage: card_volume_test <directory holding the images and the original files>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "CardVolume.h"

namespace {
	int g_failures = 0;

	void check(const bool condition, const std::string &what) {
		if (!condition) {
			g_failures++;
			printf("  FAIL %s\n", what.c_str());
		}
	}

	// A disk image held in memory
	class ImageDevice final : public Mfs::SectorDevice {
		std::vector<uint8_t> m_data;

	public:
		explicit ImageDevice(const std::string &path) {
			std::ifstream file(path, std::ios::binary);
			m_data.assign(std::istreambuf_iterator<char>(file), {});
		}

		std::vector<uint8_t> &data() { return m_data; }

		Mfs::md_error_t read(uint8_t *buffer, const Mfs::sector_t sector) override {
			const uint64_t offset = static_cast<uint64_t>(sector) * 512;
			if (offset + 512 > m_data.size()) {
				return Mfs::SectorDevice_sectorOutOfRange;
			}
			std::copy_n(m_data.data() + offset, 512, buffer);
			return 0;
		}

		Mfs::md_error_t write(const uint8_t *, Mfs::sector_t) override {
			return Mfs::SectorDevice_notWritable;
		}

		uint32_t getBytesPerSector() const override { return 512; }
	};

	std::vector<uint8_t> readAll(const std::string &path) {
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), {}};
	}

	std::vector<CardVolume::Entry> listFiles(CardVolume &volume) {
		std::vector<CardVolume::Entry> files;
		volume.forEachFile([&](const CardVolume::Entry &entry) {
			files.push_back(entry);
			return true;
		});
		return files;
	}

	void checkContents(CardVolume &volume, const std::vector<CardVolume::Entry> &files, const std::string &name,
	                   const std::string &original) {
		const auto found = std::find_if(files.begin(), files.end(), [&](const auto &f) { return name == f.name; });
		check(found != files.end(), name + " is listed");
		if (found == files.end()) {
			return;
		}
		const auto expected = readAll(original);
		check(!expected.empty(), original + " was read");
		check(found->size == expected.size(), name + " has the right size");
		CardVolume::File file(volume, *found);
		std::vector<uint8_t> actual;
		while (!file.atEnd()) {
			actual.push_back(file.nextByte());
		}
		check(actual == expected, name + " has the right contents");
	}

	void testVolume(const std::string &directory, const std::string &image) {
		printf("%s\n", image.c_str());
		ImageDevice device(directory + "/" + image);
		CardVolume volume(device);
		const char *error = volume.mount();
		check(!error, std::string("mounts: ") + (error ? error : ""));
		if (error) {
			return;
		}
		const auto files = listFiles(volume);
		for (const auto &f : files) {
			printf("  %-12s %u bytes\n", f.name, f.size);
		}
		checkContents(volume, files, "SMALL.BIN", directory + "/small.bin");
		checkContents(volume, files, "ONESECT.BIN", directory + "/onesect.bin");
		checkContents(volume, files, "ONECLUS.BIN", directory + "/onecluster.bin");
		checkContents(volume, files, "FRAG.MPG", directory + "/frag.bin");
		checkContents(volume, files, "NOEXT", directory + "/small.bin");
		for (const auto &f : files) {
			const std::string name = f.name;
			check(name != "DELETED.BIN", "deleted file isn't listed");
			check(name != "SUBDIR", "directory isn't listed");
			check(name != "INSUB.BIN", "file in a directory isn't listed");
		}
		check(files.size() >= 150, "root directory spanning clusters is fully listed: " + std::to_string(files.size()));

		// Stops when the callback says to
		int visited = 0;
		volume.forEachFile([&](const CardVolume::Entry &) {
			visited++;
			return false;
		});
		check(visited == 1, "callback can stop the listing");
	}

	void testRejected(const std::string &directory, const std::string &image) {
		printf("%s\n", image.c_str());
		ImageDevice device(directory + "/" + image);
		CardVolume volume(device);
		const char *error = volume.mount();
		check(error != nullptr, "doesn't mount");
		printf("  rejected: %s\n", error ? error : "");
	}

	// Damages the FAT: a file whose chain ends early must raise an error, not return wrong data forever
	void testCorruptChain(const std::string &directory, const std::string &image) {
		printf("%s, corrupt chain\n", image.c_str());
		ImageDevice device(directory + "/" + image);

		// The file's first cluster, from its directory entry
		uint32_t firstCluster = 0;
		{
			Mfs::Fat fat(device);
			check(fat.mount() == 0, "mounts");
			Mfs::DirectoryListing listing(fat);
			check(listing.open("/") == 0 && listing.find("FRAG.MPG"), "FRAG.MPG is listed");
			const Mfs::DirectoryEntry &entry = listing.directoryEntry();
			firstCluster = static_cast<uint32_t>(entry.DIR_FstClusHI) << 16 | entry.DIR_FstClusLO;
		}
		check(firstCluster >= 2, "FRAG.MPG has clusters");
		if (firstCluster < 2) {
			return;
		}

		// Boot sector fields, for the file's FAT entry
		const uint8_t *boot = device.data().data();
		const uint32_t reserved = boot[14] | boot[15] << 8;
		const uint32_t entryOffset = reserved * 512 + firstCluster * 4;
		for (int i = 0; i < 4; i++) {
			device.data()[entryOffset + i] = 0; // 0 is a free cluster, never part of a chain
		}

		CardVolume damaged(device);
		const char *error = damaged.mount();
		check(!error, "still mounts");
		if (error) {
			return;
		}
		CardVolume::Entry frag{};
		damaged.forEachFile([&](const CardVolume::Entry &entry) {
			if (strcmp(entry.name, "FRAG.MPG") == 0) {
				frag = entry;
				return false;
			}
			return true;
		});
		CardVolume::File file(damaged, frag);
		bool threw = false;
		try {
			while (!file.atEnd()) {
				file.nextByte();
			}
		} catch (const std::runtime_error &ex) {
			threw = true;
			printf("  raised: %s\n", ex.what());
		}
		check(threw, "reading a broken chain raises an error");
	}
}

int main(const int argc, char **argv) {
	if (argc != 2) {
		printf("usage: card_volume_test <directory>\n");
		return 2;
	}
	const std::string directory = argv[1];
	testVolume(directory, "whole.img");
	testVolume(directory, "partitioned.img");
	testRejected(directory, "fat16.img");
	testRejected(directory, "blank.img");
	testCorruptChain(directory, "whole.img");

	if (g_failures) {
		printf("%d FAILED\n", g_failures);
		return 1;
	}
	printf("All passed\n");
	return 0;
}
