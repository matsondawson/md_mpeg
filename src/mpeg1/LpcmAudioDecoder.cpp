#include "LpcmAudioDecoder.h"

#if MD_MPEG_ENABLE_MPEG2

#include <algorithm>
#include <cmath>

namespace {
	constexpr uint32_t SAMPLE_RATE[4] = {48000, 96000, 44100, 32000};

	// Downmix to stereo of 3 to 8 channels, in ffmpeg's channel order for each count:
	// 2.1: FL FR LFE, 4.0: FL FR FC BC, 5.0: FL FR FC BL BR, 5.1: FL FR FC LFE BL BR, 6.1: FL FR FC LFE BC SL SR,
	// 7.1: FL FR FC LFE BL BR SL SR. Centre and surround channels are mixed in at -3 dB, the low frequency effects
	// channel is left out. Each row is scaled so a full scale signal on every channel can't clip.
	constexpr float C = 0.70710678f;
	struct Mix {
		float left[LpcmAudioDecoder::MAX_CHANNELS];
		float right[LpcmAudioDecoder::MAX_CHANNELS];
	};
	constexpr Mix DOWNMIX[LpcmAudioDecoder::MAX_CHANNELS + 1] = {
		{}, {}, {},
		{{1, 0, 0}, {0, 1, 0}},
		{{1, 0, C, C}, {0, 1, C, C}},
		{{1, 0, C, C, 0}, {0, 1, C, 0, C}},
		{{1, 0, C, 0, C, 0}, {0, 1, C, 0, 0, C}},
		{{1, 0, C, 0, C, C, 0}, {0, 1, C, 0, C, 0, C}},
		{{1, 0, C, 0, C, 0, C, 0}, {0, 1, C, 0, 0, C, 0, C}},
	};

	AudioSample toAudioSample(const float value) {
#if MD_MPEG_AUDIO_FIXED_POINT
		const float scaled = std::nearbyint(value * 32768.0f);
		return static_cast<AudioSample>(std::clamp(scaled, -32768.0f, 32767.0f));
#else
		return value;
#endif
	}

	int32_t be16(const uint8_t *bytes) {
		return static_cast<int16_t>(bytes[0] << 8 | bytes[1]);
	}
}

void LpcmAudioDecoder::reset() {
	m_headerBytesNeeded = 0;
	m_packetPts = -1;
	m_dropPacket = true;
	m_synced = false;
	m_unitUsed = 0;
	m_channel = 0;
	m_blockUsed = 0;
	m_firstFramePtsMillis = -1;
}

void LpcmAudioDecoder::discontinuity() {
	// Samples already in the output block are kept, so the timing carries on
	m_synced = false;
	m_unitUsed = 0;
	m_channel = 0;
	m_dropPacket = true;
}

void LpcmAudioDecoder::startPacket(const uint32_t length) {
	m_headerBytesNeeded = HEADER_SIZE;
	m_packetDataBytes = length > HEADER_SIZE ? length - HEADER_SIZE : 0;
	m_packetPts = -1;
	m_dropPacket = true;
}

bool LpcmAudioDecoder::isFull() {
	return m_soundBuffer.free() < BLOCK_SAMPLES * (m_outputChannels ? m_outputChannels : 2);
}

void LpcmAudioDecoder::writeByte(const uint8_t data) {
	if (m_headerBytesNeeded) {
		m_header[HEADER_SIZE - m_headerBytesNeeded--] = data;
		if (!m_headerBytesNeeded) {
			decodeHeader();
		}
		return;
	}
	if (m_dropPacket) {
		return;
	}
	if (m_skipBytes) {
		m_skipBytes--;
		return;
	}
	m_unit[m_unitUsed++] = data;
	if (m_unitUsed == m_unitBytes) {
		m_unitUsed = 0;
		decodeUnit();
	}
}

// Header: number of frames starting in the packet, first access unit pointer (2 bytes, counted from its own last
// byte, so 4 is the first data byte), emphasis/mute/frame number, quantization (2 bits: 16, 20, 24 bits) / sample
// rate (2 bits) / reserved / channels - 1 (3 bits), dynamic range control
void LpcmAudioDecoder::decodeHeader() {
	const uint32_t quantization = m_header[4] >> 6;
	const uint32_t sampleRate = SAMPLE_RATE[(m_header[4] >> 4) & 3];
	const uint32_t channels = (m_header[4] & 7) + 1;
	// The audio output is opened for the first frame's sample rate, so it can't change
	if (quantization == 3 || (m_outputChannels && sampleRate != m_sampleRate)) {
		m_synced = false;
		return;
	}
	const uint32_t bits = 16 + 4 * quantization;
	if (bits != m_bits || channels != m_channels) {
		m_synced = false;
		m_bits = bits;
		m_channels = channels;
		// 16 bits: one sample value per unit. 20 and 24 bits: the top 16 bits of 4 values (2 for mono), then their
		// low bits: a byte each for 24 bits, a nibble each for 20. Values are in sample then channel order.
		const uint32_t values = channels == 1 ? 2 : 4;
		m_unitBytes = bits == 16 ? 2 : values * bits / 8;
	}
	if (!m_synced) {
		const uint32_t frames = m_header[0];
		const uint32_t pointer = m_header[1] << 8 | m_header[2];
		if (!frames || pointer < 4 || pointer - 4 >= m_packetDataBytes) {
			return; // no frame starts in this packet
		}
		// The audio clock is anchored on the first frame's PTS; drop packets until one is known
		if (m_firstFramePtsMillis < 0) {
			if (m_packetPts < 0) {
				return;
			}
			m_firstFramePtsMillis = m_packetPts;
		}
		if (!m_outputChannels) {
			m_sampleRate = sampleRate;
			m_outputChannels = MD_MPEG_AUDIO_MONO || channels == 1 ? 1 : 2;
		}
		m_skipBytes = pointer - 4;
		m_unitUsed = 0;
		m_channel = 0;
		m_synced = true;
	}
	m_dropPacket = false;
}

void LpcmAudioDecoder::decodeUnit() {
	if (m_bits == 16) {
		addValue(static_cast<float>(be16(m_unit)) * (1.0f / 32768.0f));
		return;
	}
	const uint32_t values = m_channels == 1 ? 2 : 4;
	for (uint32_t i = 0; i < values; i++) {
		const int32_t high = be16(m_unit + 2 * i);
		const uint8_t low = m_unit[2 * values + (m_bits == 24 ? i : i / 2)];
		if (m_bits == 24) {
			addValue(static_cast<float>(high * 256 + low) * (1.0f / 8388608.0f));
		} else {
			const int32_t nibble = i & 1 ? low & 15 : low >> 4;
			addValue(static_cast<float>(high * 16 + nibble) * (1.0f / 524288.0f));
		}
	}
}

void LpcmAudioDecoder::addValue(const float value) {
	m_sample[m_channel++] = value;
	if (m_channel == m_channels) {
		m_channel = 0;
		addSample();
	}
}

void LpcmAudioDecoder::addSample() {
	float left = m_sample[0], right = m_sample[0];
	if (m_channels == 2) {
		right = m_sample[1];
	} else if (m_channels > 2) {
		const Mix &mix = DOWNMIX[m_channels];
		float sum = 0;
		left = right = 0;
		for (uint32_t ch = 0; ch < m_channels; ch++) {
			left += mix.left[ch] * m_sample[ch];
			right += mix.right[ch] * m_sample[ch];
			sum += mix.left[ch];
		}
		left /= sum;
		right /= sum;
	}
	if (m_outputChannels == 1) {
		m_left[m_blockUsed] = (left + right) * 0.5f;
	} else {
		m_left[m_blockUsed] = left;
		m_right[m_blockUsed] = right;
	}
	if (++m_blockUsed == BLOCK_SAMPLES) {
		outputBlock();
	}
}

void LpcmAudioDecoder::outputBlock() {
	m_blockUsed = 0;
	for (const float value: m_left) {
		m_soundBuffer.writeByte(toAudioSample(value));
	}
	if (m_outputChannels == 2) {
		for (const float value: m_right) {
			m_soundBuffer.writeByte(toAudioSample(value));
		}
	}
	if (m_frameCallback) {
		m_frameCallback(m_sampleRate, m_outputChannels, &m_soundBuffer);
	}
}

#endif
