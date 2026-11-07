// Plays video on an ST7789V, looping, from the embedded clip or from the first .MPG file in the root directory
// of a FAT32 microSD card if built with VIDEO_FROM_SD. Audio is decoded but not played.
//
// With a card, the screen shows why nothing is playing: red for a card or filesystem problem, blue for no .MPG.
//
// Core 0 demuxes and decodes. When a frame is due, core 1 converts it to RGB565 one line at a time and sends
// the lines by DMA, so the decoder keeps running and no frame buffer is needed. A displayed frame stays
// intact while the next one decodes, as it's the reference for it, unless it's a B-frame.
// Frames more than one period late are skipped. Stats go to USB serial if connected.
//
// Build with VIDEO_SCALED for 1.75x (280x224) instead of 1x (160x128), interpolating pixels and lines.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "DisplayTimestamper.h"
#include "St7789.h"
#include "mpeg1/Mpeg1AudioDecoder.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/MpegProgramStream.h"

#if VIDEO_FROM_SD
#include "Fat32.h"
#include "SdCard.h"
#else
extern "C" const uint8_t clip_start[];
extern "C" const uint8_t clip_end[];
#endif

namespace {
	constexpr uint32_t SOURCE_WIDTH = 160;
	constexpr uint32_t SOURCE_HEIGHT = 128;
#if VIDEO_SCALED
	constexpr uint32_t OUTPUT_WIDTH = 280;
	constexpr uint32_t OUTPUT_HEIGHT = 224;
#else
	constexpr uint32_t OUTPUT_WIDTH = SOURCE_WIDTH;
	constexpr uint32_t OUTPUT_HEIGHT = SOURCE_HEIGHT;
#endif
	constexpr int OUTPUT_X = (St7789::WIDTH - OUTPUT_WIDTH) / 2;
	constexpr int OUTPUT_Y = (St7789::HEIGHT - OUTPUT_HEIGHT) / 2;
	constexpr uint32_t REPORT_FRAMES = 125;

	// BT.601 limited range YCbCr to RGB565. The M0+ has few registers, so red, green and blue travel together
	// in one 32-bit value: red in bits 31-22, green in 21-11, blue in 10-0. Each field is its component plus
	// 512, which keeps it positive and in range for any input, so a pixel is one addition and a range check.
	constexpr int FIELD_BIAS = 512;
	constexpr int RED_SHIFT = 22;
	constexpr int GREEN_SHIFT = 11;
	// Set if a field isn't in 512..767, i.e. its component needs clamping
	constexpr uint32_t RANGE_MASK = 0xC0380700;
	constexpr uint32_t RANGE_EXPECTED = 0x80100200;

	constexpr uint32_t packFields(const int32_t red, const int32_t green, const int32_t blue) {
		return (static_cast<uint32_t>(red) << RED_SHIFT) + (static_cast<uint32_t>(green) << GREEN_SHIFT) + static_cast<uint32_t>(blue);
	}

	// In SRAM, for speed
	uint32_t s_lumaTable[256], s_crTable[256], s_cbTable[256];

	void initTables() {
		for (int i = 0; i < 256; i++) {
			const int32_t luma = ((i - 16) * 76309 + 32768) >> 16;
			const int32_t chroma = i - 128;
			s_lumaTable[i] = packFields(luma, luma, luma);
			s_crTable[i] = packFields(FIELD_BIAS + ((chroma * 104597 + 32768) >> 16), FIELD_BIAS - ((chroma * 53278 + 32768) >> 16), FIELD_BIAS);
			s_cbTable[i] = packFields(0, -((chroma * 25674 + 32768) >> 16), (chroma * 132201 + 32768) >> 16);
		}
	}

	// RGB565
	inline uint16_t packedToPixel(const uint32_t fields) {
		// Shifts instead of masks, which would need more registers
		const uint32_t red = (fields << 2) >> 27;
		const uint32_t green = (fields << 13) >> 26;
		const uint32_t blue = (fields << 24) >> 27;
		return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
	}

	__attribute__((noinline)) uint16_t __not_in_flash_func(clampedPixel)(const uint32_t fields) {
		const int32_t red = std::clamp<int32_t>(static_cast<int32_t>(fields >> RED_SHIFT) - FIELD_BIAS, 0, 255);
		const int32_t green = std::clamp<int32_t>(static_cast<int32_t>((fields >> GREEN_SHIFT) & 0x7FF) - FIELD_BIAS, 0, 255);
		const int32_t blue = std::clamp<int32_t>(static_cast<int32_t>(fields & 0x7FF) - FIELD_BIAS, 0, 255);
		return packedToPixel(packFields(FIELD_BIAS + red, FIELD_BIAS + green, FIELD_BIAS + blue));
	}

	inline uint16_t pixel(const uint8_t luma, const uint32_t chroma) {
		const uint32_t fields = s_lumaTable[luma] + chroma;
		if ((fields & RANGE_MASK) != RANGE_EXPECTED) {
			return clampedPixel(fields);
		}
		return packedToPixel(fields);
	}

	// Ordered dither, by row and column: 16-bit colour otherwise shows flat areas as visible bands.
	// Red and blue lose 3 bits, green 2.
	constexpr uint32_t ditherFields(const uint32_t level) {
		return packFields(level * 2, level, level * 2);
	}
	constexpr uint32_t DITHER[2][2] = {
		{ditherFields(0), ditherFields(2)},
		{ditherFields(3), ditherFields(1)}
	};

	// Averages two RGB565 pixels
	inline uint16_t average(const uint16_t a, const uint16_t b) {
		return static_cast<uint16_t>(((a & 0xF7DEu) + (b & 0xF7DEu)) >> 1);
	}

	// Converts a line to RGB565
	void __not_in_flash_func(convertLine)(const Frame &frame, const uint32_t row, uint16_t *out) {
		const uint8_t *luma = frame.m_yData + row * frame.m_frameSize.luma_width;
		const uint8_t *cr = frame.m_crData + (row / 2) * frame.m_frameSize.chroma_width;
		const uint8_t *cb = frame.m_cbData + (row / 2) * frame.m_frameSize.chroma_width;
		const uint32_t ditherEven = DITHER[row & 1][0], ditherOdd = DITHER[row & 1][1];
#if VIDEO_SCALED
		// 4 source pixels become 7, with interpolated pixels between them
		for (const uint16_t *end = out + OUTPUT_WIDTH; out != end; out += 7) {
			const uint32_t first = s_crTable[cr[0]] + s_cbTable[cb[0]];
			const uint32_t second = s_crTable[cr[1]] + s_cbTable[cb[1]];
			const uint16_t a = pixel(luma[0], first + ditherEven);
			const uint16_t b = pixel(luma[1], first + ditherOdd);
			const uint16_t c = pixel(luma[2], second + ditherEven);
			const uint16_t d = pixel(luma[3], second + ditherOdd);
			out[0] = a;
			out[1] = average(a, b);
			out[2] = b;
			out[3] = average(b, c);
			out[4] = c;
			out[5] = average(c, d);
			out[6] = d;
			luma += 4;
			cr += 2;
			cb += 2;
		}
#else
		for (const uint16_t *end = out + OUTPUT_WIDTH; out != end; out += 2) {
			const uint32_t chroma = s_crTable[*cr++] + s_cbTable[*cb++];
			out[0] = pixel(luma[0], chroma + ditherEven);
			out[1] = pixel(luma[1], chroma + ditherOdd);
			luma += 2;
		}
#endif
	}

	// The display wants big-endian pixels
	void __not_in_flash_func(toDisplayOrder)(const uint16_t *in, uint16_t *out) {
		for (uint32_t x = 0; x < OUTPUT_WIDTH; x++) {
			out[x] = __builtin_bswap16(in[x]);
		}
	}

	St7789 s_display;
	uint s_spiHz = 0;
	std::atomic<bool> s_scanBusy{false};

	// Written by core 1, read by core 0 as differences. A slightly stale value only affects a report.
	volatile uint32_t s_core1ConvertUs = 0;
	volatile uint32_t s_core1ScanUs = 0;
	volatile uint32_t s_core1MaxScanUs = 0;

	void __not_in_flash_func(scanFrame)(const Frame &frame) {
		const uint32_t scanStart = time_us_32();
		uint32_t convertUs = 0;
		// The last two source lines, and the output lines being sent or about to be. A line being sent can't
		// be changed, and sendPixels() returns once the line before the one it's given has gone.
		static uint16_t source[2][OUTPUT_WIDTH];
		static uint16_t output[3][OUTPUT_WIDTH];
		uint32_t nextOutput = 0;
		const auto send = [&](const uint16_t *line) {
			uint16_t *out = output[nextOutput];
			nextOutput = (nextOutput + 1) % 3;
			toDisplayOrder(line, out);
			s_display.sendPixels(reinterpret_cast<const uint8_t *>(out), OUTPUT_WIDTH * 2);
		};

		s_display.beginPixels(OUTPUT_X, OUTPUT_Y, OUTPUT_WIDTH, OUTPUT_HEIGHT);
		for (uint32_t row = 0; row < SOURCE_HEIGHT; row++) {
			uint16_t *line = source[row & 1];
			const uint32_t convertStart = time_us_32();
			convertLine(frame, row, line);
#if VIDEO_SCALED
			// 4 source lines become 7, with interpolated lines between them
			if (row & 3) {
				static uint16_t between[OUTPUT_WIDTH];
				const uint16_t *previous = source[(row & 1) ^ 1];
				for (uint32_t x = 0; x < OUTPUT_WIDTH; x++) {
					between[x] = average(previous[x], line[x]);
				}
				convertUs += time_us_32() - convertStart;
				send(between);
			} else {
				convertUs += time_us_32() - convertStart;
			}
#else
			convertUs += time_us_32() - convertStart;
#endif
			send(line);
		}
		s_display.endPixels();
		const uint32_t scanUs = time_us_32() - scanStart;
		s_core1ConvertUs = s_core1ConvertUs + convertUs;
		s_core1ScanUs = s_core1ScanUs + scanUs;
		if (scanUs > s_core1MaxScanUs) {
			s_core1MaxScanUs = scanUs;
		}
	}

	void __not_in_flash_func(core1Main)() {
		for (;;) {
			const auto *frame = reinterpret_cast<const Frame *>(multicore_fifo_pop_blocking());
			scanFrame(*frame);
			s_scanBusy.store(false, std::memory_order_release);
		}
	}

	// Bytes of input read, to notice playback stalling
	uint32_t s_inputBytes = 0;

#if VIDEO_FROM_SD
	// Counts the time reading blocks the decoder
	class TimedCard {
		SdCard &m_card;

	public:
		uint32_t totalUs = 0;
		uint32_t maxUs = 0;

		explicit TimedCard(SdCard &card) : m_card(card) {
		}

		bool readSector(const uint32_t lba, uint8_t *buffer) {
			const uint32_t start = time_us_32();
			const bool ok = m_card.readSector(lba, buffer);
			const uint32_t us = time_us_32() - start;
			totalUs += us;
			maxUs = std::max(maxUs, us);
			return ok;
		}
	};

	using Volume = Fat32<TimedCard>;

	SdCard s_card;
	TimedCard s_timedCard(s_card);
	Volume s_volume(s_timedCard);

	class CardFileStream final : public MStream::IReadByteStream {
		Volume::File m_file;

	public:
		explicit CardFileStream(const Volume::Entry &entry) : m_file(s_volume, entry) {
		}

		uint8_t nextByte() override {
			if (m_file.atEnd()) {
				throw MStream::EndOfData();
			}
			s_inputBytes++;
			return m_file.nextByte();
		}

		bool isEmpty() override {
			return m_file.atEnd();
		}
	};
#else
	class ClipStream final : public MStream::IReadByteStream {
		const uint8_t *m_position = clip_start;

	public:
		uint8_t nextByte() override {
			if (m_position == clip_end) {
				throw MStream::EndOfData();
			}
			s_inputBytes++;
			return *m_position++;
		}

		bool isEmpty() override {
			return m_position == clip_end;
		}
	};
#endif

	struct Stats {
		uint32_t shown = 0;
		uint32_t skipped = 0;
		uint64_t startUs = 0;
		uint64_t idleUs = 0;
		uint32_t maxLateUs = 0;
		uint32_t core1ConvertUs = 0;
		uint32_t core1ScanUs = 0;
#if VIDEO_FROM_SD
		uint32_t cardUs = 0;
#endif
	};

	Stats s_stats;

	void waitForScan() {
		const uint64_t start = time_us_64();
		while (s_scanBusy.load(std::memory_order_acquire)) {
			tight_loop_contents();
		}
		s_stats.idleUs += time_us_64() - start;
	}

	void report() {
		const uint64_t now = time_us_64();
		const auto elapsedUs = static_cast<double>(now - s_stats.startUs);
		const uint32_t convertUs = s_core1ConvertUs, scanUs = s_core1ScanUs;
		printf("SPI %u MHz, %lu shown, %lu skipped, core 0 %.0f%% busy, latest frame %lu ms, "
		       "core 1 convert %.0f%% (%.1f ms per frame), frame scan %.1f ms average, %.1f ms slowest\n",
		       s_spiHz / 1000000, s_stats.shown, s_stats.skipped,
		       100.0 - static_cast<double>(s_stats.idleUs) / elapsedUs * 100.0, s_stats.maxLateUs / 1000,
		       static_cast<double>(convertUs - s_stats.core1ConvertUs) / elapsedUs * 100.0,
		       static_cast<double>(convertUs - s_stats.core1ConvertUs) / 1000.0 / s_stats.shown,
		       static_cast<double>(scanUs - s_stats.core1ScanUs) / 1000.0 / s_stats.shown,
		       s_core1MaxScanUs / 1000.0);
#if VIDEO_FROM_SD
		printf("  card reads %.1f%% of core 0, slowest %.1f ms\n",
		       static_cast<double>(s_timedCard.totalUs - s_stats.cardUs) / elapsedUs * 100.0, s_timedCard.maxUs / 1000.0);
		const uint32_t cardUs = s_timedCard.totalUs;
		s_timedCard.maxUs = 0;
#endif
		s_stats = Stats{};
		s_stats.startUs = now;
#if VIDEO_FROM_SD
		s_stats.cardUs = cardUs;
#endif
		s_stats.core1ConvertUs = convertUs;
		s_stats.core1ScanUs = scanUs;
		s_core1MaxScanUs = 0;
	}

	void play(MStream::IReadByteStream &input) {
		auto demuxer = std::make_unique<Mpeg1::MpegProgramStream>();
		auto videoDecoder = std::make_unique<Mpeg1VideoDecoder>();
		auto audioDecoder = std::make_unique<Mpeg1AudioDecoder>();
		demuxer->setVideoDecoder(videoDecoder.get());
		demuxer->setAudioDecoder(audioDecoder.get());

		DisplayTimestamper timestamper;
		AudioSampleBuffer *soundBuffer = nullptr;
		bool timelineStarted = false;
		uint64_t timelineStartUs = 0;

		videoDecoder->setFrameCallback([&](const Frame &frame) {
			const int64_t ptsMs = timestamper.next(frame.m_ptsMillis, videoDecoder->m_frame_period_millis);
			if (ptsMs < 0 || videoDecoder->m_width != SOURCE_WIDTH || videoDecoder->m_height != SOURCE_HEIGHT) {
				return;
			}
			if (!timelineStarted) {
				timelineStarted = true;
				timelineStartUs = time_us_64() - static_cast<uint64_t>(ptsMs) * 1000;
			}
			const uint64_t dueUs = timelineStartUs + static_cast<uint64_t>(ptsMs) * 1000;
			const auto periodUs = static_cast<uint64_t>(videoDecoder->m_frame_period_millis * 1000);

			// The previous frame's data can be overwritten once this returns
			waitForScan();

			const uint64_t now = time_us_64();
			if (now > dueUs + periodUs) {
				s_stats.skipped++;
				return;
			}
			if (now < dueUs) {
				sleep_until(from_us_since_boot(dueUs));
				s_stats.idleUs += dueUs - now;
			} else {
				s_stats.maxLateUs = std::max(s_stats.maxLateUs, static_cast<uint32_t>(now - dueUs));
			}

			s_scanBusy.store(true, std::memory_order_release);
			multicore_fifo_push_blocking(reinterpret_cast<uint32_t>(&frame));
			// The next picture decodes over a B-frame, nothing keeps it
			if (videoDecoder->last_picture_coding_type == VideoPictureType_B) {
				waitForScan();
			}

			s_stats.shown++;
			if (s_stats.shown + s_stats.skipped >= REPORT_FRAMES) {
				report();
			}
		});
		audioDecoder->setFrameCallback([&](uint32_t, uint32_t, AudioSampleBuffer *buffer) {
			soundBuffer = buffer;
		});

		try {
			uint32_t lastInputBytes = s_inputBytes;
			uint64_t lastProgressUs = time_us_64();
			while (demuxer->nextPacket(input) != Mpeg1::DemuxResult::EndOfStream) {
				// No audio output yet, so discard the samples
				while (soundBuffer && !soundBuffer->isEmpty()) {
					soundBuffer->nextByte();
				}
				// A picture too big for the video ring stops all input, for good
				const uint64_t now = time_us_64();
				if (s_inputBytes != lastInputBytes) {
					lastInputBytes = s_inputBytes;
					lastProgressUs = now;
				} else if (now - lastProgressUs > 3000000) {
					printf("Playback stalled, a picture may be bigger than the %d byte video ring\n", MD_MPEG_VIDEO_RING_SIZE);
					break;
				}
			}
		} catch (const std::exception &ex) {
			printf("Decoder error: %s\n", ex.what());
		}
		waitForScan();

		// The decoder doesn't free these, and each loop makes a new decoder
		delete videoDecoder->m_ptrFramePast;
		delete videoDecoder->m_ptrFrameCurrent;
		delete videoDecoder->m_ptrFrameFuture;
	}
}

int main() {
	// Top of the datasheet's rated range, which needs 1.15 V
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	sleep_ms(10);
	set_sys_clock_khz(200000, true);
	// Setting the system clock moves the peripheral clock to the 48 MHz USB PLL, too slow for this SPI rate
	clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, 200 * MHZ, 200 * MHZ);
	stdio_init_all();
	// Gives a serial monitor a moment to attach, so the startup messages aren't lost
	for (int i = 0; i < 20 && !stdio_usb_connected(); i++) {
		sleep_ms(100);
	}

	initTables();
	s_spiHz = s_display.init(50000000);
	s_display.fillRect(0, 0, St7789::WIDTH, St7789::HEIGHT, 0x0000);
	multicore_launch_core1(core1Main);
	printf("video player, %lux%lu, sys %lu MHz, SPI %u Hz\n", OUTPUT_WIDTH, OUTPUT_HEIGHT, clock_get_hz(clk_sys) / 1000000, s_spiHz);

	s_stats.startUs = time_us_64();
#if VIDEO_FROM_SD
	constexpr uint16_t COLOR_BLACK = 0x0000, COLOR_RED = 0xF800, COLOR_BLUE = 0x001F;
	uint16_t screenColor = COLOR_BLACK;
	const auto showStatus = [&](const uint16_t color) {
		if (color != screenColor) {
			screenColor = color;
			s_display.fillRect(0, 0, St7789::WIDTH, St7789::HEIGHT, color);
		}
	};
	const auto fail = [&](const char *message, const uint16_t color) {
		printf("%s\n", message);
		showStatus(color);
		sleep_ms(1000);
	};

	for (;;) {
		try {
			if (const char *error = s_card.init()) {
				fail(error, COLOR_RED);
				continue;
			}
			const char *mountError = nullptr;
			if (!s_volume.mount(mountError)) {
				fail(mountError, COLOR_RED);
				continue;
			}

			Volume::Entry video{};
			bool found = false;
			s_volume.forEachFile([&](const Volume::Entry &entry) {
				printf("  %-12s %lu bytes\n", entry.name, entry.size);
				const char *extension = strrchr(entry.name, '.');
				if (!found && entry.size > 0 && extension && strcmp(extension, ".MPG") == 0) {
					video = entry;
					found = true;
				}
				return true;
			});
			if (!found) {
				fail("No .MPG file in the root directory", COLOR_BLUE);
				continue;
			}

			showStatus(COLOR_BLACK);
			printf("Playing %s\n", video.name);
			// On the heap, as the decoder's own stack use comes on top of this
			auto stream = std::make_unique<CardFileStream>(video);
			play(*stream);
		} catch (const std::exception &ex) {
			fail(ex.what(), COLOR_RED);
		}
	}
#else
	for (;;) {
		ClipStream clip;
		play(clip);
		printf("clip finished, looping\n");
	}
#endif
}
