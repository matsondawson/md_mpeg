// Decode-only benchmark for the RP2040: no display and no audio output.
//
// Decodes a clip embedded in flash as fast as possible, timing the video and audio decoders and measuring
// memory. Then decodes it again hashing the output, to compare against a desktop decode of the same clip.
// Results go to USB serial and repeat, so a late-connecting reader still gets them.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <malloc.h>

#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "mpeg1/Mpeg1AudioDecoder.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/MpegProgramStream.h"

extern "C" const uint8_t clip_start[];
extern "C" const uint8_t clip_end[];
extern "C" char __StackBottom;

// Progress, printed by a timer so a stuck decoder still reports where it is
static volatile uint32_t g_calls, g_frames, g_samples;
static volatile const uint8_t *g_clipPosition;

static constexpr uint32_t FAULT_MAGIC = 0xFA017ED0;

// Hard faults stop USB, so note where it happened and reboot; main() reports it
extern "C" __attribute__((used)) void hardFaultReport(const uint32_t *frame) {
	watchdog_hw->scratch[0] = FAULT_MAGIC;
	watchdog_hw->scratch[1] = frame[6]; // PC
	watchdog_hw->scratch[2] = frame[5]; // LR
	watchdog_hw->scratch[3] = reinterpret_cast<uint32_t>(frame);
	watchdog_reboot(0, 0, 0);
	for (;;) {
	}
}

extern "C" __attribute__((naked)) void isr_hardfault() {
	__asm volatile("mrs r0, msp\n bl hardFaultReport\n");
}

namespace {
	constexpr uint32_t STACK_PATTERN = 0xA5A5A5A5;

	inline uint32_t micros() {
		return timer_hw->timerawl;
	}

	class ClipStream final : public MStream::IReadByteStream {
		const uint8_t *m_position = clip_start;

	public:
		uint8_t nextByte() override {
			if (m_position == clip_end) {
				throw MStream::EndOfData();
			}
			g_clipPosition = m_position;
			return *m_position++;
		}

		bool isEmpty() override {
			return m_position == clip_end;
		}
	};

	struct Timing {
		uint64_t totalUs = 0;
		uint32_t maxUs = 0;
		uint32_t calls = 0;

		void add(const uint32_t us) {
			totalUs += us;
			calls++;
			if (us > maxUs) {
				maxUs = us;
			}
		}
	};

	// Video decoding runs inside the writeByte() that completes a picture start code
	class VideoProbe final : public MStream::IWriteByteStream {
		Mpeg1VideoDecoder &m_decoder;
		uint32_t m_recent = ~0u;
		int64_t m_behindUs = 0;

	public:
		Timing timing;
		// Decode calls longer than one frame period
		uint32_t overFrame = 0;
		// Furthest behind the frame rate if video had a core to itself
		uint32_t maxBehindUs = 0;

		explicit VideoProbe(Mpeg1VideoDecoder &decoder) : m_decoder(decoder) {
		}

		int32_t getPts(const int32_t t) override { return m_decoder.getPts(t); }
		void setPts(const int32_t pts) override { m_decoder.setPts(pts); }
		bool isFull() override { return m_decoder.isFull(); }
		void executeCallbacks() override { m_decoder.executeCallbacks(); }

		void writeByte(const uint8_t data) override {
			m_recent = (m_recent << 8) | data;
			if (m_recent != PICTURE_START_CODE) {
				m_decoder.writeByte(data);
				return;
			}
			const auto start = micros();
			m_decoder.writeByte(data);
			const auto us = micros() - start;
			timing.add(us);
			const auto periodUs = static_cast<int64_t>(m_decoder.m_frame_period_millis * 1000);
			if (us > periodUs) {
				overFrame++;
			}
			m_behindUs = std::max<int64_t>(0, m_behindUs + us - periodUs);
			maxBehindUs = std::max(maxBehindUs, static_cast<uint32_t>(m_behindUs));
		}

		void flush() override {
			const auto start = micros();
			m_decoder.flush();
			timing.add(micros() - start);
		}
	};

	// Audio decoding runs inside executeCallbacks()
	class AudioProbe final : public MStream::IWriteByteStream {
		Mpeg1AudioDecoder &m_decoder;

	public:
		Timing timing;

		explicit AudioProbe(Mpeg1AudioDecoder &decoder) : m_decoder(decoder) {
		}

		int32_t getPts(const int32_t t) override { return m_decoder.getPts(t); }
		void setPts(const int32_t pts) override { m_decoder.setPts(pts); }
		void writeByte(const uint8_t data) override { m_decoder.writeByte(data); }
		bool isFull() override { return m_decoder.isFull(); }
		void flush() override { m_decoder.flush(); }

		void executeCallbacks() override {
			const auto start = micros();
			m_decoder.executeCallbacks();
			timing.add(micros() - start);
		}
	};

	uint64_t fnv(uint64_t hash, const void *data, size_t size) {
		const auto *bytes = static_cast<const uint8_t *>(data);
		while (size--) {
			hash ^= *bytes++;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	struct Result {
		uint32_t sysClockKhz = 0;
		uint64_t wallUs = 0;
		Timing video, audio;
		uint32_t frames = 0;
		uint64_t samples = 0;
		uint32_t sampleRate = 0;
		uint32_t channels = 0;
		float framePeriodMs = 0;
		uint32_t videoOverFrame = 0;
		uint32_t videoCoreBehindUs = 0;
		// Furthest behind the frame rate with everything on one core
		uint32_t singleCoreBehindUs = 0;
		uint64_t videoHash = 14695981039346656037ULL;
		uint64_t audioHash = 14695981039346656037ULL;
		uint32_t heapArena = 0;
		uint32_t heapUsed = 0;
		const char *error = "";
	};

	__attribute__((noinline)) void run(Result &result, const bool hash) {
		ClipStream clip;
		auto demuxer = std::make_unique<Mpeg1::MpegProgramStream>();
		auto videoDecoder = std::make_unique<Mpeg1VideoDecoder>();
		auto audioDecoder = std::make_unique<Mpeg1AudioDecoder>();
		VideoProbe videoProbe(*videoDecoder);
		AudioProbe audioProbe(*audioDecoder);
		demuxer->setVideoDecoder(&videoProbe);
		demuxer->setAudioDecoder(&audioProbe);

		AudioSampleBuffer *soundBuffer = nullptr;
		std::vector<uint32_t> rgb;

		int64_t singleCoreBehind = 0;
		uint32_t lastFrameWall = 0;
		bool haveFrame = false;

		videoDecoder->setFrameCallback([&](const Frame &frame) {
			result.frames++;
			g_frames = result.frames;
			if (hash) {
				rgb.resize(videoDecoder->m_width * videoDecoder->m_height);
				videoDecoder->generateRgbFrameData(rgb.data(), frame);
				result.videoHash = fnv(result.videoHash, rgb.data(), rgb.size() * sizeof(uint32_t));
				return;
			}
			const auto wall = micros();
			if (haveFrame) {
				const auto periodUs = static_cast<int64_t>(videoDecoder->m_frame_period_millis * 1000);
				singleCoreBehind = std::max<int64_t>(0, singleCoreBehind + static_cast<int64_t>(wall - lastFrameWall) - periodUs);
				result.singleCoreBehindUs = std::max(result.singleCoreBehindUs, static_cast<uint32_t>(singleCoreBehind));
			}
			haveFrame = true;
			lastFrameWall = wall;
		});
		audioDecoder->setFrameCallback([&](const uint32_t rate, const uint32_t channels, AudioSampleBuffer *buffer) {
			result.sampleRate = rate;
			result.channels = channels;
			soundBuffer = buffer;
		});

		const auto start = micros();
		try {
			while (demuxer->nextPacket(clip) != Mpeg1::DemuxResult::EndOfStream) {
				g_calls = g_calls + 1;
				while (soundBuffer && !soundBuffer->isEmpty()) {
					const AudioSample sample = soundBuffer->nextByte();
					result.samples++;
					if (hash) {
						result.audioHash = fnv(result.audioHash, &sample, sizeof(sample));
					}
				}
				g_samples = result.samples;
			}
			// Frames that were waiting for space in the sound buffer
			for (int i = 0; i < 8; i++) {
				audioProbe.executeCallbacks();
				while (soundBuffer && !soundBuffer->isEmpty()) {
					const AudioSample sample = soundBuffer->nextByte();
					result.samples++;
					if (hash) {
						result.audioHash = fnv(result.audioHash, &sample, sizeof(sample));
					}
				}
			}
		} catch (const std::exception &) {
			result.error = "decoder threw";
		}
		result.wallUs = micros() - start;

		result.video = videoProbe.timing;
		result.videoOverFrame = videoProbe.overFrame;
		result.videoCoreBehindUs = videoProbe.maxBehindUs;
		result.audio = audioProbe.timing;
		result.framePeriodMs = videoDecoder->m_frame_period_millis;

		const auto info = mallinfo();
		result.heapArena = info.arena;
		result.heapUsed = info.uordblks;

		// The decoder doesn't free these, and each run makes a new decoder
		delete videoDecoder->m_ptrFramePast;
		delete videoDecoder->m_ptrFrameCurrent;
		delete videoDecoder->m_ptrFrameFuture;
	}

	// Not inlined: everything from here down is unused, but the caller's frame must not be painted
	__attribute__((noinline)) void paintStack() {
		const auto *limit = static_cast<const char *>(__builtin_frame_address(0)) - 64;
		for (auto *p = reinterpret_cast<uint32_t *>(&__StackBottom); reinterpret_cast<const char *>(p) < limit; p++) {
			*p = STACK_PATTERN;
		}
	}

	uint32_t stackUsed() {
		auto *p = reinterpret_cast<const uint32_t *>(&__StackBottom);
		while (*p == STACK_PATTERN) {
			p++;
		}
		return PICO_STACK_SIZE - (reinterpret_cast<const char *>(p) - &__StackBottom);
	}

	void printTiming(const Result &r) {
		const double mediaSeconds = static_cast<double>(r.samples) / r.channels / r.sampleRate;
		const double seconds = static_cast<double>(r.wallUs) / 1e6;
		const double demuxUs = static_cast<double>(r.wallUs) - static_cast<double>(r.video.totalUs) - static_cast<double>(r.audio.totalUs);
		printf("clock %lu MHz: %.2f s to decode %.2f s of media, %.0f%% of real time on one core%s%s\n",
		       r.sysClockKhz / 1000, seconds, mediaSeconds, seconds / mediaSeconds * 100, *r.error ? ", " : "", r.error);
		printf("  video %.2f s (%.0f%% of a core), slowest %lu ms against a %.0f ms frame period, %lu of %lu calls over it\n",
		       static_cast<double>(r.video.totalUs) / 1e6, static_cast<double>(r.video.totalUs) / 1e6 / mediaSeconds * 100,
		       r.video.maxUs / 1000, r.framePeriodMs, r.videoOverFrame, r.video.calls);
		printf("  audio %.2f s (%.0f%% of a core), slowest %lu ms\n",
		       static_cast<double>(r.audio.totalUs) / 1e6, static_cast<double>(r.audio.totalUs) / 1e6 / mediaSeconds * 100,
		       r.audio.maxUs / 1000);
		printf("  demux and overhead %.2f s (%.0f%% of a core)\n", demuxUs / 1e6, demuxUs / 1e6 / mediaSeconds * 100);
		printf("  furthest behind frame rate: %lu ms with video on its own core, %lu ms with everything on one core\n",
		       r.videoCoreBehindUs / 1000, r.singleCoreBehindUs / 1000);
	}
}

static bool heartbeat(repeating_timer_t *) {
	printf("  progress: %lu calls, offset %ld of %ld, %lu frames, %lu samples\n", g_calls,
	       static_cast<long>(const_cast<const uint8_t *>(g_clipPosition) - clip_start), static_cast<long>(clip_end - clip_start),
	       g_frames, g_samples);
	return true;
}

int main() {
	const uint32_t bootHeapUsed = mallinfo().uordblks;
	const bool faulted = watchdog_hw->scratch[0] == FAULT_MAGIC;
	const uint32_t faultPc = watchdog_hw->scratch[1], faultLr = watchdog_hw->scratch[2], faultSp = watchdog_hw->scratch[3];
	watchdog_hw->scratch[0] = 0;
	stdio_init_all();

	while (!stdio_usb_connected()) {
		sleep_ms(50);
	}
	sleep_ms(500);
	while (faulted) {
		printf("HARD FAULT pc=%08lx lr=%08lx sp=%08lx\n==== end ====\n", faultPc, faultLr, faultSp);
		sleep_ms(1000);
	}
	static repeating_timer_t heartbeatTimer;
	add_repeating_timer_ms(5000, heartbeat, nullptr, &heartbeatTimer);
	printf("\nmd_mpeg RP2040 benchmark, clip %u bytes\n", static_cast<unsigned>(clip_end - clip_start));

	// Default clock
	Result at125;
	at125.sysClockKhz = clock_get_hz(clk_sys) / 1000;
	printf("Decoding at %lu MHz...\n", at125.sysClockKhz / 1000);
	paintStack();
	run(at125, false);
	const auto stackBytes = stackUsed();

	// Top of the datasheet's rated range, which needs 1.15 V
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	sleep_ms(10);
	const bool clockSet = set_sys_clock_khz(200000, false);
	sleep_ms(10);
	Result at200;
	if (clockSet) {
		at200.sysClockKhz = clock_get_hz(clk_sys) / 1000;
		printf("Decoding at %lu MHz...\n", at200.sysClockKhz / 1000);
		run(at200, false);
	}

	printf("Hashing output...\n");
	Result hashed;
	run(hashed, true);

	for (;;) {
		printf("\n==== md_mpeg RP2040 benchmark, clip %u bytes ====\n", static_cast<unsigned>(clip_end - clip_start));
		printf("frames %lu, samples %llu, %lu Hz, %lu channels\n", at125.frames, at125.samples, at125.sampleRate, at125.channels);
		printTiming(at125);
		if (clockSet) {
			printTiming(at200);
		}
		printf("memory: heap peak %lu bytes (%lu used after decode, %lu before main), stack %lu of %d bytes\n",
		       at125.heapArena, at125.heapUsed, bootHeapUsed, stackBytes, PICO_STACK_SIZE);
		printf("output: video %016llx audio %016llx frames %lu samples %llu%s%s\n", hashed.videoHash, hashed.audioHash, hashed.frames, hashed.samples,
		       *hashed.error ? " " : "", hashed.error);
		printf("==== end ====\n");
		fflush(stdout);
		sleep_ms(3000);
	}
}
