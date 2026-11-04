#pragma once

#include "../common.h"
#include "../stream/MStream.h"
#include "../stream/RingBufferTyped.h"
#include "../stream/RingBufferWithSizeEvent.h"
#include "../stream/ReadBitStream.h"
#include "Mpeg1VideoDecoder.h"

#include <functional>

typedef struct quantizer_spec_t {
	unsigned short levels;
	unsigned char group;
	unsigned char bits;
} quantizer_spec_t;


class Mpeg1AudioDecoder final : public MStream::IWriteByteStream
{
private:
	std::function<void(uint32_t frequency, uint32_t channels, MStream::RingBufferTyped<float, 32768>* soundBuffer)> m_frameCallback;
	int m_decoderState;

	MStream::RingBufferTyped<float, 32768> m_soundBuffer;

	bool m_hasLoggedHeaderInfo = false;

	// PTS of the first decoded audio frame, -1 until known
	int32_t m_firstFramePtsMillis = -1;

	// Bytes skipped looking for a valid frame header since the last good frame
	uint32_t m_resyncSkippedBytes = 0;
	// After a reset the first packet usually starts mid-frame, so the first skip isn't corruption
	bool m_expectResync = false;
	// Headers of other MPEG audio formats seen before any valid frame; enough of them disables audio
	uint32_t m_unsupportedHeaderCount = 0;
	bool m_audioDisabled = false;

	uint8_t m_audioVersion = 0;
	uint8_t m_layerDescription = 0;
	uint8_t m_protectionBit = 0;
	uint8_t m_bitRateIndex = 0;
	uint8_t m_samplingFrequencyIndex = 0;
	uint8_t m_channelMode = 0;
	uint32_t m_modeExtension = 0;
	uint32_t m_copyright = 0;
	uint32_t m_original = 0;
	uint32_t m_emphasis = 0;

	uint32_t m_bitRate = 0;
	uint32_t m_samplingFrequency = 0;
	uint32_t m_frameSize = 0;

	// derived
	uint8_t m_bound = 0;

	// from plm
	int v_pos = 0;

	const quantizer_spec_t* allocation[2][32]{nullptr};
	uint8_t scale_factor_info[2][32]{0};
	uint8_t m_scale_factor[2][32][3]{0};
	int sample[2][32][3]{0};

	float V[2][1024]{0};
	float U[32]{0};

public:
	MStream::RingBufferWithSizeEvent<128*1024> m_stream;
private:
	MStream::ReadBitStream m_bitStream;

public:
	Mpeg1AudioDecoder();

public:
	void setFrameCallback(const std::function<void(uint32_t frequency, uint32_t channels, MStream::RingBufferTyped<float, 32768>* soundBuffer)> &);

	// Samples per channel in one MPEG-1 layer II frame
	static constexpr uint32_t SAMPLES_PER_FRAME = 1152;

	[[nodiscard]] int32_t firstFramePtsMillis() const { return m_firstFramePtsMillis; }
	[[nodiscard]] uint32_t channels() const;
	// A complete frame is buffered, waiting for space in the sound buffer
	[[nodiscard]] bool hasPendingFrame();

	// IWriteByteStream
	int32_t getPts(int32_t frameTimeMillis) override;
	void setPts(int32_t ptsMillis) override;
	void writeByte(uint8_t data) override;
	[[nodiscard]] bool isFull() override;
	void executeCallbacks() override;
	void flush() override;

	void decode();
	void decodeHeader();
	bool isValidHeader(uint32_t header);
	void resync();
	// Discards all buffered input and decoding state, e.g. after seeking. The sound buffer is
	// cleared by its consumer.
	void reset();
	void decodeFrame();
	const quantizer_spec_t* audio_read_allocation(int sb, int tab3);
	void audio_read_samples(int ch, int sb, int part);
};
