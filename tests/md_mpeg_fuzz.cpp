// libFuzzer harness for the demuxer, decoders and DVD information file parser. Built with -DMD_MPEG_FUZZ=ON and
// Clang; scripts/check.sh runs it for a fixed time from a corpus of generated clips.
//
// The first byte picks what the rest is fed to: a program stream (demuxed into the video and audio decoders),
// an elementary video stream, or the .IFO parser. Corrupt input may throw; it must not crash, hang or leak.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

#include "stream/MStream.h"
#include "mpeg1/MpegProgramStream.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/AudioDecoders.h"
#if MD_MPEG_ENABLE_MPEG2
#include "dvd/Ifo.h"
#endif

namespace {
	class MemoryRead final : public MStream::IReadByteStream {
		const uint8_t *m_data;
		size_t m_size;
		size_t m_position = 0;

	public:
		MemoryRead(const uint8_t *data, const size_t size) : m_data(data), m_size(size) {}

		uint8_t nextByte() override {
			if (m_position == m_size) {
				throw MStream::EndOfData();
			}
			return m_data[m_position++];
		}

		bool isEmpty() override {
			return m_position == m_size;
		}
	};

	void drainAudio(uint32_t, uint32_t, AudioSampleBuffer *soundBuffer) {
		while (!soundBuffer->isEmpty()) {
			soundBuffer->nextByte();
		}
	}

	void fuzzProgramStream(const uint8_t *data, const size_t size) {
		MemoryRead input(data, size);
		Mpeg1::MpegProgramStream demux;
		Mpeg1VideoDecoder video;
		AudioDecoders audio;
		demux.setVideoDecoder(&video);
		audio.attach(demux);
		video.setFrameCallback([](const Frame &) {});
		audio.setFrameCallback(drainAudio);
		// Bounded, so a demuxer that stops consuming input shows up as a timeout rather than running forever
		for (size_t iteration = 0; iteration < size * 4 + 1000; iteration++) {
			try {
				if (demux.nextPacket(input) == Mpeg1::DemuxResult::EndOfStream) {
					for (int i = 0; i < 1000 && audio.hasPendingFrame(); i++) {
						audio.executeCallbacks();
					}
					return;
				}
			} catch (const std::exception &) {
				return;
			}
		}
		__builtin_trap(); // didn't reach the end of the input
	}

	void fuzzVideo(const uint8_t *data, const size_t size) {
		Mpeg1VideoDecoder video;
		video.setFrameCallback([](const Frame &) {});
		try {
			for (size_t i = 0; i < size; i++) {
				video.writeByte(data[i]);
			}
			video.flush();
		} catch (const std::exception &) {
		}
	}

#if MD_MPEG_ENABLE_MPEG2
	void fuzzIfo(const uint8_t *data, const size_t size) {
		try {
			(void)Dvd::parseIfo(std::vector<uint8_t>(data, data + size));
		} catch (const std::exception &) {
		}
	}
#endif
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
	if (size < 1) {
		return 0;
	}
	switch (data[0] % 3) {
		case 0:
			fuzzProgramStream(data + 1, size - 1);
			break;
		case 1:
			fuzzVideo(data + 1, size - 1);
			break;
		default:
#if MD_MPEG_ENABLE_MPEG2
			fuzzIfo(data + 1, size - 1);
#endif
			break;
	}
	return 0;
}
