#pragma once

#include "MpegProgramStream.h"
#include "Mpeg1AudioDecoder.h"
#include "A52AudioDecoder.h"

/**
 * The decoders for each audio format a program stream can carry: MP2, and A/52 when built with MPEG-2. The demuxer
 * only decodes the first audio stream it finds, so only one of them gets any data.
 */
class AudioDecoders {
public:
	using FrameCallback = std::function<void(uint32_t frequency, uint32_t channels, AudioSampleBuffer *soundBuffer)>;

	Mpeg1AudioDecoder mpeg;
#if MD_MPEG_ENABLE_MPEG2
	A52AudioDecoder a52;
#endif

	void attach(Mpeg1::MpegProgramStream &demuxer) {
		demuxer.setAudioDecoder(&mpeg);
#if MD_MPEG_ENABLE_MPEG2
		demuxer.setA52AudioDecoder(&a52);
#endif
	}

	void setFrameCallback(const FrameCallback &frameCallback) {
		mpeg.setFrameCallback(frameCallback);
#if MD_MPEG_ENABLE_MPEG2
		a52.setFrameCallback(frameCallback);
#endif
	}

	// PTS of the first decoded audio frame, -1 until known
	[[nodiscard]] int32_t firstFramePtsMillis() const {
#if MD_MPEG_ENABLE_MPEG2
		if (a52.firstFramePtsMillis() >= 0) {
			return a52.firstFramePtsMillis();
		}
#endif
		return mpeg.firstFramePtsMillis();
	}

	// A complete frame is buffered, waiting for space in the sound buffer
	[[nodiscard]] bool hasPendingFrame() {
#if MD_MPEG_ENABLE_MPEG2
		if (a52.hasPendingFrame()) {
			return true;
		}
#endif
		return mpeg.hasPendingFrame();
	}

	// Decodes frames that were waiting for space in the sound buffer
	void executeCallbacks() {
		mpeg.executeCallbacks();
#if MD_MPEG_ENABLE_MPEG2
		a52.executeCallbacks();
#endif
	}

	// Discards all buffered input and decoding state, e.g. after seeking
	void reset() {
		mpeg.reset();
#if MD_MPEG_ENABLE_MPEG2
		a52.reset();
#endif
	}
};
