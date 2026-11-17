// Plays video on the display module chosen at build time (see DisplayModule.h), looping, from the embedded
// clip or from the .MPG files in the root directory of a FAT32 microSD card if built with VIDEO_FROM_SD.
// Audio is decoded but not played, on core 1 between frame scans if built with VIDEO_AUDIO_ON_CORE1,
// otherwise on core 0 with the video.
//
// With a card, the first .MPG plays, looping, and a button from GP15 to ground moves to the next one.
// The screen shows why nothing is playing: red for a card or filesystem problem, blue for no .MPG, and
// magenta for 2 s before moving on from a file that can't be played.
//
// Core 0 demuxes and decodes. When a frame is due, core 1 converts it to RGB565 one line at a time and sends
// the lines to the display, by DMA on SPI modules, so the decoder keeps running and no frame buffer is
// needed (monochrome OLEDs keep their own small one). A displayed frame stays intact while the next one
// decodes, as it's the reference for it, unless it's a B-frame.
// Frames more than one period late are skipped. Stats go to USB serial if connected.
//
// Pictures of any size are scaled to the screen's height, interpolating pixels and lines, and cropped at the
// left and right if they're then wider than the screen. A picture too big for memory moves to the next file.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "Clocks.h"
#include "DisplayTimestamper.h"
#include "DisplayModule.h"
#include "FrameScanner.h"
#include "mpeg1/Mpeg1AudioDecoder.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/MpegProgramStream.h"

#if VIDEO_FROM_SD
#include "Button.h"
#include "Fat32.h"
#include "SdCard.h"
#else
extern "C" const uint8_t clip_start[];
extern "C" const uint8_t clip_end[];
#endif

namespace {
	using board::SCREEN_WIDTH;
	using board::SCREEN_HEIGHT;
	constexpr uint32_t REPORT_FRAMES = 125;

	board::DisplayModule s_module;
	auto &s_display = s_module.display;
	std::atomic<bool> s_scanBusy{false};

	// Written by core 1, read by core 0 as differences. A slightly stale value only affects a report.
	volatile uint32_t s_core1ConvertUs = 0;
	volatile uint32_t s_core1ScanUs = 0;
	volatile uint32_t s_core1MaxScanUs = 0;

	void __not_in_flash_func(scanFrame)(const Frame &frame) {
		const uint32_t scanStart = time_us_32();
		uint32_t convertUs = 0;
		const uint32_t width = s_plan.outputWidth;
		// The output lines being sent or about to be. A line being sent can't be changed, and sendPixels()
		// returns once the line before the one it's given has gone.
		static FrameScanner scanner;
		static uint16_t output[3][SCREEN_WIDTH];
		uint32_t nextOutput = 0;

		scanner.begin(frame);
		s_display.beginPixels(static_cast<int>(s_plan.outputX), 0, static_cast<int>(width), SCREEN_HEIGHT);
		for (uint32_t y = 0; y < SCREEN_HEIGHT; y++) {
			const uint32_t convertStart = time_us_32();
			uint16_t *out = output[nextOutput];
			nextOutput = (nextOutput + 1) % 3;
			scanner.line(y, out);
			convertUs += time_us_32() - convertStart;
			s_display.sendPixels(reinterpret_cast<const uint8_t *>(out), width * 2);
		}
		s_display.endPixels();
		const uint32_t scanUs = time_us_32() - scanStart;
		s_core1ConvertUs = s_core1ConvertUs + convertUs;
		s_core1ScanUs = s_core1ScanUs + scanUs;
		if (scanUs > s_core1MaxScanUs) {
			s_core1MaxScanUs = scanUs;
		}
	}

#if VIDEO_AUDIO_ON_CORE1
	// Compressed audio from the demuxer on core 0 to the audio decoder on core 1, with the stream's PTS marks.
	// One writer, one reader. Positions count bytes since the last reset and are never wrapped.
	class AudioPipe final : public MStream::IWriteByteStream {
		// Holds the audio arriving during a frame scan, when core 1 can't take it, with plenty to spare.
		// A power of 2.
		static constexpr uint32_t SIZE = 4096;
		static constexpr uint32_t PTS_SIZE = 16;

		struct PtsMark {
			// Where in the bytes the PTS applies
			uint32_t position;
			int32_t ptsMillis;
		};

		uint8_t m_bytes[SIZE];
		PtsMark m_marks[PTS_SIZE];
		std::atomic<uint32_t> m_written{0}, m_read{0};
		std::atomic<uint32_t> m_marksWritten{0}, m_marksRead{0};

	public:
		// Core 0, only while core 1 isn't reading
		void reset() {
			m_written.store(0, std::memory_order_relaxed);
			m_read.store(0, std::memory_order_relaxed);
			m_marksWritten.store(0, std::memory_order_relaxed);
			m_marksRead.store(0, std::memory_order_release);
		}

		// IWriteByteStream, for the demuxer on core 0

		// Only the decoder knows its frames' times
		int32_t getPts(int32_t) override {
			return -1;
		}

		void setPts(const int32_t ptsMillis) override {
			const uint32_t marks = m_marksWritten.load(std::memory_order_relaxed);
			// A PTS comes with each audio packet, so the marks can only fill if the bytes are full too
			if (marks - m_marksRead.load(std::memory_order_acquire) < PTS_SIZE) {
				m_marks[marks % PTS_SIZE] = {m_written.load(std::memory_order_relaxed), ptsMillis};
				m_marksWritten.store(marks + 1, std::memory_order_release);
			}
		}

		void writeByte(const uint8_t data) override {
			const uint32_t written = m_written.load(std::memory_order_relaxed);
			m_bytes[written % SIZE] = data;
			m_written.store(written + 1, std::memory_order_release);
		}

		[[nodiscard]] bool isFull() override {
			return m_written.load(std::memory_order_relaxed) - m_read.load(std::memory_order_acquire) >= SIZE;
		}

		// Core 1 decodes
		void executeCallbacks() override {
		}

		void flush() override {
		}

		// Core 1

		[[nodiscard]] bool isEmptyForReader() const {
			return m_read.load(std::memory_order_relaxed) == m_written.load(std::memory_order_acquire);
		}

		void discard() {
			m_marksRead.store(m_marksWritten.load(std::memory_order_acquire), std::memory_order_release);
			m_read.store(m_written.load(std::memory_order_acquire), std::memory_order_release);
		}

		// Feeds the decoder, decoding at most one frame, so a frame scan never waits for more than one
		void feed(Mpeg1AudioDecoder &decoder) {
			uint32_t read = m_read.load(std::memory_order_relaxed);
			uint32_t marksRead = m_marksRead.load(std::memory_order_relaxed);
			const uint32_t written = m_written.load(std::memory_order_acquire);
			const uint32_t marksWritten = m_marksWritten.load(std::memory_order_acquire);
			bool decoded = false;
			while (read != written) {
				if (decoder.isFull()) {
					if (decoded) {
						return;
					}
					decoder.executeCallbacks();
					decoded = true;
					if (decoder.isFull()) {
						return;
					}
				}
				while (marksRead != marksWritten && m_marks[marksRead % PTS_SIZE].position == read) {
					decoder.setPts(m_marks[marksRead % PTS_SIZE].ptsMillis);
					marksRead++;
				}
				decoder.writeByte(m_bytes[read % SIZE]);
				read++;
				// Frees space as it goes, so core 0 isn't held up by a long run of decoding
				m_marksRead.store(marksRead, std::memory_order_release);
				m_read.store(read, std::memory_order_release);
			}
			if (!decoded) {
				decoder.executeCallbacks();
			}
		}
	};

	AudioPipe s_audioPipe;
	// The decoder core 1 feeds, null while there's none. Core 0 sets it, and waits for core 1 not to be busy
	// with it before the decoder goes.
	std::atomic<Mpeg1AudioDecoder *> s_core1AudioDecoder{nullptr};
	std::atomic<bool> s_core1AudioBusy{false};
	// Core 1 stops decoding, and discards the audio, after the decoder throws
	bool s_core1AudioFailed = false;
	volatile uint32_t s_core1AudioUs = 0;
	volatile bool s_core1AudioFailedReport = false;

	void __not_in_flash_func(core1Audio)() {
		if (s_audioPipe.isEmptyForReader()) {
			return;
		}
		// Sequentially consistent with core 0's detach, so either core 0 sees core 1 busy or core 1 sees null
		s_core1AudioBusy.store(true);
		if (Mpeg1AudioDecoder *decoder = s_core1AudioDecoder.load()) {
			if (s_core1AudioFailed) {
				s_audioPipe.discard();
			} else {
				const uint32_t start = time_us_32();
				try {
					s_audioPipe.feed(*decoder);
				} catch (const std::exception &) {
					s_core1AudioFailed = true;
					s_core1AudioFailedReport = true;
					s_audioPipe.discard();
				}
				s_core1AudioUs = s_core1AudioUs + (time_us_32() - start);
			}
		}
		s_core1AudioBusy.store(false);
	}

	// Hands a decoder to core 1 for as long as this lives
	class Core1Audio {
	public:
		explicit Core1Audio(Mpeg1AudioDecoder &decoder) {
			s_audioPipe.reset();
			s_core1AudioFailed = false;
			s_core1AudioDecoder.store(&decoder);
		}

		~Core1Audio() {
			s_core1AudioDecoder.store(nullptr);
			while (s_core1AudioBusy.load()) {
				tight_loop_contents();
			}
		}

		Core1Audio(const Core1Audio &) = delete;
		Core1Audio &operator=(const Core1Audio &) = delete;
	};
#endif

	void __not_in_flash_func(core1Main)() {
		for (;;) {
#if VIDEO_AUDIO_ON_CORE1
			// Audio between scans. A scan waits for any audio frame being decoded.
			if (!multicore_fifo_rvalid()) {
				core1Audio();
				continue;
			}
#endif
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

	constexpr uint PIN_NEXT_BUTTON = 15;
	Button s_nextButton(PIN_NEXT_BUTTON);

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
#if VIDEO_AUDIO_ON_CORE1
		uint32_t core1AudioUs = 0;
#endif
#if VIDEO_FROM_SD
		uint32_t cardUs = 0;
#endif
	};

	Stats s_stats;
	// Most of the video ring used since the file started, sampled between packets, so up to a packet low
	uint32_t s_videoRingPeak = 0;

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
		printf("display bus %.1f MHz, %lu shown, %lu skipped, core 0 %.0f%% busy, latest frame %lu ms, "
		       "core 1 convert %.0f%% (%.1f ms per frame), frame scan %.1f ms average, %.1f ms slowest\n",
		       s_module.bus.clockHz() / 1e6, s_stats.shown, s_stats.skipped,
		       100.0 - static_cast<double>(s_stats.idleUs) / elapsedUs * 100.0, s_stats.maxLateUs / 1000,
		       static_cast<double>(convertUs - s_stats.core1ConvertUs) / elapsedUs * 100.0,
		       static_cast<double>(convertUs - s_stats.core1ConvertUs) / 1000.0 / s_stats.shown,
		       static_cast<double>(scanUs - s_stats.core1ScanUs) / 1000.0 / s_stats.shown,
		       s_core1MaxScanUs / 1000.0);
#if VIDEO_AUDIO_ON_CORE1
		const uint32_t audioUs = s_core1AudioUs;
		printf("  audio %.1f%% of core 1%s\n", static_cast<double>(audioUs - s_stats.core1AudioUs) / elapsedUs * 100.0,
		       s_core1AudioFailedReport ? ", stopped after a decoder error" : "");
		s_core1AudioFailedReport = false;
#endif
#if VIDEO_FROM_SD
		printf("  card reads %.1f%% of core 0, slowest %.1f ms\n",
		       static_cast<double>(s_timedCard.totalUs - s_stats.cardUs) / elapsedUs * 100.0, s_timedCard.maxUs / 1000.0);
		const uint32_t cardUs = s_timedCard.totalUs;
		s_timedCard.maxUs = 0;
#endif
		printf("  video ring peak %lu of %d bytes\n", s_videoRingPeak, MD_MPEG_VIDEO_RING_SIZE);
		s_stats = Stats{};
		s_stats.startUs = now;
#if VIDEO_FROM_SD
		s_stats.cardUs = cardUs;
#endif
		s_stats.core1ConvertUs = convertUs;
		s_stats.core1ScanUs = scanUs;
#if VIDEO_AUDIO_ON_CORE1
		s_stats.core1AudioUs = audioUs;
#endif
		s_core1MaxScanUs = 0;
	}

	enum class PlayEnd {
		// At the end of the file, or stopped by an error reading it
		Finished,
		NextRequested,
		Unplayable
	};

	PlayEnd play(MStream::IReadByteStream &input) {
		PlayEnd result = PlayEnd::Finished;
		// The first picture sets the plan and clears the screen around it
		s_plan.sourceWidth = s_plan.sourceHeight = 0;
		s_videoRingPeak = 0;
		auto demuxer = std::make_unique<Mpeg1::MpegProgramStream>();
		auto videoDecoder = std::make_unique<Mpeg1VideoDecoder>();
		auto audioDecoder = std::make_unique<Mpeg1AudioDecoder>();
		demuxer->setVideoDecoder(videoDecoder.get());
		// No audio output yet, so the samples are discarded
		audioDecoder->setFrameCallback([](uint32_t, uint32_t, AudioSampleBuffer *buffer) {
			while (!buffer->isEmpty()) {
				buffer->nextByte();
			}
		});
#if VIDEO_AUDIO_ON_CORE1
		// Goes before the decoder does
		Core1Audio core1Audio(*audioDecoder);
		demuxer->setAudioDecoder(&s_audioPipe);
#else
		demuxer->setAudioDecoder(audioDecoder.get());
#endif

		DisplayTimestamper timestamper;
		bool timelineStarted = false;
		uint64_t timelineStartUs = 0;

		videoDecoder->setFrameCallback([&](const Frame &frame) {
			const int64_t ptsMs = timestamper.next(frame.m_ptsMillis, videoDecoder->m_frame_period_millis);
			// A reference picture never decoded, after an error, has no data
			if (ptsMs < 0 || !frame.m_yData) {
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

			// Core 1 is idle, so the plan and the screen can change
			if (frame.m_width != s_plan.sourceWidth || frame.m_height != s_plan.sourceHeight) {
				buildPlan(frame.m_width, frame.m_height);
				printf("  %.2f fps, %lu bit/s, %lu KB picture buffer declared\n", videoDecoder->m_frame_rate,
				       videoDecoder->bit_rate, videoDecoder->vbv_buffer_size);
				s_display.fill(0x0000);
			}

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
		try {
			uint32_t lastInputBytes = s_inputBytes;
			uint64_t lastProgressUs = time_us_64();
			while (demuxer->nextPacket(input) != Mpeg1::DemuxResult::EndOfStream) {
				s_videoRingPeak = std::max(s_videoRingPeak, videoDecoder->m_stream.used());
#if VIDEO_FROM_SD
				if (s_nextButton.pressed()) {
					result = PlayEnd::NextRequested;
					break;
				}
#endif
				// A picture too big for the video ring stops all input, for good
				const uint64_t now = time_us_64();
				if (s_inputBytes != lastInputBytes) {
					lastInputBytes = s_inputBytes;
					lastProgressUs = now;
				} else if (now - lastProgressUs > 3000000) {
					printf("Playback stalled, a picture may be bigger than the %d byte video ring\n", MD_MPEG_VIDEO_RING_SIZE);
					result = PlayEnd::Unplayable;
					break;
				}
			}
		} catch (const std::bad_alloc &) {
			printf("Not enough memory for %lux%lu pictures\n", videoDecoder->m_width, videoDecoder->m_height);
			result = PlayEnd::Unplayable;
		} catch (const Unplayable &ex) {
			printf("%s\n", ex.what());
			result = PlayEnd::Unplayable;
		} catch (const std::exception &ex) {
			printf("Decoder error: %s\n", ex.what());
		}
		// The decoder's frames are freed with it on return, so core 1 must have finished reading them
		waitForScan();
		return result;
	}
}

int main() {
	setupClocks(true);
	stdio_init_all();
	// Gives a serial monitor a moment to attach, so the startup messages aren't lost
	for (int i = 0; i < 20 && !stdio_usb_connected(); i++) {
		sleep_ms(100);
	}

	initTables();
	s_display.init();
	s_display.fill(0x0000);
#if VIDEO_AUDIO_ON_CORE1
	// The audio decoder needs more than the 4 KB core 1 gets by default, which has to fit in a 4 KB bank
	static uint32_t core1Stack[8192 / sizeof(uint32_t)];
	multicore_launch_core1_with_stack(core1Main, core1Stack, sizeof(core1Stack));
#else
	multicore_launch_core1(core1Main);
#endif
	printf("video player on %s, %lux%lu screen, sys %lu MHz, display bus %lu Hz\n", CHIP_NAME, SCREEN_WIDTH, SCREEN_HEIGHT, clock_get_hz(clk_sys) / 1000000, s_module.bus.clockHz());

	s_stats.startUs = time_us_64();
#if VIDEO_FROM_SD
	constexpr uint16_t COLOR_BLACK = 0x0000, COLOR_RED = 0xF800, COLOR_BLUE = 0x001F, COLOR_MAGENTA = 0xF81F;
	uint16_t screenColor = COLOR_BLACK;
	const auto showStatus = [&](const uint16_t color) {
		if (color != screenColor) {
			screenColor = color;
			s_display.fill(color);
		}
	};
	const auto fail = [&](const char *message, const uint16_t color) {
		printf("%s\n", message);
		showStatus(color);
		sleep_ms(1000);
	};

	s_nextButton.init();
	// Which .MPG plays, counting in directory order. Wraps to the first after the last.
	uint32_t fileIndex = 0;
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

			Volume::Entry first{}, video{};
			uint32_t videoCount = 0;
			s_volume.forEachFile([&](const Volume::Entry &entry) {
				printf("  %-12s %lu bytes\n", entry.name, entry.size);
				const char *extension = strrchr(entry.name, '.');
				if (entry.size > 0 && extension && strcmp(extension, ".MPG") == 0) {
					if (videoCount == 0) {
						first = entry;
					}
					if (videoCount == fileIndex) {
						video = entry;
					}
					videoCount++;
				}
				return true;
			});
			if (videoCount == 0) {
				fail("No .MPG file in the root directory", COLOR_BLUE);
				continue;
			}
			// Past the last one, or the card changed
			if (fileIndex >= videoCount) {
				fileIndex = 0;
				video = first;
			}

			showStatus(COLOR_BLACK);
			printf("Playing %s (%lu of %lu)\n", video.name, fileIndex + 1, videoCount);
			// On the heap, as the decoder's own stack use comes on top of this
			auto stream = std::make_unique<CardFileStream>(video);
			switch (play(*stream)) {
				case PlayEnd::Finished:
					break;
				case PlayEnd::NextRequested:
					fileIndex++;
					s_display.fill(COLOR_BLACK);
					break;
				case PlayEnd::Unplayable:
					fileIndex++;
					showStatus(COLOR_MAGENTA);
					sleep_ms(2000);
					break;
			}
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
