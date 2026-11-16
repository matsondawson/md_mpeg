// Desktop test of Fat32.h against FAT images made by pico/pico.sh test.
//
// Usage: fat32_test <directory holding the images and the original files>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Fat32.h"

namespace {
	int g_failures = 0;

	void check(const bool condition, const std::string &what) {
		if (!condition) {
			g_failures++;
			printf("  FAIL %s\n", what.c_str());
		}
	}

	class ImageDevice {
		std::vector<uint8_t> m_data;

	public:
		explicit ImageDevice(const std::string &path) {
			std::ifstream file(path, std::ios::binary);
			m_data.assign(std::istreambuf_iterator<char>(file), {});
		}

		std::vector<uint8_t> &data() { return m_data; }

		bool readSector(const uint32_t lba, uint8_t *buffer) {
			const uint64_t offset = static_cast<uint64_t>(lba) * 512;
			if (offset + 512 > m_data.size()) {
				return false;
			}
			std::copy_n(m_data.data() + offset, 512, buffer);
			return true;
		}
	};

	std::vector<uint8_t> readAll(const std::string &path) {
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), {}};
	}

	using Volume = Fat32<ImageDevice>;

	std::vector<Volume::Entry> listFiles(Volume &volume) {
		std::vector<Volume::Entry> files;
		volume.forEachFile([&](const Volume::Entry &entry) {
			files.push_back(entry);
			return true;
		});
		return files;
	}

	void checkContents(Volume &volume, const std::vector<Volume::Entry> &files, const std::string &name, const std::string &original) {
		const auto found = std::find_if(files.begin(), files.end(), [&](const auto &f) { return name == f.name; });
		check(found != files.end(), name + " is listed");
		if (found == files.end()) {
			return;
		}
		const auto expected = readAll(original);
		check(!expected.empty(), original + " was read");
		check(found->size == expected.size(), name + " has the right size");
		Volume::File file(volume, *found);
		std::vector<uint8_t> actual;
		while (!file.atEnd()) {
			actual.push_back(file.nextByte());
		}
		check(actual == expected, name + " has the right contents");
	}

	void testVolume(const std::string &directory, const std::string &image) {
		printf("%s\n", image.c_str());
		ImageDevice device(directory + "/" + image);
		Volume volume(device);
		const char *error = nullptr;
		check(volume.mount(error), std::string("mounts: ") + (error ? error : ""));
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
		check(files.size() >= 5, "root directory spanning clusters is fully listed: " + std::to_string(files.size()));

		// Stops when the callback says to
		int visited = 0;
		volume.forEachFile([&](const Volume::Entry &) {
			visited++;
			return false;
		});
		check(visited == 1, "callback can stop the listing");
	}

	void testRejected(const std::string &directory, const std::string &image) {
		printf("%s\n", image.c_str());
		ImageDevice device(directory + "/" + image);
		Volume volume(device);
		const char *error = nullptr;
		check(!volume.mount(error), "doesn't mount");
		printf("  rejected: %s\n", error ? error : "");
	}

	// Damages the FAT: a file whose chain ends early must raise an error, not return wrong data forever
	void testCorruptChain(const std::string &directory, const std::string &image) {
		printf("%s, corrupt chain\n", image.c_str());
		ImageDevice device(directory + "/" + image);
		Volume volume(device);
		const char *error = nullptr;
		check(volume.mount(error), "mounts");
		const auto files = listFiles(volume);
		const auto found = std::find_if(files.begin(), files.end(), [](const auto &f) { return std::string("FRAG.MPG") == f.name; });
		check(found != files.end(), "FRAG.MPG is listed");
		if (found == files.end()) {
			return;
		}

		// Boot sector fields, for the file's FAT entry
		const uint8_t *boot = device.data().data();
		const uint32_t reserved = boot[14] | boot[15] << 8;
		const uint32_t entryOffset = reserved * 512 + found->firstCluster * 4;
		for (int i = 0; i < 4; i++) {
			device.data()[entryOffset + i] = 0; // 0 is a free cluster, never part of a chain
		}

		Volume damaged(device);
		check(damaged.mount(error), "still mounts");
		Volume::File file(damaged, *found);
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
		printf("usage: fat32_test <directory>\n");
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
