// ATSC A/52 audio decoder, written from the standard (ETSI TS 102 366). Clause and table numbers below are that
// document's.

#include "A52AudioDecoder.h"

#if MD_MPEG_ENABLE_MPEG2

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
	constexpr uint32_t SYNC_WORD = 0x0B77;

	// Bit rate in kbit/s by frmsizecod / 2 (table 4.13)
	constexpr uint16_t BIT_RATE[19] = {
		32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640
	};
	constexpr uint32_t SAMPLE_RATE[3] = {48000, 44100, 32000};

	// Full bandwidth channels by acmod
	constexpr int FULL_CHANNELS[8] = {2, 1, 2, 3, 3, 4, 4, 5};

	// Frame size in bytes (table 4.13), 0 if the codes are invalid
	uint32_t frameBytes(const uint32_t fscod, const uint32_t frmsizecod) {
		if (fscod > 2 || frmsizecod >= 38) {
			return 0;
		}
		const uint32_t bitRate = BIT_RATE[frmsizecod >> 1];
		uint32_t words;
		switch (fscod) {
			case 0: words = bitRate * 2; break;
			case 1: words = bitRate * 96000 / 44100 + (frmsizecod & 1); break;
			default: words = bitRate * 3; break;
		}
		return words * 2;
	}

	// ---------------------------------------------------------------- bit allocation tables (clause 6.2.3)

	constexpr int SLOW_DECAY[4] = {0x0f, 0x11, 0x13, 0x15};
	constexpr int FAST_DECAY[4] = {0x3f, 0x53, 0x67, 0x7b};
	constexpr int SLOW_GAIN[4] = {0x540, 0x4d8, 0x478, 0x410};
	constexpr int DB_PER_BIT[4] = {0x000, 0x700, 0x900, 0xb00};
	// The last value is 0xf800 as a 16-bit signed number
	constexpr int FLOOR[8] = {0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -0x800};
	constexpr int FAST_GAIN[8] = {0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};

	constexpr int BANDS = 50;
	// First bin of each band (table 6.12)
	constexpr uint8_t BAND_START[BANDS + 1] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
		28, 31, 34, 37, 40, 43, 46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133, 157, 181, 205, 229, 253
	};

	// Band of each bin (table 6.13), from BAND_START
	struct BandOfBin {
		uint8_t values[256];
	};

	constexpr BandOfBin makeBandOfBin() {
		BandOfBin table{};
		for (int band = 0; band < BANDS; band++) {
			for (int bin = BAND_START[band]; bin < BAND_START[band + 1]; bin++) {
				table.values[bin] = static_cast<uint8_t>(band);
			}
		}
		return table;
	}

	constexpr BandOfBin BAND_OF_BIN = makeBandOfBin();

	// Log-addition table (table 6.14)
	constexpr uint8_t LOG_ADD[256] = {
		0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x34, 0x33, 0x32,
		0x31, 0x30, 0x2f, 0x2f, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x29, 0x29, 0x28, 0x27, 0x26, 0x26,
		0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c,
		0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x15,
		0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f,
		0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b,
		0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
		0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05,
		0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
		0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	// Hearing threshold by fscod (table 6.15)
	constexpr uint16_t HEARING_THRESHOLD[3][BANDS] = {
		{
			0x4d0, 0x4d0, 0x440, 0x400, 0x3e0, 0x3c0, 0x3b0, 0x3b0, 0x3a0, 0x3a0, 0x3a0, 0x3a0, 0x3a0,
			0x390, 0x390, 0x390, 0x380, 0x380, 0x370, 0x370, 0x360, 0x360, 0x350, 0x350, 0x340, 0x340,
			0x330, 0x320, 0x310, 0x300, 0x2f0, 0x2f0, 0x2f0, 0x2f0, 0x300, 0x310, 0x340, 0x390, 0x3e0,
			0x420, 0x460, 0x490, 0x4a0, 0x460, 0x440, 0x440, 0x520, 0x800, 0x840, 0x840
		},
		{
			0x4f0, 0x4f0, 0x460, 0x410, 0x3e0, 0x3d0, 0x3c0, 0x3b0, 0x3b0, 0x3a0, 0x3a0, 0x3a0, 0x3a0,
			0x3a0, 0x390, 0x390, 0x390, 0x380, 0x380, 0x380, 0x370, 0x370, 0x360, 0x360, 0x350, 0x350,
			0x340, 0x340, 0x320, 0x310, 0x300, 0x2f0, 0x2f0, 0x2f0, 0x2f0, 0x300, 0x320, 0x350, 0x390,
			0x3e0, 0x420, 0x450, 0x4a0, 0x490, 0x460, 0x440, 0x480, 0x630, 0x840, 0x840
		},
		{
			0x580, 0x580, 0x4b0, 0x450, 0x420, 0x3f0, 0x3e0, 0x3d0, 0x3c0, 0x3b0, 0x3b0, 0x3b0, 0x3a0,
			0x3a0, 0x3a0, 0x3a0, 0x3a0, 0x3a0, 0x3a0, 0x3a0, 0x390, 0x390, 0x390, 0x390, 0x380, 0x380,
			0x380, 0x370, 0x360, 0x350, 0x340, 0x330, 0x320, 0x310, 0x300, 0x2f0, 0x2f0, 0x2f0, 0x300,
			0x310, 0x330, 0x350, 0x3c0, 0x410, 0x470, 0x4a0, 0x460, 0x440, 0x450, 0x4e0
		}
	};

	// Bit allocation pointer table (table 6.16)
	constexpr uint8_t BAP_TABLE[64] = {
		0, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10,
		10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15,
		15, 15, 15, 15, 15, 15
	};

	// Mantissa bits for the ungrouped quantizers, by bap (table 6.18)
	constexpr int MANTISSA_BITS[16] = {0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

	// Rematrixing band edges (tables 6.25 to 6.28)
	constexpr int REMATRIX_BAND_START[5] = {13, 25, 37, 61, 253};

	// Downmix levels (tables 4.4 and 4.5). Reserved codes use the middle level (clause 6.8.2).
	constexpr float CENTRE_MIX_LEVEL[4] = {0.7071068f, 0.5946036f, 0.5f, 0.5946036f};
	constexpr float SURROUND_MIX_LEVEL[4] = {0.7071068f, 0.5f, 0.0f, 0.5f};

	constexpr int MAX_CHANNELS = 5;
	// Exponent and bap arrays: the full bandwidth channels, then these
	constexpr int CPL = 5;
	constexpr int LFE = 6;
	constexpr int BLOCK_SAMPLES = 256;
	constexpr int COUPLING_SUB_BANDS = 18;
	constexpr int DELTA_SEGMENTS = 8;

	// CRC-16 with generator x^16 + x^15 + x^2 + 1 (clause 6.10.1)
	struct CrcTable {
		uint16_t values[256];
	};

	constexpr CrcTable makeCrcTable() {
		CrcTable table{};
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t crc = i << 8;
			for (int bit = 0; bit < 8; bit++) {
				crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1;
			}
			table.values[i] = static_cast<uint16_t>(crc);
		}
		return table;
	}

	constexpr CrcTable CRC_TABLE = makeCrcTable();

	// A frame that can't be decoded
	struct FrameError {};

	// Reads a frame's bits, most significant first. The data is followed by 4 zero bytes, so a read can look ahead.
	class BitReader {
		const uint8_t *m_data;
		uint32_t m_position = 0;
		uint32_t m_end;

	public:
		BitReader(const uint8_t *data, const uint32_t bytes) : m_data(data), m_end(bytes * 8) {
		}

		// 1..16 bits
		uint32_t get(const int bits) {
			if (m_position + bits > m_end) {
				throw FrameError();
			}
			const uint8_t *p = m_data + (m_position >> 3);
			const uint32_t word = static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
			const uint32_t value = (word << (m_position & 7)) >> (32 - bits);
			m_position += bits;
			return value;
		}

		void skip(const uint32_t bits) {
			if (m_position + bits > m_end) {
				throw FrameError();
			}
			m_position += bits;
		}

		void seek(const uint32_t bitPosition) {
			m_position = bitPosition;
		}
	};

	// 2^exponent
	inline float powerOfTwo(const int exponent) {
		return std::ldexp(1.0f, exponent);
	}
}

// ---------------------------------------------------------------- inverse transform (clause 6.9.4)

namespace {
	// In-place complex inverse FFT without normalisation: z[n] = sum Z[k] * e^(+j 2 pi k n / size)
	template <int Size>
	class InverseFft {
		float m_cos[Size / 2];
		float m_sin[Size / 2];
		uint8_t m_reversed[Size];

	public:
		InverseFft() {
			for (int i = 0; i < Size / 2; i++) {
				m_cos[i] = static_cast<float>(std::cos(2.0 * M_PI * i / Size));
				m_sin[i] = static_cast<float>(std::sin(2.0 * M_PI * i / Size));
			}
			int bits = 0;
			while ((1 << bits) < Size) {
				bits++;
			}
			for (int i = 0; i < Size; i++) {
				int reversed = 0;
				for (int b = 0; b < bits; b++) {
					reversed |= ((i >> b) & 1) << (bits - 1 - b);
				}
				m_reversed[i] = static_cast<uint8_t>(reversed);
			}
		}

		void run(float *re, float *im) const {
			for (int i = 0; i < Size; i++) {
				const int j = m_reversed[i];
				if (j > i) {
					std::swap(re[i], re[j]);
					std::swap(im[i], im[j]);
				}
			}
			for (int length = 2; length <= Size; length <<= 1) {
				const int half = length >> 1;
				const int step = Size / length;
				for (int start = 0; start < Size; start += length) {
					for (int k = 0; k < half; k++) {
						const float c = m_cos[k * step];
						const float s = m_sin[k * step];
						const int a = start + k;
						const int b = a + half;
						const float tr = re[b] * c - im[b] * s;
						const float ti = re[b] * s + im[b] * c;
						re[b] = re[a] - tr;
						im[b] = im[a] - ti;
						re[a] += tr;
						im[a] += ti;
					}
				}
			}
		}
	};

	class InverseTransform {
		static constexpr int N = 512;

		InverseFft<N / 4> m_fft128;
		InverseFft<N / 8> m_fft64;
		// Pre- and post-FFT twiddles of the 512 and 256 sample transforms
		float m_cos1[N / 4], m_sin1[N / 4];
		float m_cos2[N / 8], m_sin2[N / 8];
		// Kaiser-Bessel derived window, alpha 5 (table 6.33)
		float m_window[N / 2];

		static double besselI0(const double x) {
			double sum = 1.0, term = 1.0;
			for (int k = 1; k < 50; k++) {
				term *= (x / (2.0 * k)) * (x / (2.0 * k));
				sum += term;
			}
			return sum;
		}

	public:
		InverseTransform() {
			for (int k = 0; k < N / 4; k++) {
				m_cos1[k] = static_cast<float>(-std::cos(2.0 * M_PI * (8 * k + 1) / (8.0 * N)));
				m_sin1[k] = static_cast<float>(-std::sin(2.0 * M_PI * (8 * k + 1) / (8.0 * N)));
			}
			for (int k = 0; k < N / 8; k++) {
				m_cos2[k] = static_cast<float>(-std::cos(2.0 * M_PI * (8 * k + 1) / (4.0 * N)));
				m_sin2[k] = static_cast<float>(-std::sin(2.0 * M_PI * (8 * k + 1) / (4.0 * N)));
			}
			double kaiser[N / 2 + 1];
			double total = 0;
			for (int j = 0; j <= N / 2; j++) {
				const double t = 2.0 * j / (N / 2) - 1.0;
				kaiser[j] = besselI0(M_PI * 5.0 * std::sqrt(1.0 - t * t));
				total += kaiser[j];
			}
			double sum = 0;
			for (int n = 0; n < N / 2; n++) {
				sum += kaiser[n];
				m_window[n] = static_cast<float>(std::sqrt(sum / total));
			}
		}

		[[nodiscard]] float window(const int n) const {
			return m_window[n];
		}

		/**
		 * Inverse transforms one block's 256 coefficients, windows and overlaps it with the previous block.
		 * @param coefficients X[k], 256 values. Interleaved pairs of 128 for a block split into two transforms.
		 * @param shortBlocks blksw: two 256 sample transforms
		 * @param delay Second half of the previous block, updated
		 * @param pcm 256 output samples
		 */
		void run(const float *coefficients, const bool shortBlocks, float *delay, float *pcm) const {
			float x[N];
			const float *w = m_window;
			if (!shortBlocks) {
				float re[N / 4], im[N / 4];
				for (int k = 0; k < N / 4; k++) {
					const float a = coefficients[N / 2 - 2 * k - 1];
					const float b = coefficients[2 * k];
					re[k] = a * m_cos1[k] - b * m_sin1[k];
					im[k] = b * m_cos1[k] + a * m_sin1[k];
				}
				m_fft128.run(re, im);
				float yr[N / 4], yi[N / 4];
				for (int n = 0; n < N / 4; n++) {
					yr[n] = re[n] * m_cos1[n] - im[n] * m_sin1[n];
					yi[n] = im[n] * m_cos1[n] + re[n] * m_sin1[n];
				}
				for (int n = 0; n < N / 8; n++) {
					x[2 * n] = -yi[N / 8 + n] * w[2 * n];
					x[2 * n + 1] = yr[N / 8 - n - 1] * w[2 * n + 1];
					x[N / 4 + 2 * n] = -yr[n] * w[N / 4 + 2 * n];
					x[N / 4 + 2 * n + 1] = yi[N / 4 - n - 1] * w[N / 4 + 2 * n + 1];
					x[N / 2 + 2 * n] = -yr[N / 8 + n] * w[N / 2 - 2 * n - 1];
					x[N / 2 + 2 * n + 1] = yi[N / 8 - n - 1] * w[N / 2 - 2 * n - 2];
					x[3 * N / 4 + 2 * n] = yi[n] * w[N / 4 - 2 * n - 1];
					x[3 * N / 4 + 2 * n + 1] = -yr[N / 4 - n - 1] * w[N / 4 - 2 * n - 2];
				}
			} else {
				float re1[N / 8], im1[N / 8], re2[N / 8], im2[N / 8];
				for (int k = 0; k < N / 8; k++) {
					// x1[k] = X[2k], x2[k] = X[2k + 1]
					const float a1 = coefficients[2 * (N / 4 - 2 * k - 1)];
					const float b1 = coefficients[2 * (2 * k)];
					const float a2 = coefficients[2 * (N / 4 - 2 * k - 1) + 1];
					const float b2 = coefficients[2 * (2 * k) + 1];
					re1[k] = a1 * m_cos2[k] - b1 * m_sin2[k];
					im1[k] = b1 * m_cos2[k] + a1 * m_sin2[k];
					re2[k] = a2 * m_cos2[k] - b2 * m_sin2[k];
					im2[k] = b2 * m_cos2[k] + a2 * m_sin2[k];
				}
				m_fft64.run(re1, im1);
				m_fft64.run(re2, im2);
				float yr1[N / 8], yi1[N / 8], yr2[N / 8], yi2[N / 8];
				for (int n = 0; n < N / 8; n++) {
					yr1[n] = re1[n] * m_cos2[n] - im1[n] * m_sin2[n];
					yi1[n] = im1[n] * m_cos2[n] + re1[n] * m_sin2[n];
					yr2[n] = re2[n] * m_cos2[n] - im2[n] * m_sin2[n];
					yi2[n] = im2[n] * m_cos2[n] + re2[n] * m_sin2[n];
				}
				for (int n = 0; n < N / 8; n++) {
					x[2 * n] = -yi1[n] * w[2 * n];
					x[2 * n + 1] = yr1[N / 8 - n - 1] * w[2 * n + 1];
					x[N / 4 + 2 * n] = -yr1[n] * w[N / 4 + 2 * n];
					x[N / 4 + 2 * n + 1] = yi1[N / 8 - n - 1] * w[N / 4 + 2 * n + 1];
					x[N / 2 + 2 * n] = -yr2[n] * w[N / 2 - 2 * n - 1];
					x[N / 2 + 2 * n + 1] = yi2[N / 8 - n - 1] * w[N / 2 - 2 * n - 2];
					x[3 * N / 4 + 2 * n] = yi2[n] * w[N / 4 - 2 * n - 1];
					x[3 * N / 4 + 2 * n + 1] = -yr2[N / 8 - n - 1] * w[N / 4 - 2 * n - 2];
				}
			}
			for (int n = 0; n < N / 2; n++) {
				pcm[n] = 2.0f * (x[n] + delay[n]);
				delay[n] = x[N / 2 + n];
			}
		}
	};

	const InverseTransform &inverseTransform() {
		static const InverseTransform transform;
		return transform;
	}
}

// ---------------------------------------------------------------- frame decoding

struct A52AudioDecoder::Decoder {
	// Frame, with zero padding for the bit reader
	uint8_t frame[MAX_FRAME_BYTES + 4]{};

	// Output of one frame
	float left[SAMPLES_PER_FRAME]{};
	float right[SAMPLES_PER_FRAME]{};

	// Overlap of each full bandwidth channel's last block
	float delay[MAX_CHANNELS][BLOCK_SAMPLES]{};
	// Channel layout of the previous frame, to clear the overlap when it changes
	int lastAcmod = -1;

	uint32_t ditherState = 1;

	// ---- bsi
	int fscod = 0;
	int acmod = 0;
	int channels = 0;
	bool lfeOn = false;
	float centreMix = 0;
	float surroundMix = 0;

	// ---- audio block state, much of which carries over from block to block within a frame
	bool blockSwitch[MAX_CHANNELS]{};
	bool dither[MAX_CHANNELS]{};
	float dynamicRange[2]{1.0f, 1.0f};

	bool couplingInUse = false;
	bool inCoupling[MAX_CHANNELS]{};
	bool phaseFlagsInUse = false;
	int couplingBegin = 0; // cplbegf
	int couplingSubBands = 0; // ncplsubnd
	int couplingStart = 0; // first bin
	int couplingEnd = 0; // bin after the last
	// Coupling band of each sub-band, relative to cplbegf
	int couplingBandOfSubBand[COUPLING_SUB_BANDS]{};
	int couplingBands = 0;
	// Coupling coordinates by sub-band, relative to cplbegf, including the factor of 8 (clause 6.4.3)
	float couplingCoordinate[MAX_CHANNELS][COUPLING_SUB_BANDS]{};
	bool couplingCoordinatesValid[MAX_CHANNELS]{};
	bool phaseFlag[COUPLING_SUB_BANDS]{};

	bool rematrixFlag[4]{};

	// Exponents and bit allocation pointers, by channel, CPL and LFE
	uint8_t exponents[7][256]{};
	uint8_t bap[7][256]{};
	int endBin[7]{};
	// What each channel's exponents were sent with, for when they are reused
	bool exponentsCoupled[MAX_CHANNELS]{};
	int couplingExponentStart = 0;

	// Bit allocation parameters
	int slowDecay = 0, fastDecay = 0, slowGain = 0, dbPerBit = 0, floor = 0;
	int coarseSnrOffset = 0;
	int fineSnrOffset[7]{};
	int fastGain[7]{};
	int couplingFastLeak = 0, couplingSlowLeak = 0;

	// Delta bit allocation, by channel and CPL (5)
	struct Delta {
		bool active = false;
		int segments = 0;
		uint8_t offset[DELTA_SEGMENTS]{};
		uint8_t length[DELTA_SEGMENTS]{};
		uint8_t value[DELTA_SEGMENTS]{};
	};

	Delta delta[6];

	// Transform coefficients by channel (LFE at its index), and the coupling channel's
	float coefficients[7][256]{};

	// Mantissas of the grouped quantizers waiting to be used
	struct Groups {
		float bap1[3]{};
		int bap1Left = 0;
		float bap2[3]{};
		int bap2Left = 0;
		float bap4[2]{};
		int bap4Left = 0;
	} groups;

	// Uniform in [-0.707, 0.707) (clause 6.3.4)
	float nextDither() {
		ditherState = ditherState * 1664525u + 1013904223u;
		return static_cast<float>(static_cast<int32_t>(ditherState)) * (0.707f / 2147483648.0f);
	}

	// ---- frame

	// Decodes the frame in `frame` into left/right, outputChannels of them. Throws FrameError.
	void decodeFrame(uint32_t frameBytes, uint32_t outputChannels, int &block);

	void decodeBlock(BitReader &bits, int block);
	void decodeExponents(BitReader &bits, int strategy, int groups, int absolute, uint8_t *exponents);
	void allocateBits(int channel, int start, int end, int fastGainValue, int snrOffset);
	void readMantissas(BitReader &bits, int channel, int start, int end, bool dither);
	void decouple(int channel);
	void downmix(const float pcm[MAX_CHANNELS][BLOCK_SAMPLES], uint32_t outputChannels, float *left, float *right) const;
};

void A52AudioDecoder::Decoder::decodeFrame(const uint32_t frameBytes, const uint32_t outputChannels, int &block) {
	BitReader bits(frame, frameBytes);
	// syncinfo: syncword, crc1, fscod, frmsizecod
	bits.skip(32);
	fscod = static_cast<int>(bits.get(2));
	bits.skip(6);

	// bsi (clause 4.3.2)
	const auto bsid = bits.get(5);
	if (bsid > 8) {
		throw FrameError();
	}
	bits.skip(3); // bsmod
	acmod = static_cast<int>(bits.get(3));
	channels = FULL_CHANNELS[acmod];
	centreMix = 0;
	surroundMix = 0;
	if ((acmod & 1) && acmod != 1) {
		centreMix = CENTRE_MIX_LEVEL[bits.get(2)];
	}
	if (acmod & 4) {
		surroundMix = SURROUND_MIX_LEVEL[bits.get(2)];
	}
	if (acmod == 2) {
		bits.skip(2); // dsurmod
	}
	lfeOn = bits.get(1);
	for (int i = 0, programmes = acmod == 0 ? 2 : 1; i < programmes; i++) {
		bits.skip(5); // dialnorm
		if (bits.get(1)) bits.skip(8); // compr
		if (bits.get(1)) bits.skip(8); // langcod
		if (bits.get(1)) bits.skip(7); // mixlevel, roomtyp
	}
	bits.skip(2); // copyrightb, origbs
	if (bits.get(1)) bits.skip(14); // timecod1
	if (bits.get(1)) bits.skip(14); // timecod2
	if (bits.get(1)) {
		bits.skip((bits.get(6) + 1) * 8); // addbsi
	}

	if (acmod != lastAcmod) {
		std::memset(delay, 0, sizeof(delay));
		lastAcmod = acmod;
	}

	// Values that don't carry over from the previous frame
	couplingInUse = false;
	std::memset(inCoupling, 0, sizeof(inCoupling));
	std::memset(couplingCoordinatesValid, 0, sizeof(couplingCoordinatesValid));
	std::memset(phaseFlag, 0, sizeof(phaseFlag));
	for (auto &d: delta) {
		d.active = false;
	}

	const InverseTransform &transform = inverseTransform();
	for (block = 0; block < 6; block++) {
		decodeBlock(bits, block);

		float pcm[MAX_CHANNELS][BLOCK_SAMPLES];
		for (int ch = 0; ch < channels; ch++) {
			transform.run(coefficients[ch], blockSwitch[ch], delay[ch], pcm[ch]);
		}
		downmix(pcm, outputChannels, left + block * BLOCK_SAMPLES, right + block * BLOCK_SAMPLES);
	}
}

void A52AudioDecoder::Decoder::decodeExponents(BitReader &bits, const int strategy, const int groups, const int absolute,
                                               uint8_t *exponents) {
	const int groupSize = 1 << (strategy - 1);
	int exponent = absolute;
	for (int group = 0; group < groups; group++) {
		const int code = static_cast<int>(bits.get(7));
		if (code >= 125) {
			throw FrameError();
		}
		const int differences[3] = {code / 25 - 2, code % 25 / 5 - 2, code % 5 - 2};
		for (const int difference: differences) {
			exponent += difference;
			if (exponent < 0 || exponent > 24) {
				throw FrameError();
			}
			for (int i = 0; i < groupSize; i++) {
				*exponents++ = static_cast<uint8_t>(exponent);
			}
		}
	}
}

// Parametric bit allocation for one channel's bins start..end - 1 (clause 6.2.2)
void A52AudioDecoder::Decoder::allocateBits(const int channel, const int start, const int end, const int fastGainValue,
                                            const int snrOffset) {
	const uint8_t *exponent = exponents[channel];
	int psd[256];
	int bandPsd[BANDS + 1]{};
	int excite[BANDS]{};
	int mask[BANDS]{};

	for (int bin = start; bin < end; bin++) {
		psd[bin] = 3072 - (exponent[bin] << 7);
	}

	// Integrate the psd over each band with log-addition
	{
		int j = start;
		int k = BAND_OF_BIN.values[start];
		int lastBin;
		do {
			lastBin = std::min<int>(BAND_START[k + 1], end);
			bandPsd[k] = psd[j++];
			for (; j < lastBin; j++) {
				const int difference = bandPsd[k] - psd[j];
				const int address = std::min(std::abs(difference) >> 1, 255);
				bandPsd[k] = (difference >= 0 ? bandPsd[k] : psd[j]) + LOG_ADD[address];
			}
			k++;
		} while (end > lastBin);
	}

	// Excitation function
	const int bandStart = BAND_OF_BIN.values[start];
	const int bandEnd = BAND_OF_BIN.values[end - 1] + 1;
	auto lowCompensation = [](int a, const int b0, const int b1, const int bin) {
		if (bin < 7) {
			if (b0 + 256 == b1) {
				a = 384;
			} else if (b0 > b1) {
				a = std::max(0, a - 64);
			}
		} else if (bin < 20) {
			if (b0 + 256 == b1) {
				a = 320;
			} else if (b0 > b1) {
				a = std::max(0, a - 64);
			}
		} else {
			a = std::max(0, a - 128);
		}
		return a;
	};
	int fastLeak = 0;
	int slowLeak = 0;
	int begin;
	if (bandStart == 0) {
		// Full bandwidth and low frequency effects channels. The last band of the LFE channel (bin 6) has no
		// next band to compare with.
		const bool lfe = bandEnd == 7;
		int lowComp = lowCompensation(0, bandPsd[0], bandPsd[1], 0);
		excite[0] = bandPsd[0] - fastGainValue - lowComp;
		lowComp = lowCompensation(lowComp, bandPsd[1], bandPsd[2], 1);
		excite[1] = bandPsd[1] - fastGainValue - lowComp;
		begin = 7;
		for (int bin = 2; bin < 7; bin++) {
			if (!lfe || bin != 6) {
				lowComp = lowCompensation(lowComp, bandPsd[bin], bandPsd[bin + 1], bin);
			}
			fastLeak = bandPsd[bin] - fastGainValue;
			slowLeak = bandPsd[bin] - slowGain;
			excite[bin] = fastLeak - lowComp;
			if ((!lfe || bin != 6) && bandPsd[bin] <= bandPsd[bin + 1]) {
				begin = bin + 1;
				break;
			}
		}
		for (int bin = begin; bin < std::min(bandEnd, 22); bin++) {
			if (!lfe || bin != 6) {
				lowComp = lowCompensation(lowComp, bandPsd[bin], bandPsd[bin + 1], bin);
			}
			fastLeak = std::max(fastLeak - fastDecay, bandPsd[bin] - fastGainValue);
			slowLeak = std::max(slowLeak - slowDecay, bandPsd[bin] - slowGain);
			excite[bin] = std::max(fastLeak - lowComp, slowLeak);
		}
		begin = 22;
	} else {
		// Coupling channel
		begin = bandStart;
		fastLeak = (couplingFastLeak << 8) + 768;
		slowLeak = (couplingSlowLeak << 8) + 768;
	}
	for (int bin = begin; bin < bandEnd; bin++) {
		fastLeak = std::max(fastLeak - fastDecay, bandPsd[bin] - fastGainValue);
		slowLeak = std::max(slowLeak - slowDecay, bandPsd[bin] - slowGain);
		excite[bin] = std::max(fastLeak, slowLeak);
	}

	// Masking curve
	for (int bin = bandStart; bin < bandEnd; bin++) {
		if (bandPsd[bin] < dbPerBit) {
			excite[bin] += (dbPerBit - bandPsd[bin]) >> 2;
		}
		mask[bin] = std::max<int>(excite[bin], HEARING_THRESHOLD[fscod][bin]);
	}

	// Delta bit allocation, which the LFE channel doesn't have
	if (channel != LFE && delta[channel].active) {
		const Delta &d = delta[channel];
		int band = 0;
		for (int segment = 0; segment < d.segments; segment++) {
			band += d.offset[segment];
			const int value = d.value[segment] >= 4 ? (d.value[segment] - 3) << 7 : (d.value[segment] - 4) * 128;
			for (int k = 0; k < d.length[segment]; k++) {
				if (band >= BANDS) {
					throw FrameError();
				}
				mask[band++] += value;
			}
		}
	}

	// Bit allocation pointers
	uint8_t *bapOut = bap[channel];
	int i = start;
	int j = BAND_OF_BIN.values[start];
	int lastBin;
	do {
		lastBin = std::min<int>(BAND_START[j + 1], end);
		int m = mask[j] - snrOffset - floor;
		if (m < 0) {
			m = 0;
		}
		m = (m & 0x1fe0) + floor;
		for (; i < lastBin; i++) {
			const int address = std::clamp((psd[i] - m) >> 5, 0, 63);
			bapOut[i] = BAP_TABLE[address];
		}
		j++;
	} while (end > lastBin);
}

// Reads and dequantizes the mantissas of bins start..end - 1 into coefficients[channel] (clause 6.3)
void A52AudioDecoder::Decoder::readMantissas(BitReader &bits, const int channel, const int start, const int end,
                                             const bool useDither) {
	const uint8_t *exponent = exponents[channel];
	const uint8_t *bapIn = bap[channel];
	float *out = coefficients[channel];
	for (int bin = start; bin < end; bin++) {
		float mantissa;
		const int b = bapIn[bin];
		switch (b) {
			case 0:
				// The coupling channel's dither is added for each coupled channel when decoupling
				mantissa = useDither ? nextDither() : 0.0f;
				break;
			case 1:
				if (!groups.bap1Left) {
					const int code = static_cast<int>(bits.get(5));
					if (code >= 27) {
						throw FrameError();
					}
					const int codes[3] = {code / 9, code % 9 / 3, code % 3};
					for (int i = 0; i < 3; i++) {
						groups.bap1[i] = static_cast<float>(codes[i] - 1) * (2.0f / 3.0f);
					}
					groups.bap1Left = 3;
				}
				mantissa = groups.bap1[3 - groups.bap1Left--];
				break;
			case 2:
				if (!groups.bap2Left) {
					const int code = static_cast<int>(bits.get(7));
					if (code >= 125) {
						throw FrameError();
					}
					const int codes[3] = {code / 25, code % 25 / 5, code % 5};
					for (int i = 0; i < 3; i++) {
						groups.bap2[i] = static_cast<float>(codes[i] - 2) * (2.0f / 5.0f);
					}
					groups.bap2Left = 3;
				}
				mantissa = groups.bap2[3 - groups.bap2Left--];
				break;
			case 4:
				if (!groups.bap4Left) {
					const int code = static_cast<int>(bits.get(7));
					if (code >= 121) {
						throw FrameError();
					}
					groups.bap4[0] = static_cast<float>(code / 11 - 5) * (2.0f / 11.0f);
					groups.bap4[1] = static_cast<float>(code % 11 - 5) * (2.0f / 11.0f);
					groups.bap4Left = 2;
				}
				mantissa = groups.bap4[2 - groups.bap4Left--];
				break;
			case 3:
			case 5: {
				// Symmetric: 7 or 15 levels
				const int levels = b == 3 ? 7 : 15;
				const int code = static_cast<int>(bits.get(MANTISSA_BITS[b]));
				if (code >= levels) {
					throw FrameError();
				}
				mantissa = static_cast<float>(2 * code - (levels - 1)) / static_cast<float>(levels);
				break;
			}
			default: {
				// Asymmetric: two's complement fraction
				const int bitCount = MANTISSA_BITS[b];
				const int code = static_cast<int>(bits.get(bitCount));
				const int value = code >= 1 << (bitCount - 1) ? code - (1 << bitCount) : code;
				mantissa = static_cast<float>(value) * powerOfTwo(1 - bitCount);
				break;
			}
		}
		out[bin] = mantissa * powerOfTwo(-exponent[bin]);
	}
}

// Rebuilds a coupled channel's coefficients from the coupling channel (clause 6.4.3)
void A52AudioDecoder::Decoder::decouple(const int channel) {
	float *out = coefficients[channel];
	const float *coupled = coefficients[CPL];
	const bool invert = acmod == 2 && channel == 1;
	for (int bin = couplingStart; bin < couplingEnd; bin++) {
		const int subBand = (bin - couplingStart) / 12;
		float coordinate = couplingCoordinate[channel][subBand];
		if (invert && phaseFlag[subBand]) {
			coordinate = -coordinate;
		}
		float value = coupled[bin];
		if (bap[CPL][bin] == 0) {
			// Dither is added for each channel separately, so it isn't correlated between them
			value = dither[channel] ? nextDither() * powerOfTwo(-exponents[CPL][bin]) : 0.0f;
		}
		out[bin] = value * coordinate;
	}
}

void A52AudioDecoder::Decoder::decodeBlock(BitReader &bits, const int block) {
	const bool first = block == 0;

	for (int ch = 0; ch < channels; ch++) {
		blockSwitch[ch] = bits.get(1);
	}
	for (int ch = 0; ch < channels; ch++) {
		dither[ch] = bits.get(1);
	}

	// Dynamic range gain words (clause 6.7.2): XXX.YYYYY, gain 2^(X + 1) * 0.1YYYYY in binary
	for (int i = 0, programmes = acmod == 0 ? 2 : 1; i < programmes; i++) {
		if (bits.get(1)) {
			const int code = static_cast<int>(bits.get(8));
			const int shift = (code >> 5) - ((code & 0x80) ? 8 : 0) + 1;
			dynamicRange[i] = static_cast<float>(32 + (code & 31)) * powerOfTwo(shift - 6);
		} else if (first) {
			dynamicRange[i] = 1.0f;
		}
	}
	if (acmod != 0) {
		dynamicRange[1] = dynamicRange[0];
	}

	// Coupling strategy
	bool couplingJustEnabled = false;
	if (bits.get(1)) {
		const bool wasInUse = couplingInUse;
		couplingInUse = bits.get(1);
		if (couplingInUse) {
			int coupledChannels = 0;
			for (int ch = 0; ch < channels; ch++) {
				const bool coupled = bits.get(1);
				if (coupled && !inCoupling[ch]) {
					couplingCoordinatesValid[ch] = false;
				}
				inCoupling[ch] = coupled;
				coupledChannels += coupled;
			}
			if (coupledChannels < 2) {
				throw FrameError();
			}
			phaseFlagsInUse = acmod == 2 && bits.get(1);
			couplingBegin = static_cast<int>(bits.get(4));
			const int couplingEndCode = static_cast<int>(bits.get(4));
			couplingSubBands = 3 + couplingEndCode - couplingBegin;
			if (couplingSubBands < 1) {
				throw FrameError();
			}
			couplingStart = couplingBegin * 12 + 37;
			couplingEnd = (couplingEndCode + 3) * 12 + 37;
			couplingBands = 1;
			couplingBandOfSubBand[0] = 0;
			for (int subBand = 1; subBand < couplingSubBands; subBand++) {
				if (!bits.get(1)) {
					couplingBands++;
				}
				couplingBandOfSubBand[subBand] = couplingBands - 1;
			}
			couplingJustEnabled = !wasInUse;
		} else {
			std::memset(inCoupling, 0, sizeof(inCoupling));
		}
	} else if (first) {
		throw FrameError();
	}

	// Coupling coordinates and phase flags
	if (couplingInUse) {
		bool anyCoordinates = false;
		for (int ch = 0; ch < channels; ch++) {
			if (!inCoupling[ch]) {
				continue;
			}
			if (bits.get(1)) {
				anyCoordinates = true;
				const int master = static_cast<int>(bits.get(2)) * 3;
				float bandCoordinate[COUPLING_SUB_BANDS];
				for (int band = 0; band < couplingBands; band++) {
					const int exponent = static_cast<int>(bits.get(4));
					const int mantissa = static_cast<int>(bits.get(4));
					const float value = exponent == 15 ? mantissa / 16.0f : (mantissa + 16) / 32.0f;
					bandCoordinate[band] = value * powerOfTwo(3 - exponent - master);
				}
				for (int subBand = 0; subBand < couplingSubBands; subBand++) {
					couplingCoordinate[ch][subBand] = bandCoordinate[couplingBandOfSubBand[subBand]];
				}
				couplingCoordinatesValid[ch] = true;
			} else if (!couplingCoordinatesValid[ch]) {
				throw FrameError();
			}
		}
		if (acmod == 2 && phaseFlagsInUse && anyCoordinates) {
			bool bandFlag[COUPLING_SUB_BANDS];
			for (int band = 0; band < couplingBands; band++) {
				bandFlag[band] = bits.get(1);
			}
			for (int subBand = 0; subBand < couplingSubBands; subBand++) {
				phaseFlag[subBand] = bandFlag[couplingBandOfSubBand[subBand]];
			}
		} else if (!phaseFlagsInUse) {
			std::memset(phaseFlag, 0, sizeof(phaseFlag));
		}
	}

	// Rematrixing flags
	int rematrixBands = 0;
	if (acmod == 2) {
		rematrixBands = !couplingInUse || couplingBegin > 2 ? 4 : couplingBegin > 0 ? 3 : 2;
		if (bits.get(1)) {
			for (int band = 0; band < rematrixBands; band++) {
				rematrixFlag[band] = bits.get(1);
			}
		} else if (first) {
			throw FrameError();
		}
	}

	// Exponent strategies
	const int couplingStrategy = couplingInUse ? static_cast<int>(bits.get(2)) : 0;
	int strategy[MAX_CHANNELS];
	for (int ch = 0; ch < channels; ch++) {
		strategy[ch] = static_cast<int>(bits.get(2));
	}
	const int lfeStrategy = lfeOn ? static_cast<int>(bits.get(1)) : 0;
	if (couplingInUse && !couplingStrategy
	    && (first || couplingJustEnabled || couplingExponentStart != couplingStart || endBin[CPL] != couplingEnd)) {
		throw FrameError();
	}
	if (lfeOn && first && !lfeStrategy) {
		throw FrameError();
	}
	for (int ch = 0; ch < channels; ch++) {
		if (strategy[ch]) {
			if (inCoupling[ch]) {
				endBin[ch] = couplingStart;
			} else {
				const int bandwidth = static_cast<int>(bits.get(6));
				if (bandwidth > 60) {
					throw FrameError();
				}
				endBin[ch] = (bandwidth + 12) * 3 + 37;
			}
			exponentsCoupled[ch] = inCoupling[ch];
		} else if (first || exponentsCoupled[ch] != inCoupling[ch]
		           || (inCoupling[ch] && endBin[ch] != couplingStart)) {
			throw FrameError();
		}
	}

	// Exponents
	if (couplingInUse && couplingStrategy) {
		const int absolute = static_cast<int>(bits.get(4)) << 1;
		const int groups = (couplingEnd - couplingStart) / (3 << (couplingStrategy - 1));
		decodeExponents(bits, couplingStrategy, groups, absolute, &exponents[CPL][couplingStart]);
		couplingExponentStart = couplingStart;
		endBin[CPL] = couplingEnd;
	}
	for (int ch = 0; ch < channels; ch++) {
		if (strategy[ch]) {
			exponents[ch][0] = static_cast<uint8_t>(bits.get(4));
			const int groupSize = 3 << (strategy[ch] - 1);
			const int groups = (endBin[ch] - 1 + groupSize - 3) / groupSize;
			decodeExponents(bits, strategy[ch], groups, exponents[ch][0], &exponents[ch][1]);
			bits.skip(2); // gainrng
		}
	}
	if (lfeOn && lfeStrategy) {
		exponents[LFE][0] = static_cast<uint8_t>(bits.get(4));
		decodeExponents(bits, 1, 2, exponents[LFE][0], &exponents[LFE][1]);
		endBin[LFE] = 7;
	}

	// Bit allocation parameters
	if (bits.get(1)) {
		slowDecay = SLOW_DECAY[bits.get(2)];
		fastDecay = FAST_DECAY[bits.get(2)];
		slowGain = SLOW_GAIN[bits.get(2)];
		dbPerBit = DB_PER_BIT[bits.get(2)];
		floor = FLOOR[bits.get(3)];
	} else if (first) {
		throw FrameError();
	}
	if (bits.get(1)) {
		coarseSnrOffset = static_cast<int>(bits.get(6));
		if (couplingInUse) {
			fineSnrOffset[CPL] = static_cast<int>(bits.get(4));
			fastGain[CPL] = FAST_GAIN[bits.get(3)];
		}
		for (int ch = 0; ch < channels; ch++) {
			fineSnrOffset[ch] = static_cast<int>(bits.get(4));
			fastGain[ch] = FAST_GAIN[bits.get(3)];
		}
		if (lfeOn) {
			fineSnrOffset[LFE] = static_cast<int>(bits.get(4));
			fastGain[LFE] = FAST_GAIN[bits.get(3)];
		}
	} else if (first) {
		throw FrameError();
	}
	if (couplingInUse) {
		if (bits.get(1)) {
			couplingFastLeak = static_cast<int>(bits.get(3));
			couplingSlowLeak = static_cast<int>(bits.get(3));
		} else if (first || couplingJustEnabled) {
			throw FrameError();
		}
	}

	// Delta bit allocation
	if (bits.get(1)) {
		int deltaStrategy[6]{};
		if (couplingInUse) {
			deltaStrategy[CPL] = static_cast<int>(bits.get(2));
		}
		for (int ch = 0; ch < channels; ch++) {
			deltaStrategy[ch] = static_cast<int>(bits.get(2));
		}
		auto readDelta = [&bits, this](const int index, const int deltaStrategyValue) {
			Delta &d = delta[index];
			switch (deltaStrategyValue) {
				case 0: // reuse
					break;
				case 1:
					d.active = true;
					d.segments = static_cast<int>(bits.get(3)) + 1;
					for (int segment = 0; segment < d.segments; segment++) {
						d.offset[segment] = static_cast<uint8_t>(bits.get(5));
						d.length[segment] = static_cast<uint8_t>(bits.get(4));
						d.value[segment] = static_cast<uint8_t>(bits.get(3));
					}
					break;
				case 2:
					d.active = false;
					break;
				default:
					throw FrameError();
			}
		};
		if (couplingInUse) {
			readDelta(CPL, deltaStrategy[CPL]);
		}
		for (int ch = 0; ch < channels; ch++) {
			readDelta(ch, deltaStrategy[ch]);
		}
	}

	// Dummy data
	if (bits.get(1)) {
		bits.skip(bits.get(9) * 8);
	}

	// Bit allocation. All SNR offsets zero means no bits for any mantissa.
	bool allZero = coarseSnrOffset == 0;
	for (int ch = 0; ch < channels && allZero; ch++) {
		allZero = fineSnrOffset[ch] == 0;
	}
	allZero = allZero && (!couplingInUse || fineSnrOffset[CPL] == 0) && (!lfeOn || fineSnrOffset[LFE] == 0);
	auto snrOffset = [this](const int index) { return ((coarseSnrOffset - 15) * 16 + fineSnrOffset[index]) * 4; };
	if (allZero) {
		std::memset(bap, 0, sizeof(bap));
	} else {
		for (int ch = 0; ch < channels; ch++) {
			allocateBits(ch, 0, endBin[ch], fastGain[ch], snrOffset(ch));
		}
		if (couplingInUse) {
			allocateBits(CPL, couplingStart, couplingEnd, fastGain[CPL], snrOffset(CPL));
		}
		if (lfeOn) {
			allocateBits(LFE, 0, 7, fastGain[LFE], snrOffset(LFE));
		}
	}

	// Mantissas. The coupling channel's follow those of the first coupled channel.
	groups = Groups();
	bool gotCoupling = false;
	for (int ch = 0; ch < channels; ch++) {
		readMantissas(bits, ch, 0, endBin[ch], dither[ch]);
		if (couplingInUse && inCoupling[ch] && !gotCoupling) {
			readMantissas(bits, CPL, couplingStart, couplingEnd, false);
			gotCoupling = true;
		}
	}
	if (lfeOn) {
		// Read to keep the position, the LFE channel isn't output
		readMantissas(bits, LFE, 0, 7, false);
	}

	for (int ch = 0; ch < channels; ch++) {
		int end = endBin[ch];
		if (couplingInUse && inCoupling[ch]) {
			decouple(ch);
			end = couplingEnd;
		}
		std::fill(coefficients[ch] + end, coefficients[ch] + 256, 0.0f);
	}

	// Rematrixing (clause 6.5.4), up to the lower bandwidth of the two channels
	if (acmod == 2) {
		const int limit = std::min(couplingInUse ? couplingStart : 253, std::min(endBin[0], endBin[1]));
		for (int band = 0; band < rematrixBands; band++) {
			if (!rematrixFlag[band]) {
				continue;
			}
			const int end = std::min(REMATRIX_BAND_START[band + 1], limit);
			for (int bin = REMATRIX_BAND_START[band]; bin < end; bin++) {
				const float l = coefficients[0][bin];
				const float r = coefficients[1][bin];
				coefficients[0][bin] = l + r;
				coefficients[1][bin] = l - r;
			}
		}
	}

	// Dynamic range compression
	for (int ch = 0; ch < channels; ch++) {
		const float gain = dynamicRange[acmod == 0 && ch == 1 ? 1 : 0];
		if (gain != 1.0f) {
			for (int bin = 0; bin < 256; bin++) {
				coefficients[ch][bin] *= gain;
			}
		}
	}
}

// Downmixes one block to the output channels: L0/R0 (clause 6.8.2), scaled so a channel can't exceed full scale
void A52AudioDecoder::Decoder::downmix(const float pcm[MAX_CHANNELS][BLOCK_SAMPLES], const uint32_t outputChannels,
                                       float *left, float *right) const {
	float toLeft[MAX_CHANNELS]{};
	float toRight[MAX_CHANNELS]{};
	const float c = centreMix;
	const float s = surroundMix;
	constexpr float HALF_POWER = 0.7071068f;
	switch (acmod) {
		case 0: // Ch1, Ch2: two independent programmes, played as left and right
		case 2: // L, R
			toLeft[0] = 1;
			toRight[1] = 1;
			break;
		case 1: // C
			toLeft[0] = toRight[0] = 1;
			break;
		case 3: // L, C, R
			toLeft[0] = 1;
			toLeft[1] = toRight[1] = c;
			toRight[2] = 1;
			break;
		case 4: // L, R, S
			toLeft[0] = 1;
			toRight[1] = 1;
			toLeft[2] = toRight[2] = HALF_POWER * s;
			break;
		case 5: // L, C, R, S
			toLeft[0] = 1;
			toLeft[1] = toRight[1] = c;
			toRight[2] = 1;
			toLeft[3] = toRight[3] = HALF_POWER * s;
			break;
		case 6: // L, R, Ls, Rs
			toLeft[0] = 1;
			toRight[1] = 1;
			toLeft[2] = s;
			toRight[3] = s;
			break;
		default: // L, C, R, Ls, Rs
			toLeft[0] = 1;
			toLeft[1] = toRight[1] = c;
			toRight[2] = 1;
			toLeft[3] = s;
			toRight[4] = s;
			break;
	}
	float sumLeft = 0, sumRight = 0;
	for (int ch = 0; ch < channels; ch++) {
		sumLeft += toLeft[ch];
		sumRight += toRight[ch];
	}
	const float scale = 1.0f / std::max(1.0f, std::max(sumLeft, sumRight));
	if (outputChannels == 1) {
		// The average of the stereo downmix, which for a mono stream is the stream as it is
		for (int ch = 0; ch < channels; ch++) {
			toLeft[ch] = (toLeft[ch] + toRight[ch]) * scale * 0.5f;
		}
	} else {
		for (int ch = 0; ch < channels; ch++) {
			toLeft[ch] *= scale;
			toRight[ch] *= scale;
		}
	}
	for (int n = 0; n < BLOCK_SAMPLES; n++) {
		float l = 0, r = 0;
		for (int ch = 0; ch < channels; ch++) {
			l += toLeft[ch] * pcm[ch][n];
			r += toRight[ch] * pcm[ch][n];
		}
		left[n] = l;
		right[n] = r;
	}
}

// ---------------------------------------------------------------- stream handling

A52AudioDecoder::A52AudioDecoder()
	: m_decoder(std::make_unique<Decoder>()) {
	// Decoding starts once requiredBytes() are buffered, not at an exact size, as a header search can leave more
	m_stream.setSizeRequired(UINT32_MAX);
}

A52AudioDecoder::~A52AudioDecoder() = default;

void A52AudioDecoder::setFrameCallback(const FrameCallback &frameCallback) {
	m_frameCallback = frameCallback;
}

int32_t A52AudioDecoder::getPts(const int32_t frameTimeMillis) {
	return m_stream.getPts(frameTimeMillis);
}

void A52AudioDecoder::setPts(const int32_t ptsMillis) {
	m_stream.setPts(ptsMillis);
}

void A52AudioDecoder::writeByte(const uint8_t data) {
	m_stream.writeByte(data);
}

bool A52AudioDecoder::isFull() {
	return m_stream.used() >= requiredBytes() || m_stream.isFull();
}

void A52AudioDecoder::executeCallbacks() {
	decode();
}

void A52AudioDecoder::flush() {
	// Frames are decoded as soon as they are complete
}

void A52AudioDecoder::discontinuity() {
	if (m_state == State::Frame && hasPendingFrame()) {
		return; // a complete frame waiting for output space is kept
	}
	m_stream.skip(m_stream.used());
	m_state = State::Header;
	m_resyncSkippedBytes = 0;
	m_expectResync = true;
}

bool A52AudioDecoder::hasPendingFrame() {
	return m_state == State::Frame && m_stream.used() >= m_frameBytes;
}

void A52AudioDecoder::reset() {
	m_stream.clear();
	m_state = State::Header;
	m_firstFramePtsMillis = -1;
	m_resyncSkippedBytes = 0;
	m_expectResync = true;
	std::memset(m_decoder->delay, 0, sizeof(m_decoder->delay));
}

void A52AudioDecoder::decode() {
	while (m_stream.used() >= requiredBytes()) {
		if (m_state == State::Header) {
			decodeHeader();
		} else if (!decodeBufferedFrame()) {
			return; // waiting for space in the sound buffer
		}
	}
}

uint32_t A52AudioDecoder::validHeaderFrameBytes() {
	if ((m_stream.peekAt(0) << 8 | m_stream.peekAt(1)) != SYNC_WORD) {
		return 0;
	}
	const uint32_t fscod = m_stream.peekAt(4) >> 6;
	const uint32_t bytes = frameBytes(fscod, m_stream.peekAt(4) & 63);
	if (!bytes) {
		return 0;
	}
	// Versions above 8 use syntax this decoder doesn't know
	if ((m_stream.peekAt(5) >> 3) > 8) {
		if (m_outputChannels == 0 && ++m_unsupportedHeaderCount == 3) {
			m_audioDisabled = true;
			std::cout << "Unsupported A/52 audio version. Audio disabled." << std::endl;
		}
		return 0;
	}
	// The audio output is opened for the first frame's sample rate, so it can't change
	if (m_outputChannels && fscod != m_sampleRateCode) {
		return 0;
	}
	return bytes;
}

void A52AudioDecoder::decodeHeader() {
	if (m_audioDisabled) {
		m_stream.skip(m_stream.used());
		return;
	}
	// Search byte by byte for a valid header
	m_frameBytes = validHeaderFrameBytes();
	if (!m_frameBytes) {
		m_stream.skip(1);
		m_resyncSkippedBytes++;
		return;
	}
	if (m_resyncSkippedBytes && !m_expectResync) {
		std::cout << "Corrupt audio, skipped " << m_resyncSkippedBytes << " bytes" << std::endl;
	}
	m_resyncSkippedBytes = 0;
	m_expectResync = false;

	// The audio clock is anchored on the first frame's PTS; drop frames until one is known
	m_discardFrame = false;
	if (m_firstFramePtsMillis < 0) {
		if (m_stream.getPts(0) < 0) {
			m_discardFrame = true;
		} else {
			m_firstFramePtsMillis = m_stream.getPts(0);
		}
	}
	m_state = State::Frame;
}

bool A52AudioDecoder::bufferedFrameCrcValid() {
	// The CRC of everything after the sync word is 0 for a valid frame (clause 6.10.1)
	uint32_t crc = 0;
	for (uint32_t i = 2; i < m_frameBytes; i++) {
		crc = (crc << 8 ^ CRC_TABLE.values[(crc >> 8) ^ m_stream.peekAt(i)]) & 0xFFFF;
	}
	return crc == 0;
}

bool A52AudioDecoder::decodeBufferedFrame() {
	if (!bufferedFrameCrcValid()) {
		// Not a frame, or a damaged one: look for the next header from the byte after this one's sync word. The
		// skipped bytes are reported when a valid frame is found.
		m_stream.skip(1);
		m_resyncSkippedBytes = 1;
		m_state = State::Header;
		return true;
	}

	if (!m_discardFrame) {
		const uint32_t fscod = m_stream.peekAt(4) >> 6;
		const uint32_t acmod = m_stream.peekAt(6) >> 5;
		const uint32_t channels = m_outputChannels ? m_outputChannels : MD_MPEG_AUDIO_MONO || acmod == 1 ? 1 : 2;
		// Backpressure: if the output can't take a whole frame, leave the frame's bytes buffered. isFull() stays
		// true, so the demuxer stops feeding us and retries via executeCallbacks().
		if (m_soundBuffer.free() < SAMPLES_PER_FRAME * channels) {
			return false;
		}
		if (!m_outputChannels) {
			m_outputChannels = channels;
			m_sampleRateCode = fscod;
			m_sampleRate = SAMPLE_RATE[fscod];
		}
	}

	for (uint32_t i = 0; i < m_frameBytes; i++) {
		m_decoder->frame[i] = m_stream.nextByte();
	}
	std::memset(m_decoder->frame + m_frameBytes, 0, 4);
	m_state = State::Header;
	if (m_discardFrame) {
		return true;
	}

	int block = 0;
	try {
		m_decoder->decodeFrame(m_frameBytes, m_outputChannels, block);
	} catch (const FrameError &) {
		// Keeps the timing: the rest of the frame is silent
		std::cout << "Corrupt audio frame, muting it" << std::endl;
		std::fill(m_decoder->left + block * BLOCK_SAMPLES, m_decoder->left + SAMPLES_PER_FRAME, 0.0f);
		std::fill(m_decoder->right + block * BLOCK_SAMPLES, m_decoder->right + SAMPLES_PER_FRAME, 0.0f);
		std::memset(m_decoder->delay, 0, sizeof(m_decoder->delay));
	}
	outputFrame(m_decoder->left, m_decoder->right);
	return true;
}

static AudioSample toAudioSample(const float value) {
#if MD_MPEG_AUDIO_FIXED_POINT
	const float scaled = std::nearbyint(value * 32768.0f);
	return static_cast<AudioSample>(std::clamp(scaled, -32768.0f, 32767.0f));
#else
	return value;
#endif
}

void A52AudioDecoder::outputFrame(const float *left, const float *right) {
	if (m_outputChannels == 1) {
		for (uint32_t i = 0; i < SAMPLES_PER_FRAME; i++) {
			m_soundBuffer.writeByte(toAudioSample(left[i]));
		}
	} else {
		// Blocks of 32 left samples, then 32 right
		for (uint32_t start = 0; start < SAMPLES_PER_FRAME; start += 32) {
			for (uint32_t i = start; i < start + 32; i++) {
				m_soundBuffer.writeByte(toAudioSample(left[i]));
			}
			for (uint32_t i = start; i < start + 32; i++) {
				m_soundBuffer.writeByte(toAudioSample(right[i]));
			}
		}
	}
	if (m_frameCallback) {
		m_frameCallback(m_sampleRate, m_outputChannels, &m_soundBuffer);
	}
}

#endif
