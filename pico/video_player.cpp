// Plays the embedded clip's video on an ST7789V, looping. Audio is decoded but not played.
//
// Each frame is converted to RGB565 and sent by DMA when due, so decoding continues during the transfer.
// Frames more than one period late are skipped. Stats go to USB serial if connected.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "DisplayTimestamper.h"
#include "St7789.h"
#include "mpeg1/Mpeg1AudioDecoder.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/MpegProgramStream.h"

extern "C" const uint8_t clip_start[];
extern "C" const uint8_t clip_end[];

namespace {
	constexpr uint32_t VIDEO_WIDTH = 160;
	constexpr uint32_t VIDEO_HEIGHT = 128;
	constexpr int VIDEO_X = (St7789::WIDTH - VIDEO_WIDTH) / 2;
	constexpr int VIDEO_Y = (St7789::HEIGHT - VIDEO_HEIGHT) / 2;
	constexpr uint32_t REPORT_FRAMES = 125;

	// Big-endian RGB565, being sent while the next frame decodes
	uint8_t s_pixels[VIDEO_WIDTH * VIDEO_HEIGHT * 2];

	class ClipStream final : public MStream::IReadByteStream {
		const uint8_t *m_position = clip_start;

	public:
		uint8_t nextByte() override {
			if (m_position == clip_end) {
				throw MStream::EndOfData();
			}
			return *m_position++;
		}

		bool isEmpty() override {
			return m_position == clip_end;
		}
	};

	inline int32_t clampByte(const int32_t value) {
		return value < 0 ? 0 : value > 255 ? 255 : value;
	}

	inline void putPixel(uint8_t *out, const uint8_t luma, const int32_t r, const int32_t g, const int32_t b) {
		const int32_t y = (static_cast<int32_t>(luma) - 16) * 76309;
		const uint32_t red = clampByte((y + r) >> 16);
		const uint32_t green = clampByte((y - g) >> 16);
		const uint32_t blue = clampByte((y + b) >> 16);
		out[0] = (red & 0xF8) | (green >> 5);
		out[1] = ((green & 0x1C) << 3) | (blue >> 3);
	}

	// BT.601 limited range, the same conversion as Mpeg1VideoDecoder::generateRgbFrameData
	void __not_in_flash_func(convertFrame)(const Frame &frame) {
		const uint32_t lumaStride = frame.m_frameSize.luma_width;
		const uint32_t chromaStride = frame.m_frameSize.chroma_width;
		for (uint32_t row = 0; row < VIDEO_HEIGHT / 2; row++) {
			const uint8_t *cr = frame.m_crData + row * chromaStride;
			const uint8_t *cb = frame.m_cbData + row * chromaStride;
			const uint8_t *lumaTop = frame.m_yData + row * 2 * lumaStride;
			const uint8_t *lumaBottom = lumaTop + lumaStride;
			uint8_t *outTop = s_pixels + row * 2 * VIDEO_WIDTH * 2;
			uint8_t *outBottom = outTop + VIDEO_WIDTH * 2;
			for (uint32_t x = 0; x < VIDEO_WIDTH / 2; x++) {
				const int32_t crValue = static_cast<int32_t>(cr[x]) - 128;
				const int32_t cbValue = static_cast<int32_t>(cb[x]) - 128;
				const int32_t r = crValue * 104597;
				const int32_t g = cbValue * 25674 + crValue * 53278;
				const int32_t b = cbValue * 132201;
				putPixel(outTop + x * 4, lumaTop[x * 2], r, g, b);
				putPixel(outTop + x * 4 + 2, lumaTop[x * 2 + 1], r, g, b);
				putPixel(outBottom + x * 4, lumaBottom[x * 2], r, g, b);
				putPixel(outBottom + x * 4 + 2, lumaBottom[x * 2 + 1], r, g, b);
			}
		}
	}

	struct Stats {
		uint32_t shown = 0;
		uint32_t skipped = 0;
		uint64_t startUs = 0;
		uint64_t idleUs = 0;
		uint64_t convertUs = 0;
		uint32_t maxConvertUs = 0;
		uint64_t transferWaitUs = 0;
		uint32_t maxLateUs = 0;
	};

	St7789 s_display;
	Stats s_stats;
	bool s_transferActive = false;

	void waitForTransfer() {
		if (s_transferActive) {
			const uint64_t start = time_us_64();
			s_display.waitForTransfer();
			s_transferActive = false;
			s_stats.transferWaitUs += time_us_64() - start;
		}
	}

	void report() {
		const auto elapsedUs = static_cast<double>(time_us_64() - s_stats.startUs);
		printf("%lu shown, %lu skipped, CPU %.0f%% busy (convert %.0f%%, slowest %lu us, waiting on SPI %.0f%%), latest frame %lu ms\n",
		       s_stats.shown, s_stats.skipped,
		       100.0 - static_cast<double>(s_stats.idleUs) / elapsedUs * 100.0,
		       static_cast<double>(s_stats.convertUs) / elapsedUs * 100.0, s_stats.maxConvertUs,
		       static_cast<double>(s_stats.transferWaitUs) / elapsedUs * 100.0, s_stats.maxLateUs / 1000);
		s_stats = Stats{};
		s_stats.startUs = time_us_64();
	}

	void playOnce() {
		ClipStream clip;
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
			if (ptsMs < 0 || videoDecoder->m_width != VIDEO_WIDTH || videoDecoder->m_height != VIDEO_HEIGHT) {
				return;
			}
			if (!timelineStarted) {
				timelineStarted = true;
				timelineStartUs = time_us_64() - static_cast<uint64_t>(ptsMs) * 1000;
			}
			const uint64_t dueUs = timelineStartUs + static_cast<uint64_t>(ptsMs) * 1000;
			const auto periodUs = static_cast<uint64_t>(videoDecoder->m_frame_period_millis * 1000);

			if (time_us_64() > dueUs + periodUs) {
				s_stats.skipped++;
				return;
			}

			waitForTransfer();
			const uint64_t convertStart = time_us_64();
			convertFrame(frame);
			const auto convertUs = static_cast<uint32_t>(time_us_64() - convertStart);
			s_stats.convertUs += convertUs;
			s_stats.maxConvertUs = std::max(s_stats.maxConvertUs, convertUs);

			const uint64_t now = time_us_64();
			if (now < dueUs) {
				sleep_until(from_us_since_boot(dueUs));
				s_stats.idleUs += dueUs - now;
			} else {
				s_stats.maxLateUs = std::max(s_stats.maxLateUs, static_cast<uint32_t>(now - dueUs));
			}

			s_display.startTransfer(VIDEO_X, VIDEO_Y, VIDEO_WIDTH, VIDEO_HEIGHT, s_pixels);
			s_transferActive = true;
			s_stats.shown++;
			if (s_stats.shown + s_stats.skipped >= REPORT_FRAMES) {
				report();
			}
		});
		audioDecoder->setFrameCallback([&](uint32_t, uint32_t, AudioSampleBuffer *buffer) {
			soundBuffer = buffer;
		});

		try {
			while (demuxer->nextPacket(clip) != Mpeg1::DemuxResult::EndOfStream) {
				// No audio output yet, so discard the samples
				while (soundBuffer && !soundBuffer->isEmpty()) {
					soundBuffer->nextByte();
				}
			}
		} catch (const std::exception &ex) {
			printf("Decoder error: %s\n", ex.what());
		}

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
	stdio_init_all();

	const uint spiHz = s_display.init(40000000);
	s_display.fillRect(0, 0, St7789::WIDTH, St7789::HEIGHT, 0x0000);
	printf("video player, sys %lu MHz, SPI %u Hz\n", clock_get_hz(clk_sys) / 1000000, spiHz);

	s_stats.startUs = time_us_64();
	for (;;) {
		playOnce();
		printf("clip finished, looping\n");
	}
}
