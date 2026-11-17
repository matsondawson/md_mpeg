#pragma once

// ATSC A/52 audio decoder (also published as ETSI TS 102 366), the usual audio of DVD-Video. Built with
// MD_MPEG_ENABLE_MPEG2, since A/52 audio comes in MPEG-2 program streams (.vob).
//
// Decodes every channel layout. 3 to 5 channels are downmixed to stereo with the stream's centre and surround mix
// levels, and the low frequency effects channel is left out, as the standard allows. Output is mono for a mono
// stream (or with MD_MPEG_AUDIO_MONO), stereo otherwise, in the same layout as the MP2 decoder: blocks of 32 left
// samples then 32 right. Decodes with floating point, then converts to AudioSample.

#include "MpegProgramStream.h"

#if MD_MPEG_ENABLE_MPEG2

#include <functional>
#include <memory>

#include "AudioSample.h"
#include "../stream/MStream.h"
#include "../stream/RingBufferWithSizeEvent.h"

// 1: mixes stereo to mono, see Mpeg1AudioDecoder.h
#ifndef MD_MPEG_AUDIO_MONO
#define MD_MPEG_AUDIO_MONO 0
#endif

class A52AudioDecoder final : public MStream::IWriteByteStream {
public:
	using FrameCallback = std::function<void(uint32_t frequency, uint32_t channels, AudioSampleBuffer *soundBuffer)>;

	// Samples per channel in one frame: 6 blocks of 256
	static constexpr uint32_t SAMPLES_PER_FRAME = 1536;
	// Largest frame: 640 kbit/s at 32 kHz
	static constexpr uint32_t MAX_FRAME_BYTES = 3840;

	A52AudioDecoder();
	~A52AudioDecoder() override;

	void setFrameCallback(const FrameCallback &frameCallback);

	[[nodiscard]] int32_t firstFramePtsMillis() const { return m_firstFramePtsMillis; }
	// Channels in the output, 0 until the first frame is decoded
	[[nodiscard]] uint32_t channels() const { return m_outputChannels; }
	// A complete frame is buffered, waiting for space in the sound buffer
	[[nodiscard]] bool hasPendingFrame();
	// Discards all buffered input and decoding state, e.g. after seeking. The sound buffer is cleared by its
	// consumer.
	void reset();

	// IWriteByteStream
	int32_t getPts(int32_t frameTimeMillis) override;
	void setPts(int32_t ptsMillis) override;
	void writeByte(uint8_t data) override;
	[[nodiscard]] bool isFull() override;
	void executeCallbacks() override;
	void flush() override;
	// Drops a partly received frame, keeping the timing and output format
	void discontinuity() override;

private:
	// The 6 bytes up to bsid, enough to validate a frame header
	static constexpr uint32_t HEADER_SIZE = 6;

	enum class State { Header, Frame };

	struct Decoder;

	// Decodes what is buffered, until more input or space in the sound buffer is needed
	void decode();
	void decodeHeader();
	// False if the frame is waiting for space in the sound buffer
	bool decodeBufferedFrame();
	[[nodiscard]] uint32_t requiredBytes() const { return m_state == State::Header ? HEADER_SIZE : m_frameBytes; }
	// Frame size in bytes if the buffered header is valid, else 0
	[[nodiscard]] uint32_t validHeaderFrameBytes();
	[[nodiscard]] bool bufferedFrameCrcValid();
	void outputFrame(const float *left, const float *right);

	FrameCallback m_frameCallback;
	State m_state = State::Header;
	uint32_t m_frameBytes = 0;
	// The current frame is read but not decoded: no PTS was known for it
	bool m_discardFrame = false;

	int32_t m_firstFramePtsMillis = -1;
	// Set by the first decoded frame, after which they can't change
	uint32_t m_sampleRateCode = 0;
	uint32_t m_sampleRate = 0;
	uint32_t m_outputChannels = 0;

	// Bytes skipped looking for a valid frame since the last good frame
	uint32_t m_resyncSkippedBytes = 0;
	// After a reset the first packet usually starts mid-frame, so the first skip isn't corruption
	bool m_expectResync = false;
	// Headers of an unsupported version seen before any valid frame; enough of them disables audio
	uint32_t m_unsupportedHeaderCount = 0;
	bool m_audioDisabled = false;

	std::unique_ptr<Decoder> m_decoder;
	AudioSampleBuffer m_soundBuffer;
	MStream::RingBufferWithSizeEvent<2 * MAX_FRAME_BYTES + 512> m_stream;
};

#endif
