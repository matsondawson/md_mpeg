#pragma once

// DVD-Video LPCM audio: uncompressed big-endian samples of 16, 20 or 24 bits, 1 to 8 channels, at 48 or 96 kHz,
// carried in private stream 1 sub-streams 0xA0 to 0xA7. Built with MD_MPEG_ENABLE_MPEG2, like the A/52 decoder.
//
// Each packet starts with a 6-byte header (after the sub-stream id): the number of frames that start in the packet,
// a pointer to the first of them, and the format. Samples aren't otherwise marked, so decoding starts at a frame
// start and then follows the byte stream across packets.
//
// 3 to 8 channels are downmixed to stereo, assuming ffmpeg's layouts for those channel counts (2.1, 4.0, 5.0, 5.1,
// 6.1, 7.1), and leaving out the low frequency effects channel. Output is mono for a mono stream (or with
// MD_MPEG_AUDIO_MONO), stereo otherwise, in the same layout as the other decoders: blocks of 32 left samples then
// 32 right.

#include "MpegProgramStream.h"

#if MD_MPEG_ENABLE_MPEG2

#include <functional>

#include "AudioSample.h"
#include "../stream/MStream.h"

// 1: mixes stereo to mono, see Mpeg1AudioDecoder.h
#ifndef MD_MPEG_AUDIO_MONO
#define MD_MPEG_AUDIO_MONO 0
#endif

class LpcmAudioDecoder final : public MStream::IWriteByteStream {
public:
	using FrameCallback = std::function<void(uint32_t frequency, uint32_t channels, AudioSampleBuffer *soundBuffer)>;

	// Samples per channel written to the sound buffer at a time
	static constexpr uint32_t BLOCK_SAMPLES = 32;
	static constexpr uint32_t MAX_CHANNELS = 8;

	void setFrameCallback(const FrameCallback &frameCallback) { m_frameCallback = frameCallback; }

	[[nodiscard]] int32_t firstFramePtsMillis() const { return m_firstFramePtsMillis; }
	// Channels in the output, 0 until the first frame is found
	[[nodiscard]] uint32_t channels() const { return m_outputChannels; }
	// Samples are decoded as their bytes arrive, so none ever wait for space in the sound buffer
	[[nodiscard]] bool hasPendingFrame() const { return false; }
	// Discards all buffered input and decoding state, e.g. after seeking. The sound buffer is cleared by its
	// consumer.
	void reset();

	// IWriteByteStream
	int32_t getPts(int32_t) override { return m_packetPts; }
	void setPts(int32_t ptsMillis) override { m_packetPts = ptsMillis; }
	void writeByte(uint8_t data) override;
	// Full while the sound buffer can't take another block
	[[nodiscard]] bool isFull() override;
	void executeCallbacks() override {
	}
	void flush() override {
	}
	// Drops a partly received sample, and waits for the next frame start
	void discontinuity() override;
	// A packet of length bytes follows: its 6-byte LPCM header, then sample data
	void startPacket(uint32_t length) override;

private:
	static constexpr uint32_t HEADER_SIZE = 6;
	// The largest unit of sample data: 4 samples of 24 bits
	static constexpr uint32_t MAX_UNIT_BYTES = 12;

	void decodeHeader();
	// Decodes the buffered unit of sample data
	void decodeUnit();
	void addValue(float value);
	void addSample();
	void outputBlock();

	FrameCallback m_frameCallback;

	// The packet being received
	uint8_t m_header[HEADER_SIZE] = {};
	uint32_t m_headerBytesNeeded = 0;
	uint32_t m_packetDataBytes = 0;
	int32_t m_packetPts = -1;
	// Bytes before the packet's first frame, skipped when starting on it
	uint32_t m_skipBytes = 0;
	bool m_dropPacket = true;

	// Decoding at a unit boundary of the stream's samples
	bool m_synced = false;
	// Format of the samples being decoded, 0 until known
	uint32_t m_bits = 0;
	uint32_t m_channels = 0;
	uint32_t m_unitBytes = 0;
	uint8_t m_unit[MAX_UNIT_BYTES] = {};
	uint32_t m_unitUsed = 0;
	// The sample being assembled, one value per channel
	float m_sample[MAX_CHANNELS] = {};
	uint32_t m_channel = 0;

	// Set by the first frame, after which they can't change
	int32_t m_firstFramePtsMillis = -1;
	uint32_t m_sampleRate = 0;
	uint32_t m_outputChannels = 0;

	float m_left[BLOCK_SAMPLES] = {};
	float m_right[BLOCK_SAMPLES] = {};
	uint32_t m_blockUsed = 0;

	AudioSampleBuffer m_soundBuffer;
};

#endif
