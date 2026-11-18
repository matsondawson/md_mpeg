// Reads the first .MPG on the SD card the way the player does, a byte at a time, and times it.
// Built twice: sd_benchmark with the fat-fs library (CardVolume.h), sd_benchmark_old with SdCard.h and
// Fat32.h, so the two can be compared on the same card and file.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>

#include "Clocks.h"
#include "pico/stdlib.h"

#if SD_BENCHMARK_FAT_FS
#include "CardVolume.h"
#include "PicoSdCard.h"
#else
#include "Fat32.h"
#include "SdCard.h"
#endif

namespace {
	// Bytes read per run, or the whole file if it's smaller
	constexpr uint32_t READ_BYTES = 4 * 1024 * 1024;
	constexpr int RUNS = 3;

#if SD_BENCHMARK_FAT_FS
	using Volume = CardVolume;
	Mfs::PicoSdCard s_card(spi1, 10, 11, 12, 13);
#else
	using Volume = Fat32<SdCard>;
	SdCard s_card;
#endif
	Volume s_volume(s_card);

	const char *startCard() {
#if SD_BENCHMARK_FAT_FS
		if (s_card.begin() != 0) {
			return s_card.lastError();
		}
		return s_volume.mount();
#else
		if (const char *error = s_card.init()) {
			return error;
		}
		const char *error = nullptr;
		return s_volume.mount(error) ? nullptr : error;
#endif
	}

	void busyWait(const uint32_t us) {
		const uint32_t start = time_us_32();
		while (time_us_32() - start < us) {
		}
	}

	// Reads bytes of the file a byte at a time, with workUs of busy work after each 512 bytes, as the
	// decoder would do. @return microseconds spent reading, not counting the work
	uint32_t readFile(const Volume::Entry &entry, const uint32_t bytes, const uint32_t workUs, uint32_t &checksum) {
		Volume::File file(s_volume, entry);
		uint32_t readUs = 0;
		uint32_t sum = 0;
		uint32_t start = time_us_32();
		for (uint32_t i = 0; i < bytes; i++) {
			sum = sum * 31 + file.nextByte();
			if (workUs && (i & 511) == 511) {
				readUs += time_us_32() - start;
				busyWait(workUs);
				start = time_us_32();
			}
		}
		readUs += time_us_32() - start;
		checksum = sum;
		return readUs;
	}

	void run() {
		if (const char *error = startCard()) {
			printf("card: %s\n", error);
			return;
		}
		Volume::Entry video{};
		bool found = false;
		s_volume.forEachFile([&](const Volume::Entry &entry) {
			const char *extension = strrchr(entry.name, '.');
			if (entry.size > 0 && extension && strcmp(extension, ".MPG") == 0) {
				video = entry;
				found = true;
				return false;
			}
			return true;
		});
		if (!found) {
			printf("No .MPG file in the root directory\n");
			return;
		}
		const uint32_t bytes = std::min(READ_BYTES, video.size);
		printf("%s: %s, first %lu bytes, %d runs each\n", SD_BENCHMARK_FAT_FS ? "fat-fs" : "old", video.name, bytes,
		       RUNS);
		for (const uint32_t workUs : {0u, 250u}) {
			uint32_t best = UINT32_MAX, worst = 0, checksum = 0;
			for (int run = 0; run < RUNS; run++) {
				const uint32_t us = readFile(video, bytes, workUs, checksum);
				best = std::min(best, us);
				worst = std::max(worst, us);
			}
			printf("  %3lu us work per 512 bytes: reading took %lu-%lu ms, %lu KB/s best, checksum %08lx\n", workUs,
			       best / 1000, worst / 1000, static_cast<uint32_t>(static_cast<uint64_t>(bytes) * 1000000 / best / 1024),
			       checksum);
		}
	}
} // namespace

int main() {
	// The player's clocks, so the card's SPI runs at the same rate
	setupClocks(true);
	stdio_init_all();
	for (;;) {
		try {
			run();
		} catch (const std::exception &ex) {
			printf("%s\n", ex.what());
		}
		sleep_ms(3000);
	}
}
